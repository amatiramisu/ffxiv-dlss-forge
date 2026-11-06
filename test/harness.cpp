// Proton test harness: drives the real NGX D3D11 runtime through Forge exactly like the
// static NGX library inside ffxiv_dx11.exe does (GetProcAddress-resolved entry points).
#include "log.h"
#include "ngx_abi.h"
#include "ngx_hook.h"
#include "nr_bridge.h"
#include "settings.h"

#include <windows.h>
#include <d3d11.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace ngx;

namespace
{
	int g_failures = 0;

	void check(bool ok, const char *what)
	{
		printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
		FORGE_LOG("[%s] %s", ok ? "PASS" : "FAIL", what);
		if (!ok)
			g_failures++;
	}

	uint16_t to_half(float f)
	{
		uint32_t x;
		std::memcpy(&x, &f, 4);
		const uint32_t sign = (x >> 16) & 0x8000;
		int32_t exp = static_cast<int32_t>((x >> 23) & 0xFF) - 127 + 15;
		uint32_t mant = x & 0x7FFFFF;
		if (exp <= 0)
			return static_cast<uint16_t>(sign);
		if (exp >= 31)
			return static_cast<uint16_t>(sign | 0x7C00);
		// round to nearest even
		uint32_t h = sign | (static_cast<uint32_t>(exp) << 10) | (mant >> 13);
		const uint32_t rem = mant & 0x1FFF;
		if (rem > 0x1000 || (rem == 0x1000 && (h & 1)))
			h++;
		return static_cast<uint16_t>(h);
	}

	float from_half(uint16_t h)
	{
		const uint32_t sign = (h & 0x8000u) << 16;
		const uint32_t exp = (h >> 10) & 0x1F;
		const uint32_t mant = h & 0x3FF;
		uint32_t x;
		if (exp == 0)
		{
			if (mant == 0)
				x = sign;
			else
			{
				float f = std::ldexp(static_cast<float>(mant), -24);
				return sign ? -f : f;
			}
		}
		else if (exp == 31)
			x = sign | 0x7F800000 | (mant << 13);
		else
			x = sign | ((exp - 15 + 127) << 23) | (mant << 13);
		float f;
		std::memcpy(&f, &x, 4);
		return f;
	}

	ID3D11Texture2D *make_tex(ID3D11Device *dev, UINT w, UINT h, DXGI_FORMAT fmt, UINT bind, const void *data, UINT pitch)
	{
		D3D11_TEXTURE2D_DESC d = {};
		d.Width = w;
		d.Height = h;
		d.MipLevels = 1;
		d.ArraySize = 1;
		d.Format = fmt;
		d.SampleDesc.Count = 1;
		d.Usage = D3D11_USAGE_DEFAULT;
		d.BindFlags = bind;
		D3D11_SUBRESOURCE_DATA init = { data, pitch, 0 };
		ID3D11Texture2D *t = nullptr;
		dev->CreateTexture2D(&d, data != nullptr ? &init : nullptr, &t);
		return t;
	}

	std::vector<float> read_rgba16f(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *src)
	{
		D3D11_TEXTURE2D_DESC d;
		src->GetDesc(&d);
		d.Usage = D3D11_USAGE_STAGING;
		d.BindFlags = 0;
		d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		d.MiscFlags = 0;
		ID3D11Texture2D *staging = nullptr;
		dev->CreateTexture2D(&d, nullptr, &staging);
		std::vector<float> out;
		if (staging == nullptr)
			return out;
		ctx->CopyResource(staging, src);
		D3D11_MAPPED_SUBRESOURCE m;
		if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m)))
		{
			// Four floats per pixel whatever the format holds (half floats or 8-bit unorm); missing channels read 0, alpha 1
			const bool bytes = d.Format == DXGI_FORMAT_R8G8B8A8_UNORM || d.Format == DXGI_FORMAT_R8G8_UNORM || d.Format == DXGI_FORMAT_R8_UNORM;
			const UINT channels = d.Format == DXGI_FORMAT_R16G16_FLOAT || d.Format == DXGI_FORMAT_R8G8_UNORM ? 2 : (d.Format == DXGI_FORMAT_R16_FLOAT || d.Format == DXGI_FORMAT_R8_UNORM ? 1 : 4);
			out.resize(static_cast<size_t>(d.Width) * d.Height * 4);
			for (UINT y = 0; y < d.Height; ++y)
			{
				const uint8_t *row = static_cast<const uint8_t *>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
				const auto *halves = reinterpret_cast<const uint16_t *>(row);
				for (UINT x = 0; x < d.Width; ++x)
					for (UINT c = 0; c < 4; ++c)
						out[(static_cast<size_t>(y) * d.Width + x) * 4 + c] = c < channels ? (bytes ? row[x * channels + c] / 255.0f : from_half(halves[x * channels + c])) : (c == 3 ? 1.0f : 0.0f);
			}
			ctx->Unmap(staging, 0);
		}
		staging->Release();
		return out;
	}

	double psnr_rgb(const std::vector<float> &a, const std::vector<float> &b)
	{
		if (a.size() != b.size() || a.empty())
			return 0.0;
		double se = 0.0;
		size_t n = 0;
		for (size_t i = 0; i < a.size(); ++i)
		{
			if ((i & 3) == 3)
				continue;
			const double d = static_cast<double>(a[i]) - b[i];
			se += d * d;
			n++;
		}
		const double mse = se / static_cast<double>(n);
		return mse <= 1e-12 ? 99.0 : 10.0 * std::log10(1.0 / mse);
	}

	double mean_rgb(const std::vector<float> &a)
	{
		double s = 0.0;
		size_t n = 0;
		for (size_t i = 0; i < a.size(); ++i)
			if ((i & 3) != 3)
			{
				s += a[i];
				n++;
			}
		return n ? s / static_cast<double>(n) : 0.0;
	}

	// ----------------------------------------------------------------------------------------
	// NGX end-to-end

	struct ngx_api
	{
		PFN_Init_ProjectID init_project = nullptr;
		PFN_Shutdown1 shutdown1 = nullptr;
		PFN_GetParams get_caps = nullptr;
		PFN_GetParams alloc = nullptr;
		PFN_DestroyParams destroy = nullptr;
		PFN_CreateFeature create = nullptr;
		PFN_ReleaseFeature release = nullptr;
		PFN_EvaluateFeature evaluate = nullptr;
	};

	struct path_list { const wchar_t *const *path; unsigned int length; };
	struct logging_info { void (*callback)(const char *, int, int); int min_level; bool disable_other_sinks; };
	struct feature_common_info { path_list paths; void *internal; logging_info logging; };

	void ngx_log(const char *msg, int, int)
	{
		FORGE_LOG("NGX: %s", msg);
	}

	struct scene
	{
		ID3D11Texture2D *color = nullptr, *depth = nullptr, *mv = nullptr, *output = nullptr;
		std::vector<float> reference;
		UINT w = 1920, h = 1080;
	};

	scene make_scene(ID3D11Device *dev)
	{
		scene s;
		std::vector<uint16_t> c(static_cast<size_t>(s.w) * s.h * 4);
		s.reference.resize(c.size());
		for (UINT y = 0; y < s.h; ++y)
			for (UINT x = 0; x < s.w; ++x)
			{
				const float fx = static_cast<float>(x), fy = static_cast<float>(y);
				const float dx = fx - 960.0f, dy = fy - 540.0f;
				const float disc = (dx * dx + dy * dy < 300.0f * 300.0f) ? 0.25f : 0.0f;
				const float base[3] = {
					0.35f + 0.25f * std::sin(fx * 0.11f) * std::cos(fy * 0.07f) + disc,
					0.40f + 0.20f * std::sin(fx * 0.05f + fy * 0.04f),
					0.30f + 0.20f * std::cos(fy * 0.13f) - disc * 0.5f,
				};
				for (int k = 0; k < 3; ++k)
				{
					const size_t i = (static_cast<size_t>(y) * s.w + x) * 4 + k;
					c[i] = to_half(base[k]);
					s.reference[i] = from_half(c[i]);
				}
				const size_t a = (static_cast<size_t>(y) * s.w + x) * 4 + 3;
				c[a] = to_half(1.0f);
				s.reference[a] = 1.0f;
			}
		s.color = make_tex(dev, s.w, s.h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, c.data(), s.w * 8);
		// Inverted depth falling from 0.5 at the left edge to 0.002 at the right: 2 to 500 near-plane multiples,
		// log-uniform, so the default near band (to 20) covers 42 % of the width and the far band (from 400) 4 %
		std::vector<float> d(static_cast<size_t>(s.w) * s.h, 0.5f);
		for (UINT y = 0; y < s.h; ++y)
			for (UINT x = 0; x < s.w; ++x)
				d[static_cast<size_t>(y) * s.w + x] = std::exp(std::log(0.5f) + (std::log(0.002f) - std::log(0.5f)) * (static_cast<float>(x) / static_cast<float>(s.w - 1)));
		s.depth = make_tex(dev, s.w, s.h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, d.data(), s.w * 4);
		std::vector<uint32_t> m(static_cast<size_t>(s.w) * s.h, 0u);
		s.mv = make_tex(dev, s.w, s.h, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE, m.data(), s.w * 4);
		s.output = make_tex(dev, s.w, s.h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET, nullptr, 0);
		return s;
	}

	struct gpu_timer
	{
		ID3D11Query *disjoint = nullptr, *t0 = nullptr, *t1 = nullptr;
		void init(ID3D11Device *dev)
		{
			D3D11_QUERY_DESC q = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
			dev->CreateQuery(&q, &disjoint);
			q.Query = D3D11_QUERY_TIMESTAMP;
			dev->CreateQuery(&q, &t0);
			dev->CreateQuery(&q, &t1);
		}
		double ms(ID3D11DeviceContext *ctx)
		{
			D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
			UINT64 a = 0, b = 0;
			while (ctx->GetData(disjoint, &dj, sizeof(dj), 0) == S_FALSE) {}
			ctx->GetData(t0, &a, sizeof(a), 0);
			ctx->GetData(t1, &b, sizeof(b), 0);
			return (dj.Disjoint || dj.Frequency == 0) ? -1.0 : 1000.0 * static_cast<double>(b - a) / static_cast<double>(dj.Frequency);
		}
	};

	double run_frames(const ngx_api &api, ID3D11Device *dev, ID3D11DeviceContext *ctx, const scene &sc, Parameter *p, Handle *h, int frames, double *gpu_ms, Result *last)
	{
		gpu_timer timer;
		timer.init(dev);
		double total_ms = 0.0;
		int timed = 0;
		Result r = result_success;
		for (int i = 0; i < frames; ++i)
		{
			set_d3d11(p, name::color, sc.color);
			set_d3d11(p, name::output, sc.output);
			set_d3d11(p, name::depth, sc.depth);
			set_d3d11(p, name::motion_vectors, sc.mv);
			set_f(p, name::jitter_x, 0.0f);
			set_f(p, name::jitter_y, 0.0f);
			set_f(p, name::mv_scale_x, 1.0f);
			set_f(p, name::mv_scale_y, 1.0f);
			set_i(p, name::reset, i == 0 ? 1 : 0);
			set_ui(p, name::render_subrect_w, sc.w);
			set_ui(p, name::render_subrect_h, sc.h);
			const bool time_it = i >= frames / 2;
			if (time_it)
			{
				ctx->Begin(timer.disjoint);
				ctx->End(timer.t0);
			}
			r = api.evaluate(ctx, h, p, nullptr);
			if (time_it)
			{
				ctx->End(timer.t1);
				ctx->End(timer.disjoint);
				const double ms = timer.ms(ctx);
				if (ms >= 0.0)
				{
					total_ms += ms;
					timed++;
				}
			}
			if (!succeeded(r))
				break;
		}
		*gpu_ms = timed ? total_ms / timed : -1.0;
		*last = r;
		const std::vector<float> out = read_rgba16f(dev, ctx, sc.output);
		timer.disjoint->Release();
		timer.t0->Release();
		timer.t1->Release();
		const double mean = mean_rgb(out);
		FORGE_LOG("output mean %.4f (reference %.4f)", mean, mean_rgb(sc.reference));
		return psnr_rgb(out, sc.reference);
	}

	void stage(const char *label, const ngx_api &api, ID3D11Device *dev, ID3D11DeviceContext *ctx, const scene &sc, Parameter *p, Handle *h,
		unsigned int expect_out_w, double min_psnr)
	{
		double ms = 0.0;
		Result r = result_success;
		const double psnr = run_frames(api, dev, ctx, sc, p, h, 24, &ms, &r);
		const auto st = forge::ngx_hook::get_stats();
		char msg[256];
		snprintf(msg, sizeof(msg), "%s: result 0x%08X, DLSS output %ux%u (%s), PSNR %.2f dB vs input, GPU %.3f ms/eval",
			label, r, st.feature.eff_out_w, st.feature.eff_out_h, st.feature.using_own_feature ? "forge feature" : "game feature", psnr, ms);
		check(succeeded(r) && st.feature.eff_out_w == expect_out_w && psnr >= min_psnr, msg);
	}

	// PSNR over columns [x0, x1) of two RGBA float images of width w
	double psnr_cols(const std::vector<float> &a, const std::vector<float> &b, unsigned int w, unsigned int x0, unsigned int x1)
	{
		if (a.size() != b.size() || a.empty())
			return 0.0;
		const size_t h = a.size() / 4 / w;
		double se = 0.0;
		size_t n = 0;
		for (size_t y = 0; y < h; ++y)
			for (unsigned int x = x0; x < x1; ++x)
				for (int k = 0; k < 3; ++k)
				{
					const size_t i = (y * w + x) * 4 + k;
					const double d = static_cast<double>(a[i]) - b[i];
					se += d * d;
					n++;
				}
		const double mse = se / static_cast<double>(n);
		return mse <= 1e-12 ? 99.0 : 10.0 * std::log10(1.0 / mse);
	}

	// A real 1920x1080 picture (still life and a face) when nr-scene.rgba sits next to the harness
	ID3D11Texture2D *load_nr_scene(ID3D11Device *dev, std::vector<float> &reference)
	{
		wchar_t path[MAX_PATH] = {};
		GetModuleFileNameW(nullptr, path, MAX_PATH);
		wcscpy(wcsrchr(path, L'\\') + 1, L"nr-scene.rgba");
		FILE *f = _wfopen(path, L"rb");
		if (f == nullptr)
			return nullptr;
		std::vector<uint8_t> px(1920u * 1080u * 4u);
		const size_t got = fread(px.data(), 1, px.size(), f);
		fclose(f);
		if (got != px.size())
			return nullptr;
		std::vector<uint16_t> c(px.size());
		reference.resize(px.size());
		for (size_t i = 0; i < px.size(); ++i)
		{
			c[i] = to_half((i & 3) == 3 ? 1.0f : px[i] / 255.0f);
			reference[i] = from_half(c[i]);
		}
		return make_tex(dev, 1920, 1080, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, c.data(), 1920 * 8);
	}

	// DLSS NR (feature 18). The snippet's own D3D11 init returns FeatureNotSupported, so NGX reports the
	// feature unavailable on the game's D3D11 device; Forge runs it on its D3D12 bridge instead.
	// FORGE_NR_DEVICE=d3d12 selects the D3D12 model device; FORGE_NR_DEVICE=d3d12-direct also skips D3D12's
	// device factory, the route of D3D12 runtimes without one
	int test_device()
	{
		const char *d = getenv("FORGE_NR_DEVICE");
		if (d == nullptr || std::strncmp(d, "d3d12", 5) != 0)
			return 0;
		forge::nr::set_skip_device_factory(std::strcmp(d, "d3d12-direct") == 0);
		return 1;
	}

	void test_nr(const ngx_api &, ID3D11Device *dev, ID3D11DeviceContext *ctx, const scene &sc, Parameter *caps)
	{
		int avail = -1, init_res = -1;
		get_i(caps, "DLSSNR.Available", &avail);
		get_i(caps, "DLSSNR.FeatureInitResult", &init_res);
		char msg[400];
		snprintf(msg, sizeof(msg), "NR on the D3D11 device: DLSSNR.Available=%d FeatureInitResult=0x%08X (the snippet's D3D11 backend is disabled)", avail, init_res);
		check(avail == 0 && static_cast<unsigned int>(init_res) == 0xBAD00001u, msg);

		std::vector<float> reference;
		ID3D11Texture2D *source = load_nr_scene(dev, reference);
		const bool real = source != nullptr;
		if (!real)
		{
			source = sc.color;
			source->AddRef();
			reference = sc.reference;
		}
		FORGE_LOG("NR scene: %s", real ? "nr-scene.rgba (still life + face, x >= 1278 is the face)" : "synthetic");
		const unsigned int face_x = real ? 1278 : 0;

		ID3D11Texture2D *image = make_tex(dev, sc.w, sc.h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET, nullptr, 0);
		forge::nr::frame f;
		f.ctx = ctx;
		f.image = image;
		f.w = sc.w;
		f.h = sc.h;
		f.depth = sc.depth;
		f.motion = sc.mv;
		f.guide_w = sc.w;
		f.guide_h = sc.h;
		f.depth_inverted = true;

		forge::nr::config c;
		c.enabled = true;
		c.device = test_device();
		// Runs `n` frames of the static picture; the output of the last one is returned
		auto frames = [&](int n, const forge::nr::config &cc) {
			bool ok = true;
			for (int i = 0; i < n; ++i)
			{
				ctx->CopyResource(image, source);
				f.reset = false;
				ok = forge::nr::run(f, cc) && ok;
			}
			return ok;
		};
		// Applies a configuration, waits out the rebuild settle, and returns the settled raw model answer
		auto model_answer = [&](forge::nr::config cc, bool raw = true) {
			if (raw)
				cc.debug_view = 2;
			frames(1, cc);
			Sleep(600);
			frames(8, cc);
			return read_rgba16f(dev, ctx, image);
		};
		// The same from a feature built with exactly these values
		auto built_answer = [&](const forge::nr::config &cc) {
			forge::nr::release();
			return model_answer(cc);
		};

		ULONGLONG t0 = GetTickCount64();
		bool ok = frames(1, c);
		const ULONGLONG first_ms = GetTickCount64() - t0;
		auto st = forge::nr::get_status();
		snprintf(msg, sizeof(msg), "NR first frame (%llu ms incl. device, NGX init, feature): %s%s%s",
			static_cast<unsigned long long>(first_ms), ok ? "ran" : "did not run", st.reason[0] ? ": " : "", st.reason);
		check(ok && st.ready, msg);
		if (!ok)
		{
			image->Release();
			source->Release();
			forge::nr::release();
			return;
		}
		t0 = GetTickCount64();
		ok = frames(30, c);
		const double cpu_ms = static_cast<double>(GetTickCount64() - t0) / 30.0;
		std::vector<float> composed = read_rgba16f(dev, ctx, image);
		st = forge::nr::get_status();
		snprintf(msg, sizeof(msg), "NR evaluate: %s, network '%s', %ux%u, PSNR %.2f dB vs input, model %.3f ms GPU, %.2f ms per frame incl. sync",
			ok ? "ok" : "failed", st.pass[0].network, st.work_w, st.work_h, psnr_rgb(composed, reference), st.gpu_ms, cpu_ms);
		check(ok && psnr_rgb(composed, reference) > 18.0 && psnr_rgb(composed, reference) < 90.0, msg);

		// The three pictures the composition works with, written for inspection
		auto dump = [&](const char *name, const std::vector<float> &px) {
			wchar_t path[MAX_PATH] = {};
			GetModuleFileNameW(nullptr, path, MAX_PATH);
			swprintf(wcsrchr(path, L'\\') + 1, 64, L"nr-%hs.f32", name);
			if (FILE *out = _wfopen(path, L"wb"))
			{
				fwrite(px.data(), sizeof(float), px.size(), out);
				fclose(out);
			}
		};
		dump("composed", composed);
		for (int t = 0; t < 3; ++t)
		{
			ID3D11Texture2D *tex = forge::nr::debug_texture(t);
			const std::vector<float> px = tex != nullptr ? read_rgba16f(dev, ctx, tex) : std::vector<float>();
			double mean = 0.0, alpha = 0.0;
			for (size_t i = 0; i + 3 < px.size(); i += 4)
			{
				mean += px[i] + px[i + 1] + px[i + 2];
				alpha += px[i + 3];
			}
			const double n = px.empty() ? 1.0 : static_cast<double>(px.size() / 4);
			FORGE_LOG("NR texture %d: %zu px, mean rgb %.4f, mean alpha %.3f, PSNR vs input %.2f dB", t, px.size() / 4, mean / (3.0 * n), alpha / n, px.size() == reference.size() ? psnr_rgb(px, reference) : 0.0);
		}
		for (int view = 1; view <= 3; ++view)
		{
			forge::nr::config cv = c;
			cv.debug_view = view;
			frames(3, cv);
			const std::vector<float> o = read_rgba16f(dev, ctx, image);
			static const char *const names[] = { "", "input", "model", "difference" };
			dump(names[view], o);
			FORGE_LOG("NR debug view %d (%s) vs input: %.2f dB", view, names[view], psnr_rgb(o, reference));
		}
		{
			forge::nr::config off = c;
			off.detail = 0.0f;
			frames(2, off);
			const std::vector<float> o = read_rgba16f(dev, ctx, image);
			const double p0 = psnr_rgb(o, reference);
			size_t changed = 0, worst = 0;
			double worst_d = 0.0;
			for (size_t i = 0; i < o.size() && o.size() == reference.size(); ++i)
			{
				if ((i & 3) == 3)
					continue;
				const double d = std::fabs(static_cast<double>(o[i]) - reference[i]);
				if (d > 0.0 || std::isnan(o[i]))
				{
					changed++;
					if (d > worst_d || std::isnan(o[i]))
					{
						worst_d = std::isnan(o[i]) ? 1e9 : d;
						worst = i;
					}
				}
			}
			const size_t wp = worst / 4;
			snprintf(msg, sizeof(msg), "NR detail strength 0 leaves the frame untouched (PSNR %.2f dB, %zu channel values changed, largest %.4f at %zu,%zu: %.4f -> %.4f)",
				p0, changed, worst_d, wp % sc.w, wp / sc.w, o.empty() ? 0.0 : reference[worst], o.empty() ? 0.0 : o[worst]);
			check(p0 >= 99.0, msg);
		}

		const bool quick = getenv("FORGE_NR_QUICK") != nullptr;
		// Noise floor: the same settings rebuilt from scratch
		forge::nr::config base = c;
		const std::vector<float> a0 = built_answer(base);
		const std::vector<float> a1 = built_answer(base);
		const double floor_db = psnr_rgb(a0, a1);
		FORGE_LOG("NR probe: same settings after a rebuild: %.2f dB (face %.2f, rest %.2f) (noise floor)", floor_db,
			psnr_cols(a0, a1, sc.w, face_x, sc.w), psnr_cols(a0, a1, sc.w, 0, face_x));
		const double same_db = floor_db - 3.0;

		// Network presets (DLSSNR.Hint.Render.Preset); Forge offers 0..2, 3 and 4 measured as preset B
		std::vector<float> preset_out[3];
		char nets[3][64] = {};
		for (unsigned int pr = 0; pr <= (quick ? 0u : 2u); ++pr)
		{
			forge::nr::config cp = base;
			cp.pass[0].preset = static_cast<int>(pr);
			preset_out[pr] = built_answer(cp);
			std::memcpy(nets[pr], forge::nr::get_status().pass[0].network, 64);
		}
		if (!quick)
		{
			const double a_vs_b = psnr_rgb(preset_out[1], preset_out[2]), default_vs_b = psnr_rgb(preset_out[0], preset_out[2]);
			snprintf(msg, sizeof(msg), "NR networks: NVIDIA default vs preset B %.2f dB, preset A vs B %.2f dB", default_vs_b, a_vs_b);
			check(default_vs_b >= 99.0 && a_vs_b < 60.0, msg);
		}

		// Each model parameter: does a built value change the answer, does a value above 1 differ from 1,
		// and does the value take effect when written only at evaluate (feature built with the default)?
		struct probe_case { const char *name; float forge::nr::profile::*fp; int forge::nr::profile::*ip; float v0, v1; float base_skin = NAN, base_structure = NAN; };
		const char *only = getenv("FORGE_NR_PROBE");
		const probe_case cases[] = {
			{ "DLSSNR.Style 0->1", nullptr, &forge::nr::profile::style, 0, 1 },
			{ "DLSSNR.Style 0->2", nullptr, &forge::nr::profile::style, 0, 2 },
			{ "DLSSNR.Intensity 1->0", &forge::nr::profile::intensity, nullptr, 1.0f, 0.0f },
			{ "DLSSNR.Intensity 1->0.5", &forge::nr::profile::intensity, nullptr, 1.0f, 0.5f },
			{ "DLSSNR.Intensity 0.5->1", &forge::nr::profile::intensity, nullptr, 0.5f, 1.0f },
			{ "DLSSNR.LocalStructureStrength 1->0", &forge::nr::profile::structure, nullptr, 1.0f, 0.0f },
			{ "DLSSNR.LocalStructureStrength 1->2", &forge::nr::profile::structure, nullptr, 1.0f, 2.0f },
			{ "DLSSNR.LocalToneStrength 1->0", &forge::nr::profile::tone, nullptr, 1.0f, 0.0f },
			{ "DLSSNR.LocalToneStrength 1->2", &forge::nr::profile::tone, nullptr, 1.0f, 2.0f },
			{ "DLSSNR.SkinStructureStrength -1->0", &forge::nr::profile::skin, nullptr, -1.0f, 0.0f },
			{ "DLSSNR.SkinStructureStrength -1->1", &forge::nr::profile::skin, nullptr, -1.0f, 1.0f },
			{ "DLSSNR.SkinStructureStrength 1->2", &forge::nr::profile::skin, nullptr, 1.0f, 2.0f },
			{ "DLSSNR.SkinStructureStrength 0->0.5", &forge::nr::profile::skin, nullptr, 0.0f, 0.5f },
			{ "DLSSNR.SkinStructureStrength 0.5->1", &forge::nr::profile::skin, nullptr, 0.5f, 1.0f },
			{ "skin -1->1 at structure 0.5", &forge::nr::profile::skin, nullptr, -1.0f, 1.0f, NAN, 0.5f },
			{ "skin -1->0.5 at structure 0.5", &forge::nr::profile::skin, nullptr, -1.0f, 0.5f, NAN, 0.5f },
			{ "DLSSNR.UseAutoMask 1->0", nullptr, &forge::nr::profile::auto_mask, 1, 0 },
			{ "UseAutoMask 1->0 at skin 0", nullptr, &forge::nr::profile::auto_mask, 1, 0, 0.0f },
			{ "UseAutoMask 1->0 at skin 1, structure 0", nullptr, &forge::nr::profile::auto_mask, 1, 0, 1.0f, 0.0f },
		};
		for (const probe_case &pc : cases)
		{
			if (quick)
				break;
			if (only != nullptr && std::strstr(pc.name, only) == nullptr)
				continue;
			forge::nr::config c0 = base, c1 = base;
			if (pc.base_skin == pc.base_skin)
				c0.pass[0].skin = c1.pass[0].skin = pc.base_skin;
			if (pc.base_structure == pc.base_structure)
				c0.pass[0].structure = c1.pass[0].structure = pc.base_structure;
			if (pc.fp != nullptr)
			{
				c0.pass[0].*pc.fp = pc.v0;
				c1.pass[0].*pc.fp = pc.v1;
			}
			else
			{
				c0.pass[0].*pc.ip = static_cast<int>(pc.v0);
				c1.pass[0].*pc.ip = static_cast<int>(pc.v1);
			}
			const std::vector<float> o0 = built_answer(c0);
			const std::vector<float> built1 = built_answer(c1);
			// Built with v0, v1 written only at evaluate
			built_answer(c0);
			forge::nr::set_probe(true);
			const std::vector<float> live1 = model_answer(c1);
			forge::nr::set_probe(false);
			const double effect = psnr_rgb(o0, built1), effect_face = psnr_cols(o0, built1, sc.w, face_x, sc.w), effect_rest = psnr_cols(o0, built1, sc.w, 0, face_x);
			const double live_vs_built = psnr_rgb(live1, built1), live_vs_old = psnr_rgb(live1, o0);
			const char *verdict = effect >= same_db ? "no effect" : (live_vs_built >= same_db ? "evaluate-live" : (live_vs_old >= same_db ? "create-time" : "partly live"));
			FORGE_LOG("NR probe %-40s built: %.2f dB (face %.2f, rest %.2f); evaluate-only vs built %.2f, vs unchanged %.2f -> %s",
				pc.name, effect, effect_face, effect_rest, live_vs_built, live_vs_old, verdict);
		}
		model_answer(base);

		// Control mask (DLSSNR.ControlMask + DLSSNR.MaskBlendMode): which picture formats the snippet takes, which
		// channel carries what, how the values relate to the global strengths, and what the blend modes do.
		// The probe picture has one value pair on the left half and another on the right; each half's model
		// answer is matched against answers built without a mask at known structure/tone strengths.
		if (!quick && (only == nullptr || std::strstr("ControlMask", only) != nullptr))
		{
			const unsigned int mw = sc.w, mh = sc.h;
			std::vector<float> pic(static_cast<size_t>(mw) * mh * 4);
			const auto paint = [&](const float *l, const float *r) {
				for (unsigned int y = 0; y < mh; ++y)
					for (unsigned int x = 0; x < mw; ++x)
					{
						float *px = &pic[(static_cast<size_t>(y) * mw + x) * 4];
						for (int c = 0; c < 4; ++c)
							px[c] = x < mw / 2 ? l[c] : r[c];
					}
			};
			forge::nr::config off = base;
			off.pass[0].auto_mask = 0;
			const auto strengths = [&](float st, float tn) {
				forge::nr::config cc = off;
				cc.pass[0].structure = st;
				cc.pass[0].tone = tn;
				return built_answer(cc);
			};
			struct reference { const char *name; std::vector<float> px; };
			const reference refs[] = {
				{ "S1T1", strengths(1.0f, 1.0f) }, { "S0T0", strengths(0.0f, 0.0f) }, { "S1T0", strengths(1.0f, 0.0f) }, { "S0T1", strengths(0.0f, 1.0f) },
				{ "S.5T.5", strengths(0.5f, 0.5f) }, { "S.5T1", strengths(0.5f, 1.0f) }, { "S1T.5", strengths(1.0f, 0.5f) }, { "auto", built_answer(base) },
			};
			FORGE_LOG("NR mask references (automask off unless named): S0T0 vs S1T1 %.2f dB, S1T0 vs S1T1 %.2f, S0T1 vs S1T1 %.2f, S.5T.5 vs S1T1 %.2f, S.5T1 vs S1T1 %.2f, S1T.5 vs S1T1 %.2f, automask on vs S1T1 %.2f",
				psnr_rgb(refs[1].px, refs[0].px), psnr_rgb(refs[2].px, refs[0].px), psnr_rgb(refs[3].px, refs[0].px), psnr_rgb(refs[4].px, refs[0].px),
				psnr_rgb(refs[5].px, refs[0].px), psnr_rgb(refs[6].px, refs[0].px), psnr_rgb(refs[7].px, refs[0].px));

			struct mask_case { const char *name; DXGI_FORMAT fmt; float l[4], r[4]; int blend; bool automask; float skin; float global = NAN; };
			constexpr DXGI_FORMAT rgba = DXGI_FORMAT_R16G16B16A16_FLOAT, rg = DXGI_FORMAT_R16G16_FLOAT;
			const mask_case mcases[] = {
				{ "RGBA 1,1,1,1 | 0,0,0,0", rgba, { 1, 1, 1, 1 }, { 0, 0, 0, 0 }, 0, true, NAN },
				{ "RGBA 1,0,1,0 | 0,1,0,1", rgba, { 1, 0, 1, 0 }, { 0, 1, 0, 1 }, 0, true, NAN },
				{ "RGBA 0,1,1,0 | 1,0,0,1", rgba, { 0, 1, 1, 0 }, { 1, 0, 0, 1 }, 0, true, NAN },
				{ "RGBA 1,1,0,0 | 0,0,1,1", rgba, { 1, 1, 0, 0 }, { 0, 0, 1, 1 }, 0, true, NAN },
				{ "RGBA 1,0,0,0 | 0,0,1,0", rgba, { 1, 0, 0, 0 }, { 0, 0, 1, 0 }, 0, true, NAN },
				{ "RGBA 0,0,0,1 | 0,1,0,0", rgba, { 0, 0, 0, 1 }, { 0, 1, 0, 0 }, 0, true, NAN },
				{ "RGBA 1,1,1,1 | 0,0,0,0 skin 1", rgba, { 1, 1, 1, 1 }, { 0, 0, 0, 0 }, 0, true, 1.0f },
				{ "RGBA 1,1,1,1 | 0,0,0,0 skin 0", rgba, { 1, 1, 1, 1 }, { 0, 0, 0, 0 }, 0, true, 0.0f },
				{ "RGBA 1,1,1,1 | 0,0,0,0 skin .5", rgba, { 1, 1, 1, 1 }, { 0, 0, 0, 0 }, 0, true, 0.5f },
				{ "RGBA .5,1,1,1 | 1,.5,.5,1", rgba, { 0.5f, 1, 1, 1 }, { 1, 0.5f, 0.5f, 1 }, 0, true, NAN },
				{ "RGBA 1,.5,1,1 | 1,1,.5,1", rgba, { 1, 0.5f, 1, 1 }, { 1, 1, 0.5f, 1 }, 0, true, NAN },
				{ "RGBA 1,1,1,.5 | 1,1,1,0", rgba, { 1, 1, 1, 0.5f }, { 1, 1, 1, 0 }, 0, true, NAN },
				{ "RG 1,1 | 0,0", rg, { 1, 1, 0, 1 }, { 0, 0, 0, 1 }, 0, true, NAN },
				{ "RG 1,0 | 0,1", rg, { 1, 0, 0, 1 }, { 0, 1, 0, 1 }, 0, true, NAN },
				{ "R8 1 | 0", DXGI_FORMAT_R8_UNORM, { 1, 0, 0, 1 }, { 0, 0, 0, 1 }, 0, true, NAN },
				{ "RGBA8 1,1,1,1 | 0,0,0,0", DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 1, 1, 1 }, { 0, 0, 0, 0 }, 0, true, NAN },
				{ "RGBA 1,1,1,1 | 0,0,0,0 blend 1 automask on", rgba, { 1, 1, 1, 1 }, { 0, 0, 0, 0 }, 1, true, NAN },
				{ "RGBA 1,1,1,1 | 0,0,0,0 blend 2 automask on", rgba, { 1, 1, 1, 1 }, { 0, 0, 0, 0 }, 2, true, NAN },
				{ "RGBA 1,1,1,1 | 0,0,0,0 blend 3 automask on", rgba, { 1, 1, 1, 1 }, { 0, 0, 0, 0 }, 3, true, NAN },
				{ "RGBA 1,1,1,1 | 0,0,0,0 blend 1 automask off", rgba, { 1, 1, 1, 1 }, { 0, 0, 0, 0 }, 1, false, NAN },
				{ "RGBA 1,1,1,1 | 0,0,0,0 blend 3 automask off", rgba, { 1, 1, 1, 1 }, { 0, 0, 0, 0 }, 3, false, NAN },
				{ "RGBA .5,.5,.5,.5 blend 1 automask on", rgba, { 0.5f, 0.5f, 0.5f, 0.5f }, { 0.5f, 0.5f, 0.5f, 0.5f }, 1, true, NAN },
				{ "RGBA .5,.5,.5,.5 blend 2 automask on", rgba, { 0.5f, 0.5f, 0.5f, 0.5f }, { 0.5f, 0.5f, 0.5f, 0.5f }, 2, true, NAN },
				{ "RGBA .5,.5,.5,.5 blend 3 automask on", rgba, { 0.5f, 0.5f, 0.5f, 0.5f }, { 0.5f, 0.5f, 0.5f, 0.5f }, 3, true, NAN },
				{ "RGBA 1,1,1,1 | 1,.5,.5,1 at global .5", rgba, { 1, 1, 1, 1 }, { 1, 0.5f, 0.5f, 1 }, 0, true, NAN, 0.5f },
			};
			for (const mask_case &mc : mcases)
			{
				paint(mc.l, mc.r);
				forge::nr::set_probe_mask(pic.data(), mw, mh, mc.fmt);
				forge::nr::config cc = base;
				cc.pass[0].auto_mask = mc.automask ? 1 : 0;
				if (mc.skin == mc.skin)
					cc.pass[0].skin = mc.skin;
				if (mc.global == mc.global)
					cc.pass[0].structure = cc.pass[0].tone = mc.global;
				cc.mask.enabled = true;
				cc.mask.blend = mc.blend;
				const std::vector<float> o = model_answer(cc);
				const auto ms = forge::nr::get_status();
				char line[800];
				int n = snprintf(line, sizeof(line), "NR mask %-44s result 0x%08X%s%s |", mc.name, ms.last_result, ms.mask ? "" : " (no mask handed over)", ms.reason[0] ? ms.reason : "");
				for (int half = 0; half < 2; ++half)
				{
					const char *best = "?";
					double best_db = -1.0, second_db = -1.0;
					n += snprintf(line + n, sizeof(line) - n, " %s:", half == 0 ? "left" : "right");
					for (const reference &r : refs)
					{
						const double db = psnr_cols(o, r.px, mw, half == 0 ? 0 : mw / 2, half == 0 ? mw / 2 : mw);
						n += snprintf(line + n, sizeof(line) - n, " %s %.1f", r.name, db);
						if (db > best_db)
						{
							second_db = best_db;
							best_db = db;
							best = r.name;
						}
						else if (db > second_db)
							second_db = db;
					}
					n += snprintf(line + n, sizeof(line) - n, " -> %s%s |", best, best_db - second_db >= 6.0 ? "" : " (unclear)");
				}
				FORGE_LOG("%s", line);
			}
			forge::nr::set_probe_mask(nullptr, 0, 0, DXGI_FORMAT_UNKNOWN);
			// The painted mask: the near band at 0 structure and tone, the far band and the environment at 1
			{
				forge::nr::config cc = base;
				cc.pass[0].auto_mask = 0;
				cc.mask.enabled = true;
				cc.mask.near_band.structure = 0.0f;
				cc.mask.near_band.tone = 0.0f;
				const std::vector<float> o = model_answer(cc);
				const std::vector<float> m = read_rgba16f(dev, ctx, forge::nr::debug_texture(4));
				double sum_r = 0.0, sum_g = 0.0, sum_b = 0.0;
				for (size_t i = 0; i + 3 < m.size(); i += 4)
				{
					sum_r += m[i];
					sum_g += m[i + 1];
					sum_b += m[i + 2];
				}
				const double npx = m.empty() ? 1.0 : static_cast<double>(m.size() / 4);
				snprintf(msg, sizeof(msg), "NR painted mask: %zu px, mean weight %.3f, tone %.3f, structure %.3f (near band at 0, 42 %% of the width); answer vs S1T1 %.2f dB, vs S0T0 %.2f",
					m.size() / 4, sum_r / npx, sum_g / npx, sum_b / npx, psnr_rgb(o, refs[0].px), psnr_rgb(o, refs[1].px));
				check(m.size() / 4 == static_cast<size_t>(mw) * mh && sum_r / npx > 0.999 && sum_b / npx > 0.02 && sum_b / npx < 0.98 && std::fabs(sum_b - sum_g) < 1e-3 * npx, msg);
			}
			// The automask estimate: a second pass of the same frame with the base character at the other end
			{
				forge::nr::config cc = base;
				cc.show_automask = true;
				frames(1, cc);
				Sleep(600);
				frames(4, cc);
				st = forge::nr::get_status();
				const std::vector<float> a = read_rgba16f(dev, ctx, forge::nr::debug_texture(2));
				const std::vector<float> b = read_rgba16f(dev, ctx, forge::nr::debug_texture(3));
				snprintf(msg, sizeof(msg), "NR automask estimate: %u passes, pass 2 built with base character %.2f (pass 1 %.2f), the two answers differ by %.2f dB",
					st.passes, st.pass[1].built.skin, st.pass[0].built.skin, psnr_rgb(a, b));
				check(st.passes == 2 && st.pass[1].built.skin == 0.0f, msg);
			}
			model_answer(base);
		}

		// Pass bundling: each extra pass refines the previous answer with its own feature and history
		std::vector<float> pass_out[4];
		double pass_ms[4] = {};
		for (unsigned int n = 1; n <= 3; ++n)
		{
			forge::nr::config cn = base;
			cn.passes = n;
			frames(1, cn);
			frames(1, cn);
			frames(1, cn);
			frames(20, cn);
			pass_out[n] = read_rgba16f(dev, ctx, image);
			st = forge::nr::get_status();
			pass_ms[n] = st.gpu_ms;
			snprintf(msg, sizeof(msg), "NR %u pass%s: %u ran, %.3f ms GPU, PSNR %.2f dB vs input", n, n > 1 ? "es" : "", st.passes, st.gpu_ms, psnr_rgb(pass_out[n], reference));
			check(st.passes == n, msg);
		}
		snprintf(msg, sizeof(msg), "NR passes differ: 1 vs 2 %.2f dB, 2 vs 3 %.2f dB", psnr_rgb(pass_out[1], pass_out[2]), psnr_rgb(pass_out[2], pass_out[3]));
		check(psnr_rgb(pass_out[1], pass_out[2]) < 90.0 && psnr_rgb(pass_out[2], pass_out[3]) < 90.0, msg);

		// Pass chaining: per pass (default) against a single transfer, each from fresh features so both start
		// from the same history. One pass runs the same either way; with three passes the per-pass limits
		// change the answer.
		{
			auto settled = [&](unsigned int n, bool per_pass, double &ms) {
				forge::nr::config cs = base;
				cs.passes = n;
				cs.per_pass = per_pass;
				forge::nr::release();
				frames(1, cs);
				Sleep(600);
				frames(20, cs);
				ms = forge::nr::get_status().gpu_ms;
				return read_rgba16f(dev, ctx, image);
			};
			double ms[4] = {};
			const std::vector<float> one_pp = settled(1, true, ms[0]), one_s = settled(1, false, ms[1]);
			const std::vector<float> three_pp = settled(3, true, ms[2]), three_s = settled(3, false, ms[3]);
			const double one = psnr_rgb(one_pp, one_s), three = psnr_rgb(three_pp, three_s);
			snprintf(msg, sizeof(msg), "NR pass chaining: 1 pass per-pass vs single %.2f dB; 3 passes %.2f dB (per pass %.2f dB vs input at %.3f ms, single %.2f dB at %.3f ms)",
				one, three, psnr_rgb(three_pp, reference), ms[2], psnr_rgb(three_s, reference), ms[3]);
			check(one >= 99.0 && three < 90.0, msg);
		}

		// Model resolution below and above the frame
		for (float ws : { 0.5f, 1.5f })
		{
			forge::nr::config cw = base;
			cw.work_scale = ws;
			const unsigned int before_w = forge::nr::get_status().work_w;
			frames(3, cw);
			const bool held = forge::nr::get_status().work_w == before_w; // the size settles before it rebuilds
			Sleep(600);
			frames(25, cw);
			const std::vector<float> o = read_rgba16f(dev, ctx, image);
			st = forge::nr::get_status();
			const unsigned int expect_w = static_cast<unsigned int>(sc.w * ws + 0.5f);
			snprintf(msg, sizeof(msg), "NR model resolution %.0f %%: %ux%u (kept %s while settling), %.3f ms GPU, PSNR %.2f dB vs input, %.2f dB vs full resolution",
				ws * 100.0f, st.work_w, st.work_h, held ? "the old size" : "NOT the old size", st.gpu_ms, psnr_rgb(o, reference), psnr_rgb(o, composed));
			check(st.active && held && st.work_w == expect_w && psnr_rgb(o, reference) > 18.0, msg);
		}

		// Composition controls
		{
			forge::nr::config cc = base;
			frames(10, cc);
			const std::vector<float> full = read_rgba16f(dev, ctx, image);
			cc.colour = 0.0f;
			frames(2, cc);
			const std::vector<float> grey = read_rgba16f(dev, ctx, image);
			cc.colour = 2.0f;
			frames(2, cc);
			const std::vector<float> vivid = read_rgba16f(dev, ctx, image);
			cc.colour = 1.0f;
			cc.detail = 2.0f;
			frames(2, cc);
			const std::vector<float> strong = read_rgba16f(dev, ctx, image);
			cc.detail = 1.0f;
			cc.regions = true;
			cc.skin_detail = 0.0f;
			cc.skin_colour = 0.0f;
			frames(2, cc);
			const std::vector<float> skin_off = read_rgba16f(dev, ctx, image);
			snprintf(msg, sizeof(msg), "NR composition vs default: colour 0 %.2f dB, colour 2 %.2f dB, detail 2 %.2f dB, skin off %.2f dB (face %.2f vs input %.2f)",
				psnr_rgb(grey, full), psnr_rgb(vivid, full), psnr_rgb(strong, full), psnr_rgb(skin_off, full),
				psnr_cols(skin_off, full, sc.w, face_x, sc.w), psnr_cols(skin_off, reference, sc.w, face_x, sc.w));
			check(psnr_rgb(grey, full) < 90.0 && psnr_rgb(vivid, full) < 90.0 && psnr_rgb(strong, full) < 90.0, msg);
		}

		// Off and on again: the bridge device stays, so re-enabling only rebuilds the feature
		forge::nr::release();
		t0 = GetTickCount64();
		ok = frames(3, base);
		snprintf(msg, sizeof(msg), "NR re-enabled after release in %llu ms", static_cast<unsigned long long>(GetTickCount64() - t0));
		check(ok, msg);
		forge::nr::release();
		check(!forge::nr::get_status().pass[0].live, "NR features released");

		// Two DLSS outputs in turn: each keeps its views, nothing is rebuilt per frame
		{
			ID3D11Texture2D *image2 = make_tex(dev, sc.w, sc.h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET, nullptr, 0);
			bool alt_ok = true;
			for (int i = 0; i < 6; ++i)
			{
				ID3D11Texture2D *target = (i & 1) ? image2 : image;
				ctx->CopyResource(target, source);
				f.image = target;
				alt_ok = forge::nr::run(f, base) && alt_ok;
			}
			f.image = image;
			const double alt = psnr_rgb(read_rgba16f(dev, ctx, image2), reference);
			snprintf(msg, sizeof(msg), "NR on two alternating DLSS outputs: %s, PSNR %.2f dB vs input", alt_ok ? "ran" : "did not run", alt);
			check(alt_ok && alt > 18.0 && alt < 90.0, msg);
			image2->Release();
		}

		// The game's NGX shutdown: everything on the D3D11 side goes, the bridge device stays and re-attaches
		forge::nr::detach();
		t0 = GetTickCount64();
		ok = frames(3, base);
		const double after = psnr_rgb(read_rgba16f(dev, ctx, image), reference);
		snprintf(msg, sizeof(msg), "NR re-attached after detach in %llu ms, PSNR %.2f dB vs input", static_cast<unsigned long long>(GetTickCount64() - t0), after);
		check(ok && after > 18.0 && after < 90.0, msg);
		forge::nr::detach();
		check(!forge::nr::get_status().pass[0].live, "NR detached");
		image->Release();
		source->Release();
	}

	// Pixels of NVIDIA's on-screen indicator in the DLSS output: those that differ from DLAA of a static frame
	unsigned int indicator_pixels(ID3D11Device *dev, ID3D11DeviceContext *ctx, const scene &sc)
	{
		const std::vector<float> out = read_rgba16f(dev, ctx, sc.output);
		unsigned int changed = 0;
		for (size_t i = 0; i + 3 < out.size(); i += 4)
			for (int c = 0; c < 3; ++c)
				if (std::fabs(out[i + c] - sc.reference[i + c]) > 0.08f)
				{
					changed++;
					break;
				}
		return changed;
	}

	// NVIDIA's indicator on a DLSS feature the game creates with Forge's indicator setting
	unsigned int count_indicator_pixels(const ngx_api &api, ID3D11Device *dev, ID3D11DeviceContext *ctx, const scene &sc, bool on)
	{
		forge::settings s = forge::get_settings();
		s.indicator = on;
		forge::set_settings(s);
		Parameter *p = nullptr;
		api.alloc(&p);
		set_ui(p, name::creation_node_mask, 1);
		set_ui(p, name::visibility_node_mask, 1);
		set_ui(p, name::width, sc.w);
		set_ui(p, name::height, sc.h);
		set_ui(p, name::out_width, sc.w);
		set_ui(p, name::out_height, sc.h);
		set_i(p, name::perf_quality, perf_dlaa);
		set_i(p, name::create_flags, flag_mv_low_res | flag_depth_inverted);
		Handle *h = nullptr;
		if (!succeeded(api.create(ctx, feature_super_sampling, p, &h)) || h == nullptr)
		{
			api.destroy(p);
			return ~0u;
		}
		double ms = 0.0;
		Result r = result_success;
		run_frames(api, dev, ctx, sc, p, h, 6, &ms, &r);
		const unsigned int changed = indicator_pixels(dev, ctx, sc);
		api.release(h);
		api.destroy(p);
		s.indicator = false;
		forge::set_settings(s);
		return changed;
	}

	void test_ngx(ID3D11Device *dev, ID3D11DeviceContext *ctx)
	{
		HMODULE core = LoadLibraryW(L"_nvngx.dll");
		if (core == nullptr)
			core = LoadLibraryW(L"nvngx.dll");
		check(core != nullptr, "NGX core (_nvngx.dll) loaded from the prefix");
		if (core == nullptr)
			return;

		// Resolved through this executable's import of GetProcAddress, which Forge routes
		ngx_api api;
		api.init_project = reinterpret_cast<PFN_Init_ProjectID>(GetProcAddress(core, "NVSDK_NGX_D3D11_Init_ProjectID"));
		api.shutdown1 = reinterpret_cast<PFN_Shutdown1>(GetProcAddress(core, "NVSDK_NGX_D3D11_Shutdown1"));
		api.get_caps = reinterpret_cast<PFN_GetParams>(GetProcAddress(core, "NVSDK_NGX_D3D11_GetCapabilityParameters"));
		api.alloc = reinterpret_cast<PFN_GetParams>(GetProcAddress(core, "NVSDK_NGX_D3D11_AllocateParameters"));
		api.destroy = reinterpret_cast<PFN_DestroyParams>(GetProcAddress(core, "NVSDK_NGX_D3D11_DestroyParameters"));
		api.create = reinterpret_cast<PFN_CreateFeature>(GetProcAddress(core, "NVSDK_NGX_D3D11_CreateFeature"));
		api.release = reinterpret_cast<PFN_ReleaseFeature>(GetProcAddress(core, "NVSDK_NGX_D3D11_ReleaseFeature"));
		api.evaluate = reinterpret_cast<PFN_EvaluateFeature>(GetProcAddress(core, "NVSDK_NGX_D3D11_EvaluateFeature"));
		{
			const auto st = forge::ngx_hook::get_stats();
			char msg[128];
			snprintf(msg, sizeof(msg), "forge routed %u NGX entry points via GetProcAddress", st.resolved);
			check(st.resolved >= 6, msg);
		}

		wchar_t dir[MAX_PATH] = {};
		GetModuleFileNameW(nullptr, dir, MAX_PATH);
		*wcsrchr(dir, L'\\') = 0;
		const wchar_t *paths[] = { dir };
		feature_common_info info = {};
		info.paths = { paths, 1 };
		info.logging = { &ngx_log, getenv("FORGE_NGX_VERBOSE") ? 2 : 1, false };
		Result r = api.init_project("a0f57b54-1daf-4934-90ae-c4035c19df04", 0, "1.0", dir, dev, sdk_version_api, &info);
		char msg[256];
		snprintf(msg, sizeof(msg), "NGX D3D11 init (0x%08X)", r);
		check(succeeded(r), msg);
		if (!succeeded(r))
			return;

		Parameter *caps = nullptr;
		r = api.get_caps(&caps);
		int available = 0;
		if (caps != nullptr)
			get_i(caps, name::ss_available, &available);
		snprintf(msg, sizeof(msg), "capability parameters (0x%08X), SuperSampling.Available=%d", r, available);
		check(succeeded(r) && available == 1, msg);
		if (caps == nullptr || available != 1)
			return;

		// Optimal-settings query the way NGX_DLSS_GET_OPTIMAL_SETTINGS performs it
		auto query = [&](int perf, unsigned int &ow, unsigned int &mn, unsigned int &mx) {
			void *cb = nullptr;
			get_ptr(caps, name::optimal_settings_callback, &cb);
			set_ui(caps, name::width, 1920);
			set_ui(caps, name::height, 1080);
			set_i(caps, name::perf_quality, perf);
			set_i(caps, name::rtx_value, 0);
			const Result qr = cb != nullptr ? reinterpret_cast<PFN_OptimalSettingsCallback>(cb)(caps) : result_fail;
			ow = mn = mx = 0;
			get_ui(caps, name::out_width, &ow);
			get_ui(caps, name::dyn_min_w, &mn);
			get_ui(caps, name::dyn_max_w, &mx);
			return qr;
		};
		forge::settings s = forge::get_settings();
		s.lock = forge::render_lock::game;
		forge::set_settings(s);
		unsigned int ow, mn, mx;
		r = query(perf_max_quality, ow, mn, mx);
		snprintf(msg, sizeof(msg), "size query passthrough, Quality @1080p: NGX render %u (dynamic %u..%u)", ow, mn, mx);
		check(succeeded(r) && ow > 1000 && ow < 1920, msg);
		s.lock = forge::render_lock::native;
		forge::set_settings(s);
		r = query(perf_max_quality, ow, mn, mx);
		snprintf(msg, sizeof(msg), "size query locked native: render %u (dynamic %u..%u)", ow, mn, mx);
		check(succeeded(r) && ow == 1920 && mn == 1920 && mx == 1920, msg);
		s.lock = forge::render_lock::fixed;
		s.render_scale = 75.0f;
		forge::set_settings(s);
		r = query(perf_dlaa, ow, mn, mx);
		snprintf(msg, sizeof(msg), "size query locked 75 %%: render %u (dynamic %u..%u)", ow, mn, mx);
		check(succeeded(r) && ow == 1440 && mn == 1440 && mx == 1440, msg);
		s.lock = forge::render_lock::native;
		forge::set_settings(s);

		// Feature creation as the game requests it: 1080p DLAA, render-res vectors, inverted depth
		scene sc = make_scene(dev);
		Parameter *p = nullptr;
		api.alloc(&p);
		set_ui(p, name::creation_node_mask, 1);
		set_ui(p, name::visibility_node_mask, 1);
		set_ui(p, name::width, sc.w);
		set_ui(p, name::height, sc.h);
		set_ui(p, name::out_width, sc.w);
		set_ui(p, name::out_height, sc.h);
		set_i(p, name::perf_quality, perf_dlaa);
		set_i(p, name::create_flags, flag_mv_low_res | flag_depth_inverted);
		set_i(p, name::enable_output_subrects, 0);
		Handle *h = nullptr;
		r = api.create(ctx, feature_super_sampling, p, &h);
		snprintf(msg, sizeof(msg), "DLSS create through Forge (0x%08X)", r);
		check(succeeded(r) && h != nullptr, msg);
		if (h == nullptr)
			return;
		{
			const auto st = forge::ngx_hook::get_stats();
			unsigned int kept = 0;
			get_ui(p, name::preset_hint[perf_dlaa], &kept);
			snprintf(msg, sizeof(msg), "preset K applied to the feature (%c), game parameter object restored (%u)", preset_letter(st.feature.eff_presets[perf_dlaa]), kept);
			check(st.feature.eff_presets[perf_dlaa] == 11 && kept == 0, msg);
		}

		stage("DLAA 1.00x preset K", api, dev, ctx, sc, p, h, 1920, 28.0);
		{
			// Written off before NGX init (as the add-on does at load); --indicator 1024 checks the on case
			const unsigned int off = count_indicator_pixels(api, dev, ctx, sc, false);
			snprintf(msg, sizeof(msg), "DLSS indicator off: %u pixels", off);
			check(off == 0, msg);
		}

		s.preset = 13;
		forge::set_settings(s);
		stage("preset M", api, dev, ctx, sc, p, h, 1920, 28.0);

		s.preset = 11;
		s.exposure = forge::exposure_mode::on;
		forge::set_settings(s);
		stage("auto-exposure on", api, dev, ctx, sc, p, h, 1920, 20.0);

		s.exposure = forge::exposure_mode::game;
		s.enabled = false;
		forge::set_settings(s);
		stage("forge inactive (game request verbatim)", api, dev, ctx, sc, p, h, 1920, 28.0);
		{
			const auto st = forge::ngx_hook::get_stats();
			check(st.feature.eff_presets[perf_dlaa] == 0, "inactive forge runs the game's own preset hints");
		}

		s.enabled = true;
		forge::set_settings(s);

		// NR inside the game's DLSS evaluate
		auto stage_nr = [&](const char *label) {
			s.nr = forge::nr::config();
			s.nr.device = test_device();
			forge::set_settings(s);
			double ms = 0.0;
			Result rr = result_success;
			run_frames(api, dev, ctx, sc, p, h, 24, &ms, &rr);
			const std::vector<float> plain = read_rgba16f(dev, ctx, sc.output);
			const uint64_t before = forge::nr::get_status().frames;
			s.nr.enabled = true;
			forge::set_settings(s);
			const double psnr_in = run_frames(api, dev, ctx, sc, p, h, 24, &ms, &rr);
			const std::vector<float> edited = read_rgba16f(dev, ctx, sc.output);
			const auto ns = forge::nr::get_status();
			const auto st = forge::ngx_hook::get_stats();
			const double vs_plain = psnr_rgb(edited, plain);
			char m[400];
			snprintf(m, sizeof(m), "%s: DLSS %ux%u (%s), NR ran %llu of 24 frames, %u pass, model %ux%u on %ux%u, guides %ux%u, %.3f ms GPU, %.2f dB vs NR off, %.2f dB vs input%s%s",
				label, st.feature.eff_out_w, st.feature.eff_out_h, st.feature.using_own_feature ? "forge feature" : "game feature",
				static_cast<unsigned long long>(ns.frames - before), ns.passes, ns.work_w, ns.work_h, ns.frame_w, ns.frame_h, ns.guide_w, ns.guide_h,
				ns.gpu_ms, vs_plain, psnr_in, ns.reason[0] ? "; " : "", ns.reason);
			check(succeeded(rr) && ns.active && ns.frames - before == 24 && ns.frame_w == sc.w && ns.frame_h == sc.h && vs_plain < 60.0 && psnr_in > 12.0, m);
		};
		stage_nr("NR in the game's DLSS evaluate");
		{
			s.nr.enabled = false;
			forge::set_settings(s);
			double ms = 0.0;
			Result rr = result_success;
			run_frames(api, dev, ctx, sc, p, h, 4, &ms, &rr);
			const auto ns = forge::nr::get_status();
			check(!ns.active && !ns.pass[0].live, "NR switched off in the settings: model released at the next evaluate");
		}
		test_nr(api, dev, ctx, sc, caps);
		r = api.release(h);
		snprintf(msg, sizeof(msg), "release through Forge (0x%08X)", r);
		check(succeeded(r), msg);
		api.destroy(p);
		api.destroy(caps);
		// NVSDK_NGX_D3D11_Shutdown1 called directly on _nvngx.dll faults inside the runtime under Proton
		// even without Forge (see --raw); the game reaches it through its static NGX library instead.

		sc.color->Release();
		sc.depth->Release();
		sc.mv->Release();
		sc.output->Release();
	}
}

// NVIDIA's DLSS indicator with ShowDlssIndicator written before NGX initialises, in a fresh process
int indicator_probe(ID3D11Device *dev, ID3D11DeviceContext *ctx, DWORD value)
{
	forge::ngx_hook::claim_indicator();
	forge::ngx_hook::install(GetModuleHandleW(nullptr));
	HKEY key = nullptr;
	RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
	RegSetValueExW(key, L"ShowDlssIndicator", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value), sizeof(value));
	RegCloseKey(key);
	HMODULE core = LoadLibraryW(L"_nvngx.dll");
	ngx_api api;
	api.init_project = reinterpret_cast<PFN_Init_ProjectID>(GetProcAddress(core, "NVSDK_NGX_D3D11_Init_ProjectID"));
	api.get_caps = reinterpret_cast<PFN_GetParams>(GetProcAddress(core, "NVSDK_NGX_D3D11_GetCapabilityParameters"));
	api.alloc = reinterpret_cast<PFN_GetParams>(GetProcAddress(core, "NVSDK_NGX_D3D11_AllocateParameters"));
	api.destroy = reinterpret_cast<PFN_DestroyParams>(GetProcAddress(core, "NVSDK_NGX_D3D11_DestroyParameters"));
	api.create = reinterpret_cast<PFN_CreateFeature>(GetProcAddress(core, "NVSDK_NGX_D3D11_CreateFeature"));
	api.release = reinterpret_cast<PFN_ReleaseFeature>(GetProcAddress(core, "NVSDK_NGX_D3D11_ReleaseFeature"));
	api.evaluate = reinterpret_cast<PFN_EvaluateFeature>(GetProcAddress(core, "NVSDK_NGX_D3D11_EvaluateFeature"));
	wchar_t dir[MAX_PATH] = {};
	GetModuleFileNameW(nullptr, dir, MAX_PATH);
	*wcsrchr(dir, L'\\') = 0;
	const wchar_t *paths[] = { dir };
	feature_common_info info = {};
	info.paths = { paths, 1 };
	info.logging = { &ngx_log, 2, false };
	const Result r = api.init_project("a0f57b54-1daf-4934-90ae-c4035c19df04", 0, "1.0", dir, dev, sdk_version_api, &info);
	scene sc = make_scene(dev);
	// count_indicator_pixels rewrites the value itself; the first count keeps what was there at init
	unsigned int first = ~0u;
	{
		Parameter *p = nullptr;
		api.alloc(&p);
		set_ui(p, name::creation_node_mask, 1);
		set_ui(p, name::visibility_node_mask, 1);
		set_ui(p, name::width, sc.w);
		set_ui(p, name::height, sc.h);
		set_ui(p, name::out_width, sc.w);
		set_ui(p, name::out_height, sc.h);
		set_i(p, name::perf_quality, perf_dlaa);
		set_i(p, name::create_flags, flag_mv_low_res | flag_depth_inverted);
		Handle *h = nullptr;
		if (succeeded(api.create(ctx, feature_super_sampling, p, &h)) && h != nullptr)
		{
			double ms = 0.0;
			Result last = result_success;
			run_frames(api, dev, ctx, sc, p, h, 6, &ms, &last);
			const std::vector<float> out = read_rgba16f(dev, ctx, sc.output);
			if (FILE *dumpf = _wfopen(L"indicator-out.f32", L"wb"))
			{
				fwrite(out.data(), sizeof(float), out.size(), dumpf);
				fclose(dumpf);
			}
			first = 0;
			for (size_t i = 0; i + 3 < out.size(); i += 4)
				for (int c = 0; c < 3; ++c)
					if (std::fabs(out[i + c] - sc.reference[i + c]) > 0.08f)
					{
						first++;
						break;
					}
			api.release(h);
		}
		api.destroy(p);
	}
	char msg[160];
	snprintf(msg, sizeof(msg), "DLSS indicator with ShowDlssIndicator=%lu written before NGX init (0x%08X): %u pixels", static_cast<unsigned long>(value), r, first);
	check(first != ~0u && (value == 1024 ? first > 500 : first == 0), msg);
	forge::ngx_hook::set_indicator_registry(false);
	return g_failures ? 1 : 0;
}

// Minimal NGX lifecycle without Forge, to separate runtime behaviour from Forge's.
int raw_lifecycle(ID3D11Device *dev, ID3D11DeviceContext *ctx)
{
	HMODULE core = LoadLibraryW(L"_nvngx.dll");
	auto init = reinterpret_cast<PFN_Init_ProjectID>(GetProcAddress(core, "NVSDK_NGX_D3D11_Init_ProjectID"));
	auto shutdown1 = reinterpret_cast<PFN_Shutdown1>(GetProcAddress(core, "NVSDK_NGX_D3D11_Shutdown1"));
	auto alloc = reinterpret_cast<PFN_GetParams>(GetProcAddress(core, "NVSDK_NGX_D3D11_AllocateParameters"));
	auto destroy = reinterpret_cast<PFN_DestroyParams>(GetProcAddress(core, "NVSDK_NGX_D3D11_DestroyParameters"));
	auto create = reinterpret_cast<PFN_CreateFeature>(GetProcAddress(core, "NVSDK_NGX_D3D11_CreateFeature"));
	auto release = reinterpret_cast<PFN_ReleaseFeature>(GetProcAddress(core, "NVSDK_NGX_D3D11_ReleaseFeature"));
	wchar_t dir[MAX_PATH] = {};
	GetModuleFileNameW(nullptr, dir, MAX_PATH);
	*wcsrchr(dir, L'\\') = 0;
	const wchar_t *paths[] = { dir };
	feature_common_info info = {};
	info.paths = { paths, 1 };
	Result r = init("a0f57b54-1daf-4934-90ae-c4035c19df04", 0, "1.0", dir, dev, sdk_version_api, &info);
	FORGE_LOG("raw init 0x%08X", r);
	Parameter *p = nullptr;
	alloc(&p);
	set_ui(p, name::creation_node_mask, 1);
	set_ui(p, name::visibility_node_mask, 1);
	set_ui(p, name::width, 1920);
	set_ui(p, name::height, 1080);
	set_ui(p, name::out_width, 3840);
	set_ui(p, name::out_height, 2160);
	set_i(p, name::perf_quality, perf_max_perf);
	set_i(p, name::create_flags, flag_mv_low_res | flag_depth_inverted);
	Handle *h = nullptr;
	r = create(ctx, feature_super_sampling, p, &h);
	FORGE_LOG("raw create 0x%08X", r);
	r = release(h);
	FORGE_LOG("raw release 0x%08X", r);
	destroy(p);
	r = shutdown1(dev);
	FORGE_LOG("raw shutdown1 0x%08X", r);
	FORGE_LOG("raw lifecycle finished");
	return 0;
}

int main(int argc, char **argv)
{
	wchar_t log_path[MAX_PATH] = {};
	GetModuleFileNameW(nullptr, log_path, MAX_PATH);
	wcscpy(wcsrchr(log_path, L'.'), L".log");
	forge::log::open(log_path);
	setvbuf(stdout, nullptr, _IONBF, 0);

	const bool raw = argc > 1 && std::strcmp(argv[1], "--raw") == 0;
	const bool indicator = argc > 2 && std::strcmp(argv[1], "--indicator") == 0;
	if (!raw && !indicator)
	{
		forge::ngx_hook::claim_indicator(); // as the add-on does when ReShade loads it
		forge::ngx_hook::set_indicator_registry(false);
	}
	if (indicator)
	{
		ID3D11Device *dev = nullptr;
		ID3D11DeviceContext *ctx = nullptr;
		const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_1;
		D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
		return indicator_probe(dev, ctx, static_cast<DWORD>(std::strtoul(argv[2], nullptr, 0)));
	}
	if (!raw)
		check(forge::ngx_hook::install(GetModuleHandleW(nullptr)), "GetProcAddress import routed");

	ID3D11Device *dev = nullptr;
	ID3D11DeviceContext *ctx = nullptr;
	const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_1;
	HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
	check(SUCCEEDED(hr), "D3D11 device (DXVK) created");
	if (FAILED(hr))
		return 2;

	if (raw)
		return raw_lifecycle(dev, ctx);
	test_ngx(dev, ctx);

	ctx->Release();
	dev->Release();
	printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);
	FORGE_LOG("%d failure(s)", g_failures);
	forge::log::close();
	return g_failures ? 1 : 0;
}
