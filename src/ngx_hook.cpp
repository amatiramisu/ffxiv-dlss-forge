#include "ngx_hook.h"
#include "log.h"
#include "motion_vectors.h"
#include "nr_bridge.h"
#include "settings.h"
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

using namespace ngx;

namespace
{
	struct lock_guard
	{
		explicit lock_guard(SRWLOCK &l) : _l(l) { AcquireSRWLockExclusive(&_l); }
		~lock_guard() { ReleaseSRWLockExclusive(&_l); }
		SRWLOCK &_l;
	};

	struct real_table
	{
		std::atomic<PFN_Init> init { nullptr };
		std::atomic<PFN_Init_Ext> init_ext { nullptr };
		std::atomic<PFN_Init_ProjectID> init_project_id { nullptr };
		std::atomic<PFN_GetParams> get_capability_parameters { nullptr };
		std::atomic<PFN_GetParams> get_parameters { nullptr };
		std::atomic<PFN_GetParams> allocate_parameters { nullptr };
		std::atomic<PFN_DestroyParams> destroy_parameters { nullptr };
		std::atomic<PFN_CreateFeature> create_feature { nullptr };
		std::atomic<PFN_ReleaseFeature> release_feature { nullptr };
		std::atomic<PFN_EvaluateFeature> evaluate_feature { nullptr };
		std::atomic<PFN_EvaluateFeature> evaluate_feature_c { nullptr };
		std::atomic<PFN_Shutdown> shutdown { nullptr };
		std::atomic<PFN_Shutdown1> shutdown1 { nullptr };
		std::atomic<PFN_OptimalSettingsCallback> optimal { nullptr };
	} g_real;

	using PFN_GetProcAddress = FARPROC(WINAPI *)(HMODULE, LPCSTR);
	std::atomic<PFN_GetProcAddress> g_real_gpa { nullptr };
	std::atomic<HMODULE> g_core { nullptr };

	SRWLOCK g_feature_lock = SRWLOCK_INIT; // serializes the wrapped create/evaluate/release
	SRWLOCK g_stats_lock = SRWLOCK_INIT;
	forge::ngx_hook::stats g_stats;
	forge::ngx_hook::inputs g_inputs; // under g_stats_lock

	struct effective
	{
		unsigned int out_w = 0, out_h = 0;
		int perf = -1;
		int flags = 0;
		unsigned int presets[6] = {};

		bool operator==(const effective &o) const
		{
			return out_w == o.out_w && out_h == o.out_h && perf == o.perf && flags == o.flags &&
				std::memcmp(presets, o.presets, sizeof(presets)) == 0;
		}
		bool operator!=(const effective &o) const { return !(*this == o); }
	};

	struct feature_record
	{
		Handle *game_handle = nullptr; // what the game holds; stays alive until the game releases it
		Handle *own_handle = nullptr;  // replacement created here when the running config differs from game_eff

		// The game's creation request
		unsigned int in_w = 0, in_h = 0, out_w = 0, out_h = 0;
		int perf = -1, flags = 0, output_subrects = 0;
		unsigned int creation_mask = 1, visibility_mask = 1;
		unsigned int presets[6] = {};

		effective game_eff; // config the game's own handle was created with
		effective eff;      // config that runs now
		bool native = false; // created on the native context below ReShade; evaluated there as well
		uint64_t generation = 0;
		bool pending_reset = false;
		uint64_t recreates = 0;
		unsigned int last_render_w = 0, last_render_h = 0;

		Handle *live() const { return own_handle != nullptr ? own_handle : game_handle; }
	};

	std::vector<std::unique_ptr<feature_record>> g_features;
	Parameter *g_own_params = nullptr;
	bool g_logged_untracked = false;
	bool g_nr_on = false; // the NR bridge holds resources for the game's frame

	// No reference is held: the context belongs to the game's device, and ReShade clears it before that device goes.
	std::atomic<ID3D11DeviceContext *> g_native_ctx { nullptr };

	// The native immediate context for the game's immediate context; any other context stays as it is.
	ID3D11DeviceContext *unwrap(ID3D11DeviceContext *ctx)
	{
		ID3D11DeviceContext *native = g_native_ctx.load();
		if (native == nullptr || ctx == nullptr || ctx == native || ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
			return ctx;
		return native;
	}

	// The device below ReShade's wrapper for the game's device, from the native immediate context. No reference
	// is kept: the native context belongs to that device.
	ID3D11Device *unwrap(ID3D11Device *dev)
	{
		ID3D11DeviceContext *native_ctx = g_native_ctx.load();
		if (native_ctx == nullptr || dev == nullptr)
			return dev;
		ID3D11Device *native = nullptr;
		native_ctx->GetDevice(&native);
		if (native == nullptr)
			return dev;
		native->Release();
		return native;
	}

	// NGX keeps the Init device and creates the D3D11 objects of its features on it, shaders and state objects included
	std::atomic<bool> g_init_native { false };

	// ---------------------------------------------------------------------------------------------

	Result get_int(const Parameter *p, const char *n, int *v)
	{
		Result r = get_i(p, n, v);
		if (succeeded(r))
			return r;
		unsigned int u = 0;
		r = get_ui(p, n, &u);
		if (succeeded(r))
			*v = static_cast<int>(u);
		return r;
	}

	unsigned int get_uint_or(const Parameter *p, const char *n, unsigned int fallback)
	{
		unsigned int v = 0;
		if (succeeded(get_ui(p, n, &v)))
			return v;
		int i = 0;
		if (succeeded(get_i(p, n, &i)) && i >= 0)
			return static_cast<unsigned int>(i);
		return fallback;
	}

	void set_error(const char *fmt, ...)
	{
		char buf[160];
		va_list args;
		va_start(args, fmt);
		vsnprintf(buf, sizeof(buf), fmt, args);
		va_end(args);
		FORGE_LOG("error: %s", buf);
		lock_guard g(g_stats_lock);
		std::memcpy(g_stats.last_error, buf, sizeof(buf));
	}

	int perf_for_ratio(double r)
	{
		if (r <= 1.0001) return perf_dlaa;
		if (r < 1.41) return perf_ultra_quality;
		if (r < 1.61) return perf_max_quality;
		if (r < 1.86) return perf_balanced;
		if (r < 2.5) return perf_max_perf;
		return perf_ultra_performance;
	}

	effective request_effective(const feature_record &r)
	{
		effective e;
		e.out_w = r.out_w;
		e.out_h = r.out_h;
		e.perf = r.perf;
		e.flags = r.flags;
		std::memcpy(e.presets, r.presets, sizeof(e.presets));
		return e;
	}

	effective compute_effective(const feature_record &r, const forge::settings &s)
	{
		effective e = request_effective(r);
		if (!s.enabled)
			return e;
		if (r.in_w != 0)
			e.perf = perf_for_ratio(static_cast<double>(e.out_w) / static_cast<double>(r.in_w));

		if (s.exposure == forge::exposure_mode::on)
			e.flags |= flag_auto_exposure;
		else if (s.exposure == forge::exposure_mode::off)
			e.flags &= ~flag_auto_exposure;

		if (s.preset != 0)
			for (unsigned int &p : e.presets)
				p = s.preset;
		return e;
	}

	void apply(Parameter *p, const effective &e)
	{
		set_ui(p, name::out_width, e.out_w);
		set_ui(p, name::out_height, e.out_h);
		if (e.perf >= 0)
			set_i(p, name::perf_quality, e.perf);
		set_i(p, name::create_flags, e.flags);
		for (int i = 0; i < 6; ++i)
			set_ui(p, name::preset_hint[i], e.presets[i]);
	}

	void snapshot(const Parameter *p, feature_record &r)
	{
		r.in_w = get_uint_or(p, name::width, 0);
		r.in_h = get_uint_or(p, name::height, 0);
		r.out_w = get_uint_or(p, name::out_width, 0);
		r.out_h = get_uint_or(p, name::out_height, 0);
		int v = -1;
		r.perf = succeeded(get_int(p, name::perf_quality, &v)) ? v : -1;
		v = 0;
		r.flags = succeeded(get_int(p, name::create_flags, &v)) ? v : 0;
		v = 0;
		r.output_subrects = succeeded(get_int(p, name::enable_output_subrects, &v)) ? v : 0;
		r.creation_mask = get_uint_or(p, name::creation_node_mask, 1);
		r.visibility_mask = get_uint_or(p, name::visibility_node_mask, 1);
		for (int i = 0; i < 6; ++i)
			r.presets[i] = get_uint_or(p, name::preset_hint[i], 0);
	}

	void format_presets(char *buf, size_t size, const unsigned int presets[6])
	{
		// Order: DLAA, Quality, Balanced, Performance, UltraPerf, UltraQuality
		snprintf(buf, size, "DLAA=%c Q=%c B=%c P=%c UP=%c UQ=%c",
			preset_letter(presets[perf_dlaa]), preset_letter(presets[perf_max_quality]), preset_letter(presets[perf_balanced]),
			preset_letter(presets[perf_max_perf]), preset_letter(presets[perf_ultra_performance]), preset_letter(presets[perf_ultra_quality]));
	}

	void publish(const feature_record *r)
	{
		lock_guard g(g_stats_lock);
		forge::ngx_hook::feature_view &v = g_stats.feature;
		if (r == nullptr)
		{
			v = {};
			return;
		}
		v.valid = true;
		v.game_in_w = r->in_w;
		v.game_in_h = r->in_h;
		v.game_out_w = r->out_w;
		v.game_out_h = r->out_h;
		v.game_perf = r->perf;
		v.game_flags = r->flags;
		v.game_output_subrects = r->output_subrects;
		std::memcpy(v.game_presets, r->presets, sizeof(v.game_presets));
		v.eff_out_w = r->eff.out_w;
		v.eff_out_h = r->eff.out_h;
		v.eff_perf = r->eff.perf;
		v.eff_flags = r->eff.flags;
		std::memcpy(v.eff_presets, r->eff.presets, sizeof(v.eff_presets));
		v.using_own_feature = r->own_handle != nullptr;
		v.recreates = r->recreates;
	}

	void log_effective(const char *what, const effective &e)
	{
		char presets[64];
		format_presets(presets, sizeof(presets), e.presets);
		FORGE_LOG("%s: output %ux%u perf=%s flags=0x%X presets[%s]", what, e.out_w, e.out_h, perf_quality_label(e.perf), e.flags, presets);
	}

	feature_record *find(const Handle *h)
	{
		for (auto &r : g_features)
			if (r->game_handle == h)
				return r.get();
		return nullptr;
	}

	Parameter *own_params()
	{
		if (g_own_params != nullptr)
			return g_own_params;
		PFN_GetParams alloc = g_real.allocate_parameters.load();
		if (alloc == nullptr && g_core.load() != nullptr && g_real_gpa.load() != nullptr)
			alloc = reinterpret_cast<PFN_GetParams>(g_real_gpa.load()(g_core.load(), "NVSDK_NGX_D3D11_AllocateParameters"));
		if (alloc == nullptr || !succeeded(alloc(&g_own_params)))
			g_own_params = nullptr;
		return g_own_params;
	}

	void destroy_own_params()
	{
		if (g_own_params == nullptr)
			return;
		PFN_DestroyParams destroy = g_real.destroy_parameters.load();
		if (destroy == nullptr && g_core.load() != nullptr && g_real_gpa.load() != nullptr)
			destroy = reinterpret_cast<PFN_DestroyParams>(g_real_gpa.load()(g_core.load(), "NVSDK_NGX_D3D11_DestroyParameters"));
		if (destroy != nullptr)
			destroy(g_own_params);
		g_own_params = nullptr;
	}

	// Brings the running feature in line with the settings. Called with g_feature_lock held,
	// on the thread and context the game evaluates DLSS with.
	void reconfigure(ID3D11DeviceContext *ctx, feature_record &r, const forge::settings &s)
	{
		effective e = compute_effective(r, s);
		if (e == r.eff)
			return;

		PFN_CreateFeature create = g_real.create_feature.load();
		PFN_ReleaseFeature release = g_real.release_feature.load();
		if (create == nullptr || release == nullptr)
			return;

		if (e == r.game_eff)
		{
			if (r.own_handle != nullptr)
				release(r.own_handle);
			r.own_handle = nullptr;
			r.eff = e;
			r.pending_reset = true;
			r.recreates++;
			log_effective("switched back to the game's DLSS feature", e);
			publish(&r);
			return;
		}

		Parameter *p = own_params();
		if (p == nullptr)
		{
			// r.eff keeps describing the live handle; the next settings change retries
			set_error("NGX AllocateParameters unavailable; settings stay as created");
			return;
		}
		set_ui(p, name::width, r.in_w);
		set_ui(p, name::height, r.in_h);
		set_i(p, name::enable_output_subrects, r.output_subrects);
		set_ui(p, name::creation_node_mask, r.creation_mask);
		set_ui(p, name::visibility_node_mask, r.visibility_mask);
		apply(p, e);

		Handle *h = nullptr;
		const Result res = create(ctx, feature_super_sampling, p, &h);
		{
			lock_guard g(g_stats_lock);
			g_stats.creates++;
			g_stats.last_create_result = res;
		}
		if (!succeeded(res) || h == nullptr)
		{
			set_error("DLSS re-create failed (0x%08X) for %ux%u -> %ux%u", res, r.in_w, r.in_h, e.out_w, e.out_h);
			// r.eff keeps describing the live handle; the next settings change retries
			publish(&r);
			return;
		}
		if (r.own_handle != nullptr)
			release(r.own_handle);
		r.own_handle = h;
		r.eff = e;
		r.pending_reset = true;
		r.recreates++;
		log_effective("DLSS feature re-created", e);
		publish(&r);
	}

	void record_render_size(feature_record &r, unsigned int w, unsigned int h, float jx, float jy, bool reset, Result res)
	{
		const bool changed = (w != r.last_render_w || h != r.last_render_h);
		r.last_render_w = w;
		r.last_render_h = h;

		lock_guard g(g_stats_lock);
		g_stats.evaluations++;
		if (reset)
			g_stats.resets++;
		if (changed)
			g_stats.render_size_changes++;
		g_stats.last_result = res;
		g_stats.render_w = w;
		g_stats.render_h = h;
		g_stats.jitter_x = jx;
		g_stats.jitter_y = jy;
		g_stats.history[g_stats.history_pos] = r.out_w != 0 ? 100.0f * static_cast<float>(w) / static_cast<float>(r.out_w) : 0.0f;
		g_stats.history_pos = (g_stats.history_pos + 1) % forge::ngx_hook::history_size;
		if (g_stats.history_count < forge::ngx_hook::history_size)
			g_stats.history_count++;
	}

	// DLSS 5 neural rendering on the finished DLSS output: (bx, by) with the game's output size.
	// Depth and motion vectors are the ones the game handed DLSS.
	void run_nr(ID3D11DeviceContext *ctx, const Parameter *p, const feature_record &r, const forge::settings &s, ID3D11Resource *out,
		unsigned int bx, unsigned int by, unsigned int rw, unsigned int rh, bool dlss_ok, bool reset)
	{
		if (!s.enabled || s.guest_mode || !s.nr.enabled)
		{
			if (g_nr_on)
			{
				forge::nr::release();
				g_nr_on = false;
			}
			return;
		}
		if (!dlss_ok || out == nullptr)
			return;

		forge::nr::frame f;
		f.ctx = ctx;
		f.source = r.live();
		f.image = out;
		f.x = bx;
		f.y = by;
		f.w = r.out_w;
		f.h = r.out_h;
		get_d3d11(p, name::depth, &f.depth);
		get_d3d11(p, name::motion_vectors, &f.motion);
		f.depth_x = get_uint_or(p, name::depth_subrect_x, 0);
		f.depth_y = get_uint_or(p, name::depth_subrect_y, 0);
		f.motion_x = get_uint_or(p, name::mv_subrect_x, 0);
		f.motion_y = get_uint_or(p, name::mv_subrect_y, 0);
		// FFXIV's render subrect can exceed the render size it created DLSS with; the guides hold the smaller
		f.guide_w = rw < r.in_w ? rw : r.in_w;
		f.guide_h = rh < r.in_h ? rh : r.in_h;
		const bool low_res_motion = (r.flags & flag_mv_low_res) != 0;
		f.motion_w = low_res_motion ? f.guide_w : f.w;
		f.motion_h = low_res_motion ? f.guide_h : f.h;
		get_f(p, name::jitter_x, &f.jitter_x);
		get_f(p, name::jitter_y, &f.jitter_y);
		f.motion_jittered = (r.flags & flag_mv_jittered) != 0;
		if (!succeeded(get_f(p, name::mv_scale_x, &f.mv_scale_x)))
			f.mv_scale_x = 1.0f;
		if (!succeeded(get_f(p, name::mv_scale_y, &f.mv_scale_y)))
			f.mv_scale_y = 1.0f;
		f.depth_inverted = (r.flags & flag_depth_inverted) != 0;
		f.hdr = (r.flags & flag_is_hdr) != 0;
		f.reset = reset;
		g_nr_on = true;

		static bool logged = false;
		if (!logged && f.depth != nullptr && f.motion != nullptr)
		{
			logged = true;
			FORGE_LOG("DLSS NR frame: output %ux%u at %u,%u, guides %ux%u (depth at %u,%u, vectors at %u,%u), vector scale %.3f %.3f, depth %s, %s",
				f.w, f.h, f.x, f.y, f.guide_w, f.guide_h, f.depth_x, f.depth_y, f.motion_x, f.motion_y, f.mv_scale_x, f.mv_scale_y,
				f.depth_inverted ? "inverted" : "not inverted", f.hdr ? "HDR" : "display-referred");
		}
		forge::nr::run(f, s.nr);
	}

	// ---------------------------------------------------------------------------------------------
	// Optimal-settings callback: the game asks NGX which render size to use for its output size.

	Result hk_optimal_settings(Parameter *p)
	{
		const PFN_OptimalSettingsCallback real = g_real.optimal.load();
		if (real == nullptr)
			return result_fail;
		const Result res = real(p);

		const unsigned int tw = get_uint_or(p, name::width, 0), th = get_uint_or(p, name::height, 0);
		int pq = -1;
		get_int(p, name::perf_quality, &pq);
		const unsigned int ow = get_uint_or(p, name::out_width, 0), oh = get_uint_or(p, name::out_height, 0);
		const unsigned int mnw = get_uint_or(p, name::dyn_min_w, 0), mnh = get_uint_or(p, name::dyn_min_h, 0);
		const unsigned int mxw = get_uint_or(p, name::dyn_max_w, 0), mxh = get_uint_or(p, name::dyn_max_h, 0);

		const forge::settings s = forge::get_settings();
		unsigned int rw = ow, rh = oh;
		bool overridden = false;
		if (succeeded(res) && s.enabled && s.lock != forge::render_lock::game && tw != 0 && th != 0)
		{
			const double ratio = s.lock == forge::render_lock::native ? 1.0 : s.render_scale / 100.0;
			const unsigned int min_w = (tw + 2) / 3, min_h = (th + 2) / 3;
			rw = static_cast<unsigned int>(std::lround(tw * ratio));
			rh = static_cast<unsigned int>(std::lround(th * ratio));
			rw = rw < min_w ? min_w : (rw > tw ? tw : rw);
			rh = rh < min_h ? min_h : (rh > th ? th : rh);
			set_ui(p, name::out_width, rw);
			set_ui(p, name::out_height, rh);
			set_ui(p, name::dyn_max_w, rw);
			set_ui(p, name::dyn_max_h, rh);
			set_ui(p, name::dyn_min_w, rw);
			set_ui(p, name::dyn_min_h, rh);
			overridden = true;
		}

		bool log_it;
		{
			lock_guard g(g_stats_lock);
			// One line per new output size or locked render size; the game asks once per quality mode
			log_it = g_stats.optimal_calls == 0 || tw != g_stats.opt_target_w || th != g_stats.opt_target_h || rw != g_stats.opt_ret_w || rh != g_stats.opt_ret_h;
			g_stats.optimal_calls++;
			g_stats.opt_target_w = tw;
			g_stats.opt_target_h = th;
			g_stats.opt_perf = pq;
			g_stats.opt_ngx_w = ow;
			g_stats.opt_ngx_h = oh;
			g_stats.opt_ngx_min_w = mnw;
			g_stats.opt_ngx_min_h = mnh;
			g_stats.opt_ngx_max_w = mxw;
			g_stats.opt_ngx_max_h = mxh;
			g_stats.opt_ret_w = rw;
			g_stats.opt_ret_h = rh;
			g_stats.opt_overridden = overridden;
		}
		if (log_it)
			FORGE_LOG("size query: output %ux%u, render %ux%u%s", tw, th, rw, rh, overridden ? " (locked by Forge)" : "");
		return res;
	}

	void wrap_optimal_callback(Parameter *p)
	{
		void *cb = nullptr;
		if (p == nullptr || !succeeded(get_ptr(p, name::optimal_settings_callback, &cb)) || cb == nullptr)
			return;
		if (cb == reinterpret_cast<void *>(&hk_optimal_settings))
			return;
		const auto prev = g_real.optimal.exchange(reinterpret_cast<PFN_OptimalSettingsCallback>(cb));
		set_ptr(p, name::optimal_settings_callback, reinterpret_cast<void *>(&hk_optimal_settings));
		if (prev != reinterpret_cast<PFN_OptimalSettingsCallback>(cb))
		{
			FORGE_LOG("optimal-settings callback wrapped (NGX %p)", cb);
			lock_guard g(g_stats_lock);
			g_stats.callback_wrapped = true;
		}
	}

	// ---------------------------------------------------------------------------------------------
	// NGX D3D11 entry-point wrappers

	Result hk_GetCapabilityParameters(Parameter **out)
	{
		const PFN_GetParams real = g_real.get_capability_parameters.load();
		const Result r = real != nullptr ? real(out) : result_fail;
		if (succeeded(r) && out != nullptr)
			wrap_optimal_callback(*out);
		return r;
	}

	Result hk_GetParameters(Parameter **out)
	{
		const PFN_GetParams real = g_real.get_parameters.load();
		const Result r = real != nullptr ? real(out) : result_fail;
		if (succeeded(r) && out != nullptr)
			wrap_optimal_callback(*out);
		return r;
	}

	Result hk_AllocateParameters(Parameter **out)
	{
		const PFN_GetParams real = g_real.allocate_parameters.load();
		const Result r = real != nullptr ? real(out) : result_fail;
		if (succeeded(r) && out != nullptr)
			wrap_optimal_callback(*out);
		return r;
	}

	// NGX starts on the native device, so ReShade sees none of the objects NGX creates
	template <typename F>
	Result init_on_native(const char *entry, ID3D11Device *device, F &&call)
	{
		ID3D11Device *const native = unwrap(device);
		Result r = call(native);
		if (native != device && !succeeded(r))
		{
			set_error("NGX %s on the native device failed (0x%08X); NGX stays on ReShade's device", entry, r);
			r = call(device);
			g_init_native.store(false);
			return r;
		}
		g_init_native.store(native != device);
		FORGE_LOG("NGX %s on the %s device (game %p, used %p): 0x%08X", entry, native != device ? "native" : "game's",
			static_cast<void *>(device), static_cast<void *>(native), r);
		return r;
	}

	Result hk_Init(unsigned long long app_id, const wchar_t *path, ID3D11Device *device, const void *info, int version)
	{
		const PFN_Init real = g_real.init.load();
		if (real == nullptr)
			return result_fail;
		return init_on_native("Init", device, [&](ID3D11Device *d) { return real(app_id, path, d, info, version); });
	}

	Result hk_Init_Ext(unsigned long long app_id, const wchar_t *path, ID3D11Device *device, int version, const void *info)
	{
		const PFN_Init_Ext real = g_real.init_ext.load();
		if (real == nullptr)
			return result_fail;
		return init_on_native("Init_Ext", device, [&](ID3D11Device *d) { return real(app_id, path, d, version, info); });
	}

	Result hk_Init_ProjectID(const char *project_id, int engine, const char *engine_version, const wchar_t *path, ID3D11Device *device,
		int version, const void *info)
	{
		const PFN_Init_ProjectID real = g_real.init_project_id.load();
		if (real == nullptr)
			return result_fail;
		return init_on_native("Init_ProjectID", device,
			[&](ID3D11Device *d) { return real(project_id, engine, engine_version, path, d, version, info); });
	}

	Result hk_CreateFeature(ID3D11DeviceContext *ctx, int feature, Parameter *p, Handle **out)
	{
		const PFN_CreateFeature real = g_real.create_feature.load();
		if (real == nullptr)
			return result_fail;
		if (feature != feature_super_sampling || p == nullptr || out == nullptr)
			return real(ctx, feature, p, out);

		lock_guard fg(g_feature_lock);
		auto rec = std::make_unique<feature_record>();
		snapshot(p, *rec);
		const forge::settings s = forge::get_settings();
		rec->generation = forge::feature_generation();

		char presets[64];
		format_presets(presets, sizeof(presets), rec->presets);
		FORGE_LOG("game creates DLSS: render %ux%u -> output %ux%u perf=%s flags=0x%X output-subrects=%d presets[%s]",
			rec->in_w, rec->in_h, rec->out_w, rec->out_h, perf_quality_label(rec->perf), rec->flags, rec->output_subrects, presets);

		const effective request = request_effective(*rec);
		auto attempt = [&](ID3D11DeviceContext *run, effective &e)
		{
			apply(p, e);
			Result r = real(run, feature, p, out);
			if ((!succeeded(r) || *out == nullptr) && e != request)
			{
				set_error("DLSS create with forge settings failed (0x%08X); using the game's request", r);
				e = request;
				apply(p, e);
				r = real(run, feature, p, out);
			}
			return r;
		};
		ID3D11DeviceContext *run = unwrap(ctx);
		effective e = compute_effective(*rec, s);
		Result res = attempt(run, e);
		if ((!succeeded(res) || *out == nullptr) && run != ctx)
		{
			set_error("DLSS create on the native device context failed (0x%08X); DLSS stays on ReShade's device", res);
			run = ctx;
			e = compute_effective(*rec, s);
			res = attempt(run, e);
		}
		apply(p, request); // the game's parameter object keeps the game's own values
		{
			lock_guard g(g_stats_lock);
			g_stats.creates++;
			g_stats.last_create_result = res;
		}
		if (!succeeded(res) || *out == nullptr)
		{
			set_error("game DLSS create failed (0x%08X)", res);
			return res;
		}
		rec->game_handle = *out;
		rec->game_eff = e;
		rec->eff = e;
		rec->native = run != ctx;
		log_effective(rec->native ? "DLSS feature created on the native device context" : "DLSS feature created", e);
		publish(rec.get());
		g_features.push_back(std::move(rec));
		return res;
	}

	// Logs the textures behind the game's DLSS inputs whenever the set changes (inside the evaluate, where they are alive)
	void describe_inputs(const forge::ngx_hook::inputs &in)
	{
		static void *seen[4] = {};
		void *now[4] = { in.color, in.depth, in.motion, in.output };
		if (std::memcmp(seen, now, sizeof(now)) == 0)
			return;
		std::memcpy(seen, now, sizeof(now));
		const char *names[4] = { "color", "depth", "motion", "output" };
		char line[512];
		int n = 0;
		for (int i = 0; i < 4 && n < static_cast<int>(sizeof(line)) - 1; ++i)
		{
			D3D11_TEXTURE2D_DESC d = {};
			ID3D11Texture2D *t = nullptr;
			if (now[i] != nullptr && SUCCEEDED(static_cast<ID3D11Resource *>(now[i])->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&t))) && t != nullptr)
			{
				t->GetDesc(&d);
				t->Release();
			}
			n += snprintf(line + n, sizeof(line) - n, "%s%s %p %ux%u f%d b0x%X", i == 0 ? "" : " | ", names[i], now[i], d.Width, d.Height, static_cast<int>(d.Format), d.BindFlags);
		}
		FORGE_LOG("DLSS inputs (thread %lu): %s", in.thread, line);
	}

	Result evaluate(PFN_EvaluateFeature real, ID3D11DeviceContext *ctx, const Handle *h, const Parameter *cp, void *cb)
	{
		if (real == nullptr)
			return result_fail;
		Parameter *p = const_cast<Parameter *>(cp);

		AcquireSRWLockExclusive(&g_feature_lock);
		feature_record *rec = find(h);
		if (rec == nullptr || p == nullptr || ctx == nullptr)
		{
			if (!g_logged_untracked && rec == nullptr)
			{
				g_logged_untracked = true;
				FORGE_LOG("evaluate on a DLSS handle Forge did not see created (%p); passed through", static_cast<const void *>(h));
			}
			ReleaseSRWLockExclusive(&g_feature_lock);
			return real(ctx, h, cp, cb);
		}

		// The context the feature was created on; NR and re-created features follow it
		ID3D11DeviceContext *const run = rec->native ? unwrap(ctx) : ctx;
		const forge::settings s = forge::get_settings();
		const uint64_t gen = forge::feature_generation();
		if (gen != rec->generation)
		{
			rec->generation = gen;
			reconfigure(run, *rec, s);
		}

		unsigned int rw = get_uint_or(p, name::render_subrect_w, 0), rh = get_uint_or(p, name::render_subrect_h, 0);
		if (rw == 0 || rh == 0)
		{
			rw = rec->in_w;
			rh = rec->in_h;
		}
		float jx = 0.0f, jy = 0.0f;
		get_f(p, name::jitter_x, &jx);
		get_f(p, name::jitter_y, &jy);
		int game_reset = 0;
		get_int(p, name::reset, &game_reset);
		const bool forced_reset = rec->pending_reset;
		bool touched_reset = forced_reset;
		rec->pending_reset = false;
		if (forced_reset)
			set_i(p, name::reset, 1);

		ID3D11Resource *out = nullptr;
		get_d3d11(p, name::output, &out);
		unsigned int bx = 0, by = 0;
		if (rec->output_subrects != 0)
		{
			bx = get_uint_or(p, name::output_subrect_x, 0);
			by = get_uint_or(p, name::output_subrect_y, 0);
		}
		const Result res = real(run, rec->live(), p, cb);
		if (touched_reset)
			set_i(p, name::reset, game_reset);
		{
			forge::ngx_hook::inputs in;
			get_d3d11(p, name::color, reinterpret_cast<ID3D11Resource **>(&in.color));
			get_d3d11(p, name::depth, reinterpret_cast<ID3D11Resource **>(&in.depth));
			get_d3d11(p, name::motion_vectors, reinterpret_cast<ID3D11Resource **>(&in.motion));
			in.output = out;
			in.thread = GetCurrentThreadId();
			describe_inputs(in);
			lock_guard g(g_stats_lock);
			in.evaluation = g_stats.evaluations + 1;
			g_inputs = in;
		}
		if (succeeded(res))
		{
			// The game's vectors for ReShade effects: at render size (MV low-res flag) or else at output size
			ID3D11Resource *motion = nullptr;
			get_d3d11(p, name::motion_vectors, &motion);
			float sx = 1.0f, sy = 1.0f;
			if (!succeeded(get_f(p, name::mv_scale_x, &sx)))
				sx = 1.0f;
			if (!succeeded(get_f(p, name::mv_scale_y, &sy)))
				sy = 1.0f;
			const bool low_res = (rec->flags & flag_mv_low_res) != 0;
			const unsigned int mw = low_res ? (rw < rec->in_w ? rw : rec->in_w) : rec->out_w;
			const unsigned int mh = low_res ? (rh < rec->in_h ? rh : rec->in_h) : rec->out_h;
			forge::motion::publish(run, motion, get_uint_or(p, name::mv_subrect_x, 0), get_uint_or(p, name::mv_subrect_y, 0), mw, mh, sx, sy);
		}
		run_nr(run, p, *rec, s, out, bx, by, rw, rh, succeeded(res), game_reset != 0 || touched_reset);

		record_render_size(*rec, rw, rh, jx, jy, touched_reset || game_reset != 0, res);
		if (!succeeded(res))
		{
			static int logged = 0;
			if (logged++ < 10)
				set_error("DLSS evaluate failed (0x%08X)", res);
		}
		ReleaseSRWLockExclusive(&g_feature_lock);
		return res;
	}

	Result hk_EvaluateFeature(ID3D11DeviceContext *ctx, const Handle *h, const Parameter *p, void *cb)
	{
		return evaluate(g_real.evaluate_feature.load(), ctx, h, p, cb);
	}

	Result hk_EvaluateFeature_C(ID3D11DeviceContext *ctx, const Handle *h, const Parameter *p, void *cb)
	{
		return evaluate(g_real.evaluate_feature_c.load(), ctx, h, p, cb);
	}

	Result hk_ReleaseFeature(Handle *h)
	{
		const PFN_ReleaseFeature real = g_real.release_feature.load();
		if (real == nullptr)
			return result_fail;
		{
			lock_guard fg(g_feature_lock);
			for (auto it = g_features.begin(); it != g_features.end(); ++it)
			{
				if ((*it)->game_handle != h)
					continue;
				if ((*it)->own_handle != nullptr)
					real((*it)->own_handle);
				FORGE_LOG("game releases DLSS feature %p", static_cast<void *>(h));
				g_features.erase(it);
				break;
			}
			if (g_features.empty() && g_nr_on)
			{
				forge::nr::release();
				g_nr_on = false;
			}
			publish(g_features.empty() ? nullptr : g_features.back().get());
		}
		return real(h);
	}

	void teardown(const char *why)
	{
		lock_guard fg(g_feature_lock);
		const PFN_ReleaseFeature release = g_real.release_feature.load();
		for (auto &r : g_features)
			if (r->own_handle != nullptr && release != nullptr)
				release(r->own_handle);
		g_features.clear();
		destroy_own_params();
		forge::nr::detach();
		g_nr_on = false;
		publish(nullptr);
		FORGE_LOG("NGX %s: forge state cleared", why);
	}

	Result hk_Shutdown()
	{
		teardown("shutdown");
		const PFN_Shutdown real = g_real.shutdown.load();
		return real != nullptr ? real() : result_fail;
	}

	Result hk_Shutdown1(ID3D11Device *device)
	{
		teardown("shutdown1");
		const PFN_Shutdown1 real = g_real.shutdown1.load();
		return real != nullptr ? real(g_init_native.load() ? unwrap(device) : device) : result_fail; // the device NGX started on
	}

	// ---------------------------------------------------------------------------------------------

	template <typename T>
	FARPROC route(std::atomic<T> &slot, FARPROC real, T wrapper)
	{
		slot.store(reinterpret_cast<T>(real));
		return reinterpret_cast<FARPROC>(wrapper);
	}

	FARPROC intercept(HMODULE module, const char *suffix, FARPROC real)
	{
		FARPROC result = real;
		if (std::strcmp(suffix, "Init") == 0)
			result = route(g_real.init, real, &hk_Init);
		else if (std::strcmp(suffix, "Init_Ext") == 0)
			result = route(g_real.init_ext, real, &hk_Init_Ext);
		else if (std::strcmp(suffix, "Init_ProjectID") == 0)
			result = route(g_real.init_project_id, real, &hk_Init_ProjectID);
		else if (std::strcmp(suffix, "GetCapabilityParameters") == 0)
			result = route(g_real.get_capability_parameters, real, &hk_GetCapabilityParameters);
		else if (std::strcmp(suffix, "GetParameters") == 0)
			result = route(g_real.get_parameters, real, &hk_GetParameters);
		else if (std::strcmp(suffix, "AllocateParameters") == 0)
			result = route(g_real.allocate_parameters, real, &hk_AllocateParameters);
		else if (std::strcmp(suffix, "DestroyParameters") == 0)
			g_real.destroy_parameters.store(reinterpret_cast<PFN_DestroyParams>(real));
		else if (std::strcmp(suffix, "CreateFeature") == 0)
			result = route(g_real.create_feature, real, &hk_CreateFeature);
		else if (std::strcmp(suffix, "ReleaseFeature") == 0)
			result = route(g_real.release_feature, real, &hk_ReleaseFeature);
		else if (std::strcmp(suffix, "EvaluateFeature") == 0)
			result = route(g_real.evaluate_feature, real, &hk_EvaluateFeature);
		else if (std::strcmp(suffix, "EvaluateFeature_C") == 0)
			result = route(g_real.evaluate_feature_c, real, &hk_EvaluateFeature_C);
		else if (std::strcmp(suffix, "Shutdown") == 0)
			result = route(g_real.shutdown, real, &hk_Shutdown);
		else if (std::strcmp(suffix, "Shutdown1") == 0)
			result = route(g_real.shutdown1, real, &hk_Shutdown1);

		if (g_core.exchange(module) != module)
		{
			char path[260] = {};
			GetModuleFileNameA(module, path, sizeof(path));
			FORGE_LOG("NGX core: %s", path);
			lock_guard g(g_stats_lock);
			std::memcpy(g_stats.core_path, path, sizeof(path));
		}
		if (result != real)
		{
			lock_guard g(g_stats_lock);
			g_stats.resolved++;
		}
		return result;
	}

	FARPROC WINAPI hk_GetProcAddress(HMODULE module, LPCSTR name)
	{
		const FARPROC real = g_real_gpa.load()(module, name);
		if (real == nullptr || reinterpret_cast<uintptr_t>(name) <= 0xFFFF)
			return real;
		if (std::strncmp(name, "NVSDK_NGX_D3D11_", 16) != 0)
			return real;
		return intercept(module, name + 16, real);
	}

	bool patch_iat(HMODULE module, const char *func, FARPROC k32, FARPROC kb, void *hook)
	{
		auto *base = reinterpret_cast<uint8_t *>(module);
		const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
			return false;
		const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
		const IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
		if (dir.VirtualAddress == 0)
			return false;

		bool patched = false;
		for (auto *imp = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR *>(base + dir.VirtualAddress); imp->Name != 0; ++imp)
		{
			auto *addrs = reinterpret_cast<IMAGE_THUNK_DATA *>(base + imp->FirstThunk);
			const auto *names = imp->OriginalFirstThunk != 0 ? reinterpret_cast<const IMAGE_THUNK_DATA *>(base + imp->OriginalFirstThunk) : nullptr;
			for (size_t i = 0; addrs[i].u1.Function != 0; ++i)
			{
				bool match = false;
				if (names != nullptr && !IMAGE_SNAP_BY_ORDINAL(names[i].u1.Ordinal))
				{
					const auto *ibn = reinterpret_cast<const IMAGE_IMPORT_BY_NAME *>(base + names[i].u1.AddressOfData);
					match = std::strcmp(reinterpret_cast<const char *>(ibn->Name), func) == 0;
				}
				const auto current = reinterpret_cast<FARPROC>(addrs[i].u1.Function);
				match = match || (current != nullptr && (current == k32 || current == kb));
				if (!match || current == reinterpret_cast<FARPROC>(hook))
					continue;

				// Chain to whatever the slot pointed at before (another hook or kernel32)
				if (!patched)
					g_real_gpa.store(reinterpret_cast<PFN_GetProcAddress>(current));
				DWORD old = 0;
				if (!VirtualProtect(&addrs[i].u1.Function, sizeof(ULONG_PTR), PAGE_READWRITE, &old))
					continue;
				addrs[i].u1.Function = reinterpret_cast<ULONG_PTR>(hook);
				VirtualProtect(&addrs[i].u1.Function, sizeof(ULONG_PTR), old, &old);
				patched = true;
			}
		}
		return patched;
	}
}

bool forge::ngx_hook::install(HMODULE module)
{
	{
		lock_guard g(g_stats_lock);
		if (g_stats.hooked)
			return true;
	}
	const HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
	const HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
	const FARPROC gpa_k32 = k32 != nullptr ? GetProcAddress(k32, "GetProcAddress") : nullptr;
	const FARPROC gpa_kb = kb != nullptr ? GetProcAddress(kb, "GetProcAddress") : nullptr;
	g_real_gpa.store(reinterpret_cast<PFN_GetProcAddress>(gpa_k32 != nullptr ? gpa_k32 : gpa_kb));
	if (g_real_gpa.load() == nullptr)
		return false;

	const bool ok = patch_iat(module, "GetProcAddress", gpa_k32, gpa_kb, reinterpret_cast<void *>(&hk_GetProcAddress));
	char path[260] = {};
	GetModuleFileNameA(module, path, sizeof(path));
	FORGE_LOG("GetProcAddress import of %s %s", path, ok ? "routed through Forge" : "NOT found");
	lock_guard g(g_stats_lock);
	g_stats.hooked = ok;
	return ok;
}

forge::ngx_hook::stats forge::ngx_hook::get_stats()
{
	AcquireSRWLockShared(&g_stats_lock);
	stats copy = g_stats;
	ReleaseSRWLockShared(&g_stats_lock);
	return copy;
}

void forge::ngx_hook::set_native_context(ID3D11DeviceContext *ctx)
{
	ID3D11DeviceContext *expected = nullptr;
	if (g_native_ctx.compare_exchange_strong(expected, ctx))
		FORGE_LOG("native D3D11 context below ReShade: %p", static_cast<void *>(ctx));
	else if (expected != ctx)
		FORGE_LOG("a second D3D11 device (context %p); DLSS stays with the first one", static_cast<void *>(ctx));
}

void forge::ngx_hook::clear_native_context(ID3D11DeviceContext *ctx)
{
	ID3D11DeviceContext *expected = ctx;
	if (g_native_ctx.compare_exchange_strong(expected, nullptr))
		FORGE_LOG("native D3D11 context released by ReShade");
}

forge::ngx_hook::inputs forge::ngx_hook::last_inputs()
{
	AcquireSRWLockShared(&g_stats_lock);
	inputs copy = g_inputs;
	ReleaseSRWLockShared(&g_stats_lock);
	return copy;
}

HMODULE forge::ngx_hook::core_module()
{
	return g_core.load();
}

FARPROC forge::ngx_hook::core_proc(const char *name)
{
	const HMODULE core = g_core.load();
	const PFN_GetProcAddress gpa = g_real_gpa.load();
	return core != nullptr && gpa != nullptr ? gpa(core, name) : nullptr;
}

void forge::ngx_hook::claim_indicator()
{
	wchar_t value[256] = {};
	if (GetEnvironmentVariableW(L"DXVK_NVAPI_SET_NGX_DEBUG_OPTIONS", value, 256) == 0)
		return;
	SetEnvironmentVariableW(L"DXVK_NVAPI_SET_NGX_DEBUG_OPTIONS", nullptr);
	FORGE_LOG("DXVK_NVAPI_SET_NGX_DEBUG_OPTIONS=%ls taken out of the environment%s; ShowDlssIndicator follows the Forge setting", value,
		GetModuleHandleW(L"nvapi64.dll") != nullptr ? " (nvapi64.dll was loaded already and may have read it)" : "");
}

bool forge::ngx_hook::set_indicator_registry(bool on)
{
	HKEY key = nullptr;
	LSTATUS st = RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
	if (st != ERROR_SUCCESS)
	{
		set_error("NGXCore registry key not writable (%ld)", static_cast<long>(st));
		return false;
	}
	const DWORD value = on ? 1024 : 0;
	st = RegSetValueExW(key, L"ShowDlssIndicator", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value), sizeof(value));
	RegCloseKey(key);
	FORGE_LOG("ShowDlssIndicator = %lu (%ld)", static_cast<unsigned long>(value), static_cast<long>(st));
	return st == ERROR_SUCCESS;
}
