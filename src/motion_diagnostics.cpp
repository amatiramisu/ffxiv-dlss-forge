#include "motion_diagnostics.h"
#include "motion_diagnostic_params.h"
#include "motion_diagnostic_shaders.h"
#include "ngx_hook.h"
#include "nr_bridge.h"
#include "settings.h"
#include "log.h"
#include <d3d11_1.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace forge::motion_diagnostics;
using namespace ngx;

namespace
{
	constexpr unsigned int ring_size = 12;
	const char *const tests[] = { "static", "camera", "character", "resolution", "upscaling", "nr", "reshade", "other" };
	const char *const hypotheses[] = { "zero", "raw", "raw_plus_jitter_delta", "raw_minus_jitter_delta", "reverse", "half", "double", "texture_units" };
	template <typename T> void drop(T *&p) { if (p) { p->Release(); p = nullptr; } }

	DXGI_FORMAT view_format(DXGI_FORMAT f)
	{
		switch (f)
		{
		case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
		case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
		case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
		case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
		case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
		case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
		case DXGI_FORMAT_B8G8R8X8_TYPELESS: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_UNORM;
		case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
		case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
		case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
		case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
		default: return f;
		}
	}
	DXGI_FORMAT copy_format(DXGI_FORMAT f)
	{
		switch (f)
		{
		case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_TYPELESS;
		case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_TYPELESS;
		case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24G8_TYPELESS;
		case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32G8X24_TYPELESS;
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
		case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_TYPELESS;
		default: return f;
		}
	}
	unsigned int bytes_per_pixel(DXGI_FORMAT f)
	{
		switch (view_format(f))
		{
		case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
		case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return 8;
		case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM: return 2;
		case DXGI_FORMAT_R8_UNORM: return 1;
		default: return 4;
		}
	}

	struct resource_info
	{
		uintptr_t identity = 0;
		D3D11_TEXTURE2D_DESC desc = {};
		unsigned int x = 0, y = 0, requested_w = 0, requested_h = 0, w = 0, h = 0;
		HRESULT result = E_PENDING;
		bool describe(ID3D11Resource *source)
		{
			identity = reinterpret_cast<uintptr_t>(source); w = h = 0; desc = {};
			if (!source) { result = E_POINTER; return false; }
			ID3D11Texture2D *t = nullptr;
			result = source->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&t));
			if (FAILED(result)) return false;
			t->GetDesc(&desc); t->Release(); result = E_PENDING;
			return true;
		}
		bool operator==(const resource_info &b) const
		{
			return desc.Width == b.desc.Width && desc.Height == b.desc.Height && desc.Format == b.desc.Format &&
				x == b.x && y == b.y && w == b.w && h == b.h && requested_w == b.requested_w && requested_h == b.requested_h;
		}
	};
	struct texture
	{
		ID3D11Texture2D *image = nullptr;
		ID3D11ShaderResourceView *view = nullptr;
		unsigned int w = 0, h = 0, origin_x = 0, origin_y = 0;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		void release() { drop(view); drop(image); w = h = origin_x = origin_y = 0; }
		bool capture(ID3D11Device *device, ID3D11DeviceContext *ctx, ID3D11Resource *source, resource_info &info)
		{
			if (!info.describe(source)) return false;
			info.result = S_OK;
			ID3D11Device *source_device = nullptr;
			source->GetDevice(&source_device);
			bool same_device = device == source_device;
			if (!same_device && source_device != nullptr)
			{
				// ReShade's ID3D11Resource::GetDevice hook returns its proxy device; the proxy's immediate context wraps ctx
				ID3D11DeviceContext *immediate = nullptr;
				source_device->GetImmediateContext(&immediate);
				same_device = immediate != nullptr && forge::ngx_hook::native_context(immediate) == ctx;
				drop(immediate);
			}
			drop(source_device);
			if (!same_device || info.desc.ArraySize != 1 || info.desc.MipLevels != 1 || info.desc.SampleDesc.Count != 1 || info.x >= info.desc.Width || info.y >= info.desc.Height || info.requested_w == 0 || info.requested_h == 0)
			{ info.result = E_INVALIDARG; return false; }
			info.w = (std::min)(info.requested_w, info.desc.Width - info.x);
			info.h = (std::min)(info.requested_h, info.desc.Height - info.y);
			const DXGI_FORMAT storage = copy_format(info.desc.Format);
			const bool full = (info.desc.BindFlags & D3D11_BIND_DEPTH_STENCIL) != 0;
			const unsigned int copy_w = full ? info.desc.Width : info.w, copy_h = full ? info.desc.Height : info.h;
			if (image == nullptr || w != copy_w || h != copy_h || format != storage)
			{
				release();
				D3D11_TEXTURE2D_DESC d = {};
				d.Width = copy_w; d.Height = copy_h; d.MipLevels = d.ArraySize = 1;
				d.Format = storage; d.SampleDesc.Count = 1; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
				info.result = device->CreateTexture2D(&d, nullptr, &image);
				D3D11_SHADER_RESOURCE_VIEW_DESC v = {};
				v.Format = view_format(info.desc.Format); v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; v.Texture2D.MipLevels = 1;
				if (SUCCEEDED(info.result)) info.result = device->CreateShaderResourceView(image, &v, &view);
				if (FAILED(info.result)) { release(); return false; }
				w = copy_w; h = copy_h; format = storage;
			}
			origin_x = full ? info.x : 0; origin_y = full ? info.y : 0;
			const D3D11_BOX box = { info.x, info.y, 0, info.x + info.w, info.y + info.h, 1 };
			if (full) ctx->CopyResource(image, source);
			else ctx->CopySubresourceRegion(image, 0, 0, 0, 0, source, 0, &box);
			return true;
		}
		double memory() const { return static_cast<double>(w) * h * bytes_per_pixel(format); }
	};
	struct frame_info
	{
		uint64_t id = 0, present = 0, config = 0;
		double time_ms = 0.0, through_sr_ms = 0.0, hook_ms = 0.0;
		HRESULT final_result = E_PENDING;
		unsigned int thread = 0, test = 0, reads = 0, continuity = 0;
		feature_info feature;
		unsigned int param_w = 0, param_h = 0, render_w = 0, render_h = 0;
		float jitter[2] = {}, mv_scale[2] = { 1, 1 }, pre_exposure = 1, exposure_scale = 1, frame_ms = 0;
		uintptr_t exposure_texture = 0, context = 0;
		int game_reset = 0;
		bool effective_reset = false, numeric = false, gpu_ready = false, input_ok = false, motion_ok = false, depth_ok = false, output_ok = false, final_ok = false, exposure_ok = false;
		bool input_history = false, output_history = false;
		Result result = result_fail;
		resource_info resources[5]; // input, motion, depth, SR output, exposure
	};
	struct readback
	{
		ID3D11Texture2D *stats = nullptr, *samples = nullptr;
		ID3D11Query *done = nullptr;
		bool pending = false, sampled = false;
		unsigned int phase = 0, color_space = 0;
		uint64_t event = 0;
		resource_info surface;
		forge::nr::status nr;
		frame_info info;
		void release() { drop(stats); drop(samples); drop(done); pending = false; }
	};
	struct state
	{
		ID3D11Device *device = nullptr;
		ID3DDeviceContextState *context_state = nullptr;
		ID3D11ComputeShader *shader = nullptr;
		ID3D11Buffer *cb = nullptr;
		ID3D11SamplerState *linear = nullptr;
		ID3D11Texture2D *stats = nullptr, *sample_image = nullptr;
		ID3D11UnorderedAccessView *stats_uav = nullptr, *samples_uav = nullptr;
		texture input[2], output[2], motion, depth, final_image, exposure, surface;
		readback slots[ring_size];
		unsigned int current = 0, next_slot = 0;
		uint64_t present_id = 0, config_id = 0, event_id = 0;
		unsigned int color_space = 0;
		bool requested = false, in_flight = false, have_previous = false, saw_evaluate = false;
		frame_info frame, previous;
		ID3D11Resource *output_source = nullptr;
		FILE *frames = nullptr, *regions_file = nullptr, *samples_file = nullptr, *notes = nullptr, *surfaces = nullptr;
		LARGE_INTEGER start = {}, frequency = {};
		std::string configuration;
		status st;
	} g;
	SRWLOCK status_lock = SRWLOCK_INIT;
	status published_status;
	measurement published_measurement;

	void publish()
	{
		g.st.memory_mib = (g.input[0].memory() + g.input[1].memory() + g.output[0].memory() + g.output[1].memory() +
			g.motion.memory() + g.depth.memory() + g.final_image.memory() + g.exposure.memory() + g.surface.memory()) / (1024.0 * 1024.0);
		AcquireSRWLockExclusive(&status_lock);
		published_status = g.st;
		ReleaseSRWLockExclusive(&status_lock);
	}
	void issue(const char *text)
	{
		if (std::strcmp(g.st.issue, text) != 0)
		{
			FORGE_LOG("motion capture: %s", text);
			if (g.notes && !ferror(g.notes)) fprintf(g.notes, "issue frame=%llu: %s\n", static_cast<unsigned long long>(g.st.frames), text);
		}
		snprintf(g.st.issue, sizeof(g.st.issue), "%s", text);
	}
	void release_gpu()
	{
		for (auto &s : g.slots) s.release();
		for (auto &t : g.input) t.release();
		for (auto &t : g.output) t.release();
		g.motion.release(); g.depth.release(); g.final_image.release(); g.exposure.release(); g.surface.release();
		drop(g.stats_uav); drop(g.samples_uav); drop(g.stats); drop(g.sample_image);
		drop(g.linear); drop(g.shader); drop(g.cb); drop(g.context_state); drop(g.device);
		g.have_previous = false;
		g.next_slot = 0;
	}
	bool ensure_gpu(ID3D11DeviceContext *ctx)
	{
		if (ctx == nullptr || ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
		{ issue("motion capture requires a D3D11.1 immediate context"); return false; }
		ID3D11Device *device = nullptr;
		ctx->GetDevice(&device);
		if (device == nullptr) return false;
		if (device == g.device && g.context_state != nullptr) { device->Release(); return true; }
		for (auto &s : g.slots) if (s.pending)
		{
			++g.st.dropped;
			if (g.notes) fprintf(g.notes, "device_change_drop probe=%llu frame=%llu\n", static_cast<unsigned long long>(s.event), static_cast<unsigned long long>(s.info.id));
		}
		release_gpu();
		g.device = device;
		ID3D11Device1 *d1 = nullptr;
		const D3D_FEATURE_LEVEL level = device->GetFeatureLevel();
		if (g.notes) fprintf(g.notes, "D3D11 device=0x%llX feature_level=0x%X\n", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(device)), level);
		bool ok = SUCCEEDED(device->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void **>(&d1)));
		ok = ok && SUCCEEDED(d1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr, &g.context_state));
		drop(d1);
		D3D11_BUFFER_DESC bd = {}; bd.ByteWidth = sizeof(shader_params); bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		D3D11_SAMPLER_DESC sd = {}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
		ok = ok && SUCCEEDED(device->CreateComputeShader(motion_probe, sizeof(motion_probe), nullptr, &g.shader)) &&
			SUCCEEDED(device->CreateBuffer(&bd, nullptr, &g.cb)) && SUCCEEDED(device->CreateSamplerState(&sd, &g.linear));
		D3D11_TEXTURE2D_DESC d = {}; d.Width = columns; d.Height = regions; d.MipLevels = d.ArraySize = 1;
		d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; d.SampleDesc.Count = 1; d.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		ok = ok && SUCCEEDED(device->CreateTexture2D(&d, nullptr, &g.stats)) && SUCCEEDED(device->CreateUnorderedAccessView(g.stats, nullptr, &g.stats_uav));
		d.Width = sample_w; d.Height = sample_h * sample_planes;
		ok = ok && SUCCEEDED(device->CreateTexture2D(&d, nullptr, &g.sample_image)) && SUCCEEDED(device->CreateUnorderedAccessView(g.sample_image, nullptr, &g.samples_uav));
		d.BindFlags = 0; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		D3D11_QUERY_DESC q = { D3D11_QUERY_EVENT, 0 };
		for (auto &s : g.slots)
		{
			d.Width = columns; d.Height = regions;
			ok = ok && SUCCEEDED(device->CreateTexture2D(&d, nullptr, &s.stats));
			d.Width = sample_w; d.Height = sample_h * sample_planes;
			ok = ok && SUCCEEDED(device->CreateTexture2D(&d, nullptr, &s.samples)) && SUCCEEDED(device->CreateQuery(&q, &s.done));
		}
		if (!ok) { issue("D3D11 diagnostic resources could not be created"); release_gpu(); }
		return ok;
	}
	void modules(FILE *file)
	{
		for (const wchar_t *name : { L"_nvngx.dll", L"nvngx_dlss.dll", L"nvngx_dlssnr.dll", L"d3d11.dll", L"dxgi.dll", L"nvapi64.dll", L"d3d12.dll", L"d3d12core.dll" })
		{
			HMODULE module = GetModuleHandleW(name);
			if (!module) { fprintf(file, "module %ls: unloaded\n", name); continue; }
			wchar_t path[MAX_PATH] = {};
			GetModuleFileNameW(module, path, MAX_PATH);
			DWORD unused = 0, bytes = GetFileVersionInfoSizeW(path, &unused);
			std::vector<unsigned char> data(bytes);
			VS_FIXEDFILEINFO *version = nullptr; UINT size = 0;
			if (bytes && GetFileVersionInfoW(path, 0, bytes, data.data()) && VerQueryValueW(data.data(), L"\\", reinterpret_cast<void **>(&version), &size) && size >= sizeof(*version))
				fprintf(file, "module %ls: %u.%u.%u.%u\n", name, HIWORD(version->dwFileVersionMS), LOWORD(version->dwFileVersionMS), HIWORD(version->dwFileVersionLS), LOWORD(version->dwFileVersionLS));
			else fprintf(file, "module %ls: loaded, version unavailable\n", name);
		}
	}
	bool open_capture()
	{
		g.st = {};
		HMODULE module = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&open_capture), &module);
		wchar_t module_path[MAX_PATH] = {};
		if (!GetModuleFileNameW(module, module_path, MAX_PATH)) return false;
		wchar_t *slash = wcsrchr(module_path, L'\\');
		if (!slash) return false;
		*slash = 0;
		std::wstring root = std::wstring(module_path) + L"\\forge-captures";
		if (!CreateDirectoryW(root.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
		SYSTEMTIME time; GetSystemTime(&time);
		std::wstring folder;
		bool created = false;
		for (unsigned int id = 1; id <= 100; ++id)
		{
			wchar_t suffix[96];
			swprintf(suffix, 96, L"motion-%04u%02u%02u-%02u%02u%02u-%02u", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, id);
			folder = root + L"\\" + suffix;
			if (CreateDirectoryW(folder.c_str(), nullptr))
			{
				snprintf(g.st.folder, sizeof(g.st.folder), "forge-captures/%ls", suffix);
				created = true; break;
			}
			if (GetLastError() != ERROR_ALREADY_EXISTS) break;
		}
		if (!created) return false;
		g.frames = _wfopen((folder + L"\\frames.csv").c_str(), L"wb");
		g.regions_file = _wfopen((folder + L"\\regions.csv").c_str(), L"wb");
		g.samples_file = _wfopen((folder + L"\\samples.bin").c_str(), L"wb");
		g.notes = _wfopen((folder + L"\\capture.txt").c_str(), L"wb");
		g.surfaces = _wfopen((folder + L"\\presentation.csv").c_str(), L"wb");
		if (!g.frames || !g.regions_file || !g.samples_file || !g.notes || !g.surfaces) return false;
		for (FILE *f : { g.frames, g.regions_file, g.samples_file, g.notes, g.surfaces }) setvbuf(f, nullptr, _IOFBF, 64 * 1024);
		fprintf(g.notes, "Forge motion capture schema 1. Build %s %s. UTC %04u-%02u-%02u %02u:%02u:%02u.\n", __DATE__, __TIME__, time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
		fputs("Captures come from the game's NGX arguments. Input colour, motion and depth are copied before NR and SR. SR output is copied before after-upscaling NR. Final is the SR output resource after the hook's NR work; Present and ReShade effects happen later.\n", g.notes);
		fputs("Every evaluation gets a metadata row. GPU statistics use 9 screen regions, each sampled on a 24 by 12 grid. Regions 0..8 run left to right, top to bottom. RGB errors use stored values and bilinear history sampling. Comparisons exclude out-of-bounds and unusable samples. Numeric statistics use finite values with magnitude below 1e10.\n", g.notes);
		fputs("Motion pixel statistics multiply stored values by the supplied SR scale, which declares render-pixel units. Jitter delta is previous normalized jitter minus current normalized jitter. Hypotheses: zero, raw, raw+delta, raw-delta, -raw, raw*0.5, raw*2, raw scaled by the physical motion texture size. Resolved-image motion samples use UV plus current jitter. Labels describe candidates; the capture makes no automatic verdict about the game.\n", g.notes);
		fputs("Raw samples are taken every 8 evaluations and on continuity changes. samples.bin starts with 8 bytes FGMVS001, then 6 little-endian u32 values: schema, width, height, planes, channels, interval. Each record is u64 probe, u64 frame, u64 present counter, u32 payload bytes, u32 flags (bit 0 input history, bit 1 output history, bits 8..15 phase: 0 NGX, 1 present, 2 after effects), then tightly packed little-endian float32 rows. Plane 0: raw raster MV.xy and resolved-position MV.xy. Planes 1..3: input RGBA, SR RGBA, final RGBA. Plane 4: raw depth, input UV.xy, validity bits (input 1, SR 2, final 4, raster MV 8, resolved MV 16). Resource availability is in frames.csv; null shader reads return zero.\n", g.notes);
		fputs("Continuity bits: 1 first or failed previous evaluation, 2 feature/context change, 4 presentation gap, 8 geometry/format change, 16 motion metadata change, 32 game or forced reset, 64 bad numeric parameters, 128 failed SR. GPU readback drops are counted separately. Value -1 denotes an unavailable averaged metric.\n", g.notes);
		modules(g.notes);
		fputs("frame,present,time_ms,thread,test,config,game_handle,live_handle,game_in_w,game_in_h,game_out_w,game_out_h,effective_w,effective_h,game_perf,effective_perf,game_flags,effective_flags,param_render_w,param_render_h,render_w,render_h,jitter_x,jitter_y,mv_scale_x,mv_scale_y,reads,pre_exposure,exposure_scale,frame_ms,exposure_texture,game_reset,forced_reset,effective_reset,sr_result,continuity,numeric,input_ok,motion_ok,depth_ok,output_ok,final_ok,input_history,output_history,probe_queued,sampled,nr_active,nr_passes,nr_reset,nr_result,nr_work_w,nr_work_h,nr_gpu_ms,context,exposure_ok,nr_frame_counter,probe,through_sr_ms,hook_ms,final_result,output_subrects", g.frames);
		for (const char *n : { "input", "motion", "depth", "output", "exposure" })
			fprintf(g.frames, ",%s_id,%s_texture_w,%s_texture_h,%s_format,%s_bind,%s_mips,%s_array,%s_samples,%s_x,%s_y,%s_requested_w,%s_requested_h,%s_copy_w,%s_copy_h,%s_result", n,n,n,n,n,n,n,n,n,n,n,n,n,n,n);
		fputs(",generation,recreates,nr_stage,nr_temporal,nr_guide_w,nr_guide_h,nr_sync_ms", g.frames);
		for (unsigned int i = 0; i < 6; ++i) fprintf(g.frames, ",game_preset_%u,effective_preset_%u", i, i);
		fputc('\n', g.frames);
		fputs("frame,present,probe,region,total,mv_valid,mv_zero,mv_below_0_001px,mv_raw_x,mv_raw_y,mv_raw_rms_x,mv_raw_rms_y,mv_px_x,mv_px_y,mv_px_rms_x,mv_px_rms_y,mv_raw_min_x,mv_raw_min_y,mv_raw_max_x,mv_raw_max_y,mv_px_max,input_bad,sr_bad,final_bad,input_mean,sr_mean,final_mean,input_rms,sr_rms,final_rms,depth_mean,depth_min,depth_max,depth_valid,depth_zero,input_min,input_max,sr_min,sr_max,final_min,final_max,mv_over_one_uv,final_mae,final_luma_change,final_stops,exposure_mean,exposure_valid", g.regions_file);
		for (const char *image : { "input", "sr" }) for (const char *h : hypotheses)
			fprintf(g.regions_file, ",%s_%s_mse,%s_%s_mae,%s_%s_count,%s_%s_rejected", image,h,image,h,image,h,image,h);
		fputc('\n', g.regions_file);
		fputs("probe,frame,present_counter,phase,time_ms,region,config,color_space,source,width,height,format,bind,result,count,bad,mean,rms,min,max,sampled,nr_frame_counter,nr_active,nr_stage,nr_temporal,nr_passes,nr_reset,nr_result,nr_work_w,nr_work_h,nr_guide_w,nr_guide_h,nr_gpu_ms,nr_sync_ms\n", g.surfaces);
		fputs("presentation.csv samples the swapchain before ReShade effects, after Present-stage NR, and each ReShade finish-effects event before its overlay. These rows reference the latest completed NGX evaluation; frame=0 means none in this capture. The present counter advances in the native present callback. ReShade can invoke effects at other pipeline points, so use phase, time and probe ID together. Color-space values: 0 unknown, 1 sRGB, 2 scRGB, 3 PQ, 4 HLG; after-effects rows use the latest observed swapchain color space. Surface sample records use plane 3 for RGBA; other image planes are unavailable.\n", g.notes);
		fputs("CPU timing columns through_sr_ms and hook_ms measure elapsed time since the input capture began, through SR and through hook NR respectively. These include capture copies and CPU scheduling. Sampled formats use float views for typeless float families and UNORM views for sRGB colour. Depth-stencil sources are copied in full and sampled at their recorded subrect. Other copies contain the recorded rectangle. Preset suffixes use NGX performance IDs 0..5.\n", g.notes);
		fputs("Parameter-read bits: jitter X=1, Y=2, MV scale X=4, Y=8, pre-exposure=16, exposure scale=32, frame time=64. Unset bits indicate that the corresponding getter failed and its API default was recorded. Exposure texture statistics read its first texel.\n", g.notes);
		fwrite("FGMVS001", 1, 8, g.samples_file);
		const uint32_t header[] = { 1, sample_w, sample_h, sample_planes, 4, sample_interval };
		fwrite(header, sizeof(header), 1, g.samples_file);
		QueryPerformanceCounter(&g.start); QueryPerformanceFrequency(&g.frequency);
		g.config_id = 0; g.event_id = 0; g.previous = {}; g.configuration.clear(); g.have_previous = false; g.saw_evaluate = false; g.current = 0;
		g.st.active = true;
		FORGE_LOG("motion capture started: %s (raw samples %ux%u every %u evaluations)", g.st.folder, sample_w, sample_h, sample_interval);
		return true;
	}
	void configuration(const forge::settings &s)
	{
		char b[768];
		snprintf(b, sizeof(b), "test=%d forge=%d guest=%d lock=%d render_scale=%.9g preset=%u exposure=%d desktop=%ux%u lod=%.9g NR=%d stage=%d device=%d passes=%u work_scale=%.9g temporal=%d apply=%d compare=%d split=%.9g swap=%d debug=%d mask=%d blend=%d mask_show=%d estimate=%d white=%.9g",
			s.motion_capture_test,s.enabled,s.guest_mode,static_cast<int>(s.lock),s.render_scale,s.preset,static_cast<int>(s.exposure),s.desktop_w,s.desktop_h,s.lod_bias,
			s.nr.enabled,static_cast<int>(s.nr.selected_stage()),s.nr.device,s.nr.pass_count(),s.nr.work_scale,s.nr.temporal,s.nr.apply,s.nr.compare,s.nr.split,s.nr.swap,s.nr.debug_view,s.nr.mask.enabled,s.nr.mask.blend,s.nr.mask.show,s.nr.show_automask,s.nr.paper_white);
		std::string text(b);
		for (unsigned int i = 0; i < s.nr.pass_count(); ++i)
		{
			const auto t = s.nr.resolve(i);
			snprintf(b, sizeof(b), "\npass=%u preset=%u style=%u intensity=%.9g structure=%.9g tone=%.9g skin=%.9g automask=%d", i,t.preset,t.style,t.intensity,t.structure,t.tone,t.skin,t.auto_mask);
			text += b;
		}
		const auto &m = s.nr.mask;
		snprintf(b, sizeof(b), "\nmask environment=%.9g,%.9g near=%d,%.9g,%.9g,%.9g far=%d,%.9g,%.9g,%.9g feather=%.9g\n",
			m.environment.structure,m.environment.tone,m.near_band.on,m.near_band.structure,m.near_band.tone,m.near_end,m.far_band.on,m.far_band.structure,m.far_band.tone,m.far_start,m.feather);
		text += b;
		if (text != g.configuration) { g.configuration = text; fprintf(g.notes, "\nconfiguration %llu\n%s", static_cast<unsigned long long>(++g.config_id), text.c_str()); }
	}
	float average(float sum, float count) { return count > 0 ? sum / count : -1.0f; }
	float rms(float sum, float count) { return count > 0 ? std::sqrt((std::max)(sum / count, 0.0f)) : -1.0f; }
	void write_regions(const readback &slot, const measurement &m)
	{
		for (unsigned int region = 0; region < regions; ++region)
		{
			const auto &v = m.values[region];
			fprintf(g.regions_file, "%llu,%llu,%llu,%u", static_cast<unsigned long long>(slot.info.id), static_cast<unsigned long long>(slot.info.present), static_cast<unsigned long long>(slot.event), region);
			const float n = v[0][0], ni = v[7][0], no = v[7][1], nf = v[7][2], nd = v[6][3];
			const float values[] = { v[0][3], n, v[0][1], v[0][2], average(v[1][0],n), average(v[1][1],n), rms(v[1][2],n), rms(v[1][3],n),
				average(v[2][0],n), average(v[2][1],n), rms(v[2][2],n), rms(v[2][3],n), n?v[3][0]:-1,n?v[3][1]:-1,n?v[3][2]:-1,n?v[3][3]:-1,v[4][0],v[4][1],v[4][2],v[4][3],
				average(v[5][0],ni),average(v[5][1],no),average(v[24][0],nf),rms(v[24][2],ni),rms(v[24][3],no),rms(v[24][1],nf),
				average(v[6][0],nd),nd?v[6][1]:-1,nd?v[6][2]:-1,nd,v[27][3],ni?v[26][0]:-1,ni?v[26][1]:-1,no?v[26][2]:-1,no?v[26][3]:-1,nf?v[27][0]:-1,nf?v[27][1]:-1,v[27][2],
				average(v[25][0],v[25][3]),average(v[25][1],v[25][3]),average(v[25][2],v[25][3]),average(v[5][2],v[5][3]),v[5][3] };
			for (float value : values) fprintf(g.regions_file, ",%.9g", value);
			for (unsigned int c = 8; c < 24; ++c)
				fprintf(g.regions_file, ",%.9g,%.9g,%.0f,%.0f", average(v[c][0],v[c][2]), average(v[c][1],v[c][2]), v[c][2], v[c][3]);
			fputc('\n', g.regions_file);
		}
	}
	void drain(bool wait)
	{
		if (!g.device) return;
		ID3D11DeviceContext *ctx = nullptr;
		g.device->GetImmediateContext(&ctx);
		if (!ctx) return;
		if (wait) ctx->Flush();
		const ULONGLONG deadline = GetTickCount64() + 1500;
		const auto map_read = [&](ID3D11Texture2D *image, D3D11_MAPPED_SUBRESOURCE &mapped) {
			HRESULT result = ctx->Map(image, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
			while (wait && result == DXGI_ERROR_WAS_STILL_DRAWING && GetTickCount64() < deadline)
			{ Sleep(1); result = ctx->Map(image, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped); }
			return result;
		};
		for (unsigned int index = 0; index < ring_size; ++index)
		{
			readback &slot = g.slots[(g.next_slot + index) % ring_size];
			if (!slot.pending) continue;
			HRESULT ready = ctx->GetData(slot.done, nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
			while (wait && ready == S_FALSE && GetTickCount64() < deadline)
			{ Sleep(1); ready = ctx->GetData(slot.done, nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH); }
			if (ready == S_FALSE && !wait) continue;
			D3D11_MAPPED_SUBRESOURCE map = {}, sample_map = {};
			const HRESULT mapped = ready == S_OK ? map_read(slot.stats, map) : ready;
			if (mapped == DXGI_ERROR_WAS_STILL_DRAWING && !wait) continue;
			if (ready == S_OK && SUCCEEDED(mapped))
			{
				const HRESULT sample_result = slot.sampled ? map_read(slot.samples, sample_map) : S_FALSE;
				if (sample_result == DXGI_ERROR_WAS_STILL_DRAWING && !wait) { ctx->Unmap(slot.stats, 0); continue; }
				measurement m;
				m.frame = slot.info.id; m.input_history = slot.info.input_history; m.output_history = slot.info.output_history;
				for (unsigned int y = 0; y < regions; ++y) std::memcpy(m.values[y], static_cast<const unsigned char *>(map.pData) + y * map.RowPitch, sizeof(m.values[y]));
				ctx->Unmap(slot.stats, 0);
				if (slot.phase == 0)
				{
					write_regions(slot, m);
					++g.st.measured;
					AcquireSRWLockExclusive(&status_lock);
					if (m.frame >= published_measurement.frame) published_measurement = m;
					ReleaseSRWLockExclusive(&status_lock);
				}
				else
				{
					++g.st.surfaces;
					for (unsigned int region = 0; region < regions; ++region)
					{
						const auto &v = m.values[region];
						const float n = v[7][2]; const auto &r = slot.surface;
						fprintf(g.surfaces,"%llu,%llu,%llu,%s,%.6f,%u,%llu,%u,0x%llX,%u,%u,%u,0x%X,0x%08lX,%.0f,%.0f,%.9g,%.9g,%.9g,%.9g,%d",
							static_cast<unsigned long long>(slot.event),static_cast<unsigned long long>(slot.info.id),static_cast<unsigned long long>(slot.info.present),slot.phase == 1 ? "present" : "after_effects",slot.info.time_ms,region,
							static_cast<unsigned long long>(slot.info.config),slot.color_space,static_cast<unsigned long long>(r.identity),r.w,r.h,static_cast<unsigned int>(r.desc.Format),r.desc.BindFlags,static_cast<unsigned long>(r.result),
							n,v[4][3],average(v[24][0],n),rms(v[24][1],n),n?v[27][0]:-1,n?v[27][1]:-1,slot.sampled);
						const auto &nr = slot.nr;
						fprintf(g.surfaces, ",%llu,%d,%d,%d,%u,%u,0x%X,%u,%u,%u,%u,%.6f,%.6f\n", static_cast<unsigned long long>(nr.frames),
							nr.active,static_cast<int>(nr.position),nr.temporal,nr.passes,nr.reset_bits,nr.last_result,nr.work_w,nr.work_h,nr.guide_w,nr.guide_h,nr.gpu_ms,nr.sync_ms);
					}
				}
				if (slot.sampled && SUCCEEDED(sample_result))
				{
					const uint64_t ids[] = { slot.event, slot.info.id, slot.info.present };
					const uint32_t layout[] = { sample_w * sample_h * sample_planes * 4 * sizeof(float), (slot.info.input_history ? 1u : 0u) | (slot.info.output_history ? 2u : 0u) | (slot.phase << 8) };
					fwrite(ids, sizeof(ids), 1, g.samples_file); fwrite(layout, sizeof(layout), 1, g.samples_file);
					for (unsigned int y = 0; y < sample_h * sample_planes; ++y) fwrite(static_cast<const unsigned char *>(sample_map.pData) + y * sample_map.RowPitch, sample_w * 4 * sizeof(float), 1, g.samples_file);
					ctx->Unmap(slot.samples, 0); ++g.st.sample_frames;
				}
				else if (slot.sampled)
				{
					++g.st.dropped;
					fprintf(g.notes, "samples_drop probe=%llu frame=%llu result=0x%08lX\n", static_cast<unsigned long long>(slot.event), static_cast<unsigned long long>(slot.info.id), static_cast<unsigned long>(sample_result));
				}
			}
			else { ++g.st.dropped; fprintf(g.notes, "readback_drop probe=%llu frame=%llu result=0x%08lX\n", static_cast<unsigned long long>(slot.event), static_cast<unsigned long long>(slot.info.id), static_cast<unsigned long>(mapped)); }
			slot.pending = false;
		}
		drop(ctx);
	}
	void close_capture()
	{
		if (g.st.active) drain(true);
		for (auto &slot : g.slots) if (slot.pending) { ++g.st.dropped; slot.pending = false; }
		if (g.notes) fprintf(g.notes, "\nfinished frames=%llu measured=%llu surfaces=%llu dropped=%llu sample_frames=%llu gaps=%llu\n",
			static_cast<unsigned long long>(g.st.frames),static_cast<unsigned long long>(g.st.measured),static_cast<unsigned long long>(g.st.surfaces),static_cast<unsigned long long>(g.st.dropped),static_cast<unsigned long long>(g.st.sample_frames),static_cast<unsigned long long>(g.st.gaps));
		for (FILE **file : { &g.frames, &g.regions_file, &g.samples_file, &g.notes, &g.surfaces }) if (*file)
		{
			const bool failed = ferror(*file) != 0;
			const int result = fclose(*file); *file = nullptr;
			if (failed || result != 0) issue("capture file write failed; check disk space");
		}
		if (g.st.active) FORGE_LOG("motion capture finished: %s, %llu frames, %llu measured, %llu drops", g.st.folder,
			static_cast<unsigned long long>(g.st.frames),static_cast<unsigned long long>(g.st.measured),static_cast<unsigned long long>(g.st.dropped));
		g.st.active = false; g.in_flight = false; g.output_source = nullptr;
		release_gpu(); publish();
	}
	void configure(const forge::settings &s)
	{
		if (s.motion_capture == g.requested) return;
		g.requested = s.motion_capture;
		if (g.requested)
		{
			if (!open_capture()) { close_capture(); issue("the capture directory or files could not be opened"); }
			AcquireSRWLockExclusive(&status_lock); published_measurement = {}; ReleaseSRWLockExclusive(&status_lock);
		}
		else close_capture();
		publish();
	}
	unsigned int uint_parameter(const Parameter *p, const char *key, unsigned int fallback = 0)
	{
		unsigned int v = fallback; if (succeeded(get_ui(p, key, &v))) return v;
		int i = 0; return succeeded(get_i(p, key, &i)) && i >= 0 ? static_cast<unsigned int>(i) : fallback;
	}
	readback *queue_probe(ID3D11DeviceContext *ctx, const shader_params &cb, ID3D11ShaderResourceView *const *views, const frame_info &info, unsigned int phase)
	{
		if (!ctx || !g.device || !g.shader) return nullptr;
		unsigned int index = g.next_slot;
		for (unsigned int n = 0; n < ring_size && g.slots[index].pending; ++n) index = (index + 1) % ring_size;
		readback &slot = g.slots[index];
		if (slot.pending) return nullptr;
		ID3D11DeviceContext1 *c1 = nullptr;
		if (FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void **>(&c1)))) return nullptr;
		ID3DDeviceContextState *saved = nullptr;
		c1->SwapDeviceContextState(g.context_state, &saved);
		c1->ClearState();
		ctx->UpdateSubresource(g.cb, 0, nullptr, &cb, 0, 0);
		const bool sampled = cb.images[3] != 0;
		ID3D11UnorderedAccessView *outputs[] = { g.stats_uav, sampled ? g.samples_uav : nullptr };
		ctx->CSSetShader(g.shader, nullptr, 0); ctx->CSSetConstantBuffers(0, 1, &g.cb); ctx->CSSetSamplers(0, 1, &g.linear);
		ctx->CSSetShaderResources(0, 8, views); ctx->CSSetUnorderedAccessViews(0, 2, outputs, nullptr);
		ctx->Dispatch(regions, 1, 1);
		c1->ClearState();
		ctx->CopyResource(slot.stats, g.stats);
		if (sampled) ctx->CopyResource(slot.samples, g.sample_image);
		ctx->End(slot.done);
		c1->SwapDeviceContextState(saved, nullptr); drop(saved); drop(c1);
		slot.pending = true; slot.sampled = sampled; slot.info = info; slot.phase = phase; slot.event = ++g.event_id;
		g.next_slot = (index + 1) % ring_size;
		return &slot;
	}
	void surface_probe(ID3D11DeviceContext *ctx, ID3D11Resource *image, unsigned int phase)
	{
		if (!g.st.active || !image) return;
		resource_info r;
		r.requested_w = r.requested_h = UINT_MAX;
		if (!ensure_gpu(ctx) || !g.surface.capture(g.device, ctx, image, r))
		{
			++g.st.dropped;
			fprintf(g.notes, "surface_copy_drop phase=%u present=%llu result=0x%08lX\n", phase,
				static_cast<unsigned long long>(g.present_id), static_cast<unsigned long>(r.result));
			return;
		}
		frame_info info;
		info.id = g.previous.id; info.present = g.present_id; info.config = g.config_id;
		LARGE_INTEGER now; QueryPerformanceCounter(&now);
		info.time_ms = 1000.0 * static_cast<double>(now.QuadPart - g.start.QuadPart) / g.frequency.QuadPart;
		shader_params cb = {};
		cb.input_size[0] = cb.output_size[0] = r.w; cb.input_size[1] = cb.output_size[1] = r.h;
		cb.images[2] = 1; cb.images[3] = g.present_id % sample_interval == 0 || g.previous.continuity != 0;
		ID3D11ShaderResourceView *views[] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, g.surface.view, nullptr };
		if (auto *slot = queue_probe(ctx, cb, views, info, phase)) { slot->surface = r; slot->color_space = g.color_space; slot->nr = forge::nr::get_status(); }
		else { ++g.st.dropped; fprintf(g.notes, "surface_probe_drop phase=%u present=%llu\n", phase, static_cast<unsigned long long>(g.present_id)); }
	}
	void frame_csv(const forge::nr::status &nr, uint64_t probe, bool sampled)
	{
		const frame_info &f = g.frame; const auto &v = f.feature;
		fprintf(g.frames, "%llu,%llu,%.6f,%u,%s,%llu,0x%llX,0x%llX,%u,%u,%u,%u,%u,%u,%d,%d,0x%X,0x%X,%u,%u,%u,%u,%.9g,%.9g,%.9g,%.9g,0x%X,%.9g,%.9g,%.9g,0x%llX,%d,%d,%d,0x%X,%u,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%u,%u,0x%X,%u,%u,%.6f",
			static_cast<unsigned long long>(f.id),static_cast<unsigned long long>(f.present),f.time_ms,f.thread,tests[f.test],static_cast<unsigned long long>(f.config),
			static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(v.game_handle)),static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(v.live_handle)),
			v.in_w,v.in_h,v.out_w,v.out_h,v.effective_w,v.effective_h,v.game_perf,v.effective_perf,v.game_flags,v.effective_flags,f.param_w,f.param_h,f.render_w,f.render_h,
			f.jitter[0],f.jitter[1],f.mv_scale[0],f.mv_scale[1],f.reads,f.pre_exposure,f.exposure_scale,f.frame_ms,static_cast<unsigned long long>(f.exposure_texture),
			f.game_reset,v.forced_reset,f.effective_reset,f.result,f.continuity,f.numeric,f.input_ok,f.motion_ok,f.depth_ok,f.output_ok,f.final_ok,f.input_history,f.output_history,probe != 0,sampled,
			nr.active,nr.passes,nr.reset_bits,nr.last_result,nr.work_w,nr.work_h,nr.gpu_ms);
		fprintf(g.frames, ",0x%llX,%d,%llu,%llu", static_cast<unsigned long long>(f.context), f.exposure_ok, static_cast<unsigned long long>(nr.frames), static_cast<unsigned long long>(probe));
		fprintf(g.frames, ",%.6f,%.6f,0x%08lX,%d", f.through_sr_ms, f.hook_ms, static_cast<unsigned long>(f.final_result), f.feature.output_subrects);
		for (const auto &r : f.resources)
			fprintf(g.frames, ",0x%llX,%u,%u,%u,0x%X,%u,%u,%u,%u,%u,%u,%u,%u,%u,0x%08lX", static_cast<unsigned long long>(r.identity),
				r.desc.Width,r.desc.Height,static_cast<unsigned int>(r.desc.Format),r.desc.BindFlags,r.desc.MipLevels,r.desc.ArraySize,r.desc.SampleDesc.Count,
				r.x,r.y,r.requested_w,r.requested_h,r.w,r.h,static_cast<unsigned long>(r.result));
		fprintf(g.frames, ",%llu,%llu,%d,%d,%u,%u,%.6f", static_cast<unsigned long long>(v.generation), static_cast<unsigned long long>(v.recreates),
			static_cast<int>(nr.position),nr.temporal,nr.guide_w,nr.guide_h,nr.sync_ms);
		for (unsigned int i = 0; i < 6; ++i) fprintf(g.frames, ",%u,%u", v.game_presets[i], v.effective_presets[i]);
		fputc('\n', g.frames);
	}
}

void forge::motion_diagnostics::begin(ID3D11DeviceContext *ctx, const Parameter *p, const feature_info &feature, const settings &s)
{
	configure(s);
	if (!g.st.active || !p) return;
	drain(false);
	configuration(s);
	g.in_flight = true; g.saw_evaluate = true;
	g.frame = {};
	auto &f = g.frame;
	f.id = ++g.st.frames; f.present = g.present_id; f.config = g.config_id; f.feature = feature; f.thread = GetCurrentThreadId();
	f.test = s.motion_capture_test >= 0 && s.motion_capture_test < 8 ? static_cast<unsigned int>(s.motion_capture_test) : 7;
	LARGE_INTEGER now; QueryPerformanceCounter(&now);
	f.time_ms = 1000.0 * static_cast<double>(now.QuadPart - g.start.QuadPart) / g.frequency.QuadPart;
	f.param_w = uint_parameter(p, name::render_subrect_w); f.param_h = uint_parameter(p, name::render_subrect_h);
	f.render_w = f.param_w && f.param_h ? f.param_w : feature.in_w; f.render_h = f.param_w && f.param_h ? f.param_h : feature.in_h;
	f.game_reset = feature.game_reset;
	f.context = reinterpret_cast<uintptr_t>(ctx);
	const auto read_float = [&](const char *key, float &value, unsigned int bit) { if (succeeded(get_f(p,key,&value))) f.reads |= 1u << bit; };
	read_float(name::jitter_x,f.jitter[0],0); read_float(name::jitter_y,f.jitter[1],1);
	read_float(name::mv_scale_x,f.mv_scale[0],2); read_float(name::mv_scale_y,f.mv_scale[1],3);
	read_float("DLSS.Pre.Exposure",f.pre_exposure,4); read_float("DLSS.Exposure.Scale",f.exposure_scale,5); read_float("FrameTimeDeltaInMsec",f.frame_ms,6);
	ID3D11Resource *input = nullptr, *motion = nullptr, *depth = nullptr, *exposure = nullptr;
	get_d3d11(p,name::color,&input); get_d3d11(p,name::motion_vectors,&motion); get_d3d11(p,name::depth,&depth); get_d3d11(p,"ExposureTexture",&exposure);
	get_d3d11(p,name::output,&g.output_source); f.exposure_texture = reinterpret_cast<uintptr_t>(exposure);
	f.numeric = f.render_w && f.render_h && std::isfinite(f.jitter[0]) && std::isfinite(f.jitter[1]) && std::isfinite(f.mv_scale[0]) && std::isfinite(f.mv_scale[1]);
	f.resources[0].x = uint_parameter(p,name::color_subrect_x); f.resources[0].y = uint_parameter(p,name::color_subrect_y);
	f.resources[0].requested_w = f.render_w; f.resources[0].requested_h = f.render_h;
	f.resources[1].x = uint_parameter(p,name::mv_subrect_x); f.resources[1].y = uint_parameter(p,name::mv_subrect_y);
	const bool low = (feature.effective_flags & flag_mv_low_res) != 0;
	// Guides cover the render subrect, also beyond the feature's render size (Game decides); copies clamp to the texture
	f.resources[1].requested_w = low ? f.render_w : feature.effective_w;
	f.resources[1].requested_h = low ? f.render_h : feature.effective_h;
	f.resources[2].x = uint_parameter(p,name::depth_subrect_x); f.resources[2].y = uint_parameter(p,name::depth_subrect_y);
	f.resources[2].requested_w = f.render_w; f.resources[2].requested_h = f.render_h;
	f.resources[3].x = feature.output_subrects ? uint_parameter(p,name::output_subrect_x) : 0;
	f.resources[3].y = feature.output_subrects ? uint_parameter(p,name::output_subrect_y) : 0;
	f.resources[3].requested_w = feature.effective_w; f.resources[3].requested_h = feature.effective_h;
	f.resources[4].requested_w = f.resources[4].requested_h = 1;
	f.resources[0].describe(input); f.resources[1].describe(motion); f.resources[2].describe(depth);
	f.resources[3].describe(g.output_source); f.resources[4].describe(exposure);
	f.gpu_ready = ensure_gpu(ctx);
	if (f.gpu_ready)
	{
		f.input_ok = g.input[g.current].capture(g.device,ctx,input,f.resources[0]);
		f.motion_ok = g.motion.capture(g.device,ctx,motion,f.resources[1]);
		f.depth_ok = g.depth.capture(g.device,ctx,depth,f.resources[2]);
		f.exposure_ok = g.exposure.capture(g.device,ctx,exposure,f.resources[4]);
	}
}

void forge::motion_diagnostics::after_sr(ID3D11DeviceContext *ctx, Result result, bool effective_reset)
{
	if (!g.st.active || !g.in_flight) return;
	g.frame.result = result; g.frame.effective_reset = effective_reset;
	LARGE_INTEGER now; QueryPerformanceCounter(&now);
	g.frame.through_sr_ms = 1000.0 * static_cast<double>(now.QuadPart - g.start.QuadPart) / g.frequency.QuadPart - g.frame.time_ms;
	if (g.frame.gpu_ready && succeeded(result)) g.frame.output_ok = g.output[g.current].capture(g.device,ctx,g.output_source,g.frame.resources[3]);
}

void forge::motion_diagnostics::finish(ID3D11DeviceContext *ctx, const settings &, const nr::status &nr)
{
	if (!g.st.active || !g.in_flight) return;
	auto &f = g.frame;
	resource_info final_info = f.resources[3]; final_info.result = E_PENDING;
	if (f.gpu_ready && succeeded(f.result)) f.final_ok = g.final_image.capture(g.device,ctx,g.output_source,final_info);
	f.final_result = final_info.result;
	LARGE_INTEGER now; QueryPerformanceCounter(&now);
	f.hook_ms = 1000.0 * static_cast<double>(now.QuadPart - g.start.QuadPart) / g.frequency.QuadPart - f.time_ms;
	f.continuity = !g.have_previous ? 1u : 0u;
	if (g.have_previous)
	{
		const auto &old = g.previous;
		if (f.feature.live_handle != old.feature.live_handle || f.context != old.context) f.continuity |= 2u;
		if (f.present != old.present && f.present != old.present + 1) f.continuity |= 4u;
		if (f.render_w != old.render_w || f.render_h != old.render_h) f.continuity |= 8u;
		for (unsigned int i = 0; i < 4; ++i) if (!(f.resources[i] == old.resources[i])) f.continuity |= 8u;
		if (f.mv_scale[0] != old.mv_scale[0] || f.mv_scale[1] != old.mv_scale[1] || f.feature.effective_flags != old.feature.effective_flags) f.continuity |= 16u;
	}
	if (f.effective_reset) f.continuity |= 32u;
	if (!f.numeric) f.continuity |= 64u;
	if (!succeeded(f.result)) f.continuity |= 128u;
	f.input_history = f.continuity == 0 && f.input_ok && g.previous.input_ok;
	f.output_history = f.continuity == 0 && f.output_ok && g.previous.output_ok;
	uint64_t probe = 0;
	bool sampled = false;
	if (f.gpu_ready)
	{
		shader_params cb = {};
		const unsigned int prev = g.current ^ 1;
		cb.input_size[0] = f.resources[0].w; cb.input_size[1] = f.resources[0].h;
		cb.input_size[2] = g.input[prev].w; cb.input_size[3] = g.input[prev].h;
		cb.output_size[0] = f.resources[3].w; cb.output_size[1] = f.resources[3].h;
		cb.output_size[2] = g.output[prev].w; cb.output_size[3] = g.output[prev].h;
		cb.guide_size[0] = f.resources[1].w; cb.guide_size[1] = f.resources[1].h;
		cb.guide_size[2] = f.resources[2].w; cb.guide_size[3] = f.resources[2].h;
		cb.depth_origin[0] = g.depth.origin_x; cb.depth_origin[1] = g.depth.origin_y;
		if (f.numeric)
		{
			cb.scale[0] = f.mv_scale[0] / f.render_w; cb.scale[1] = f.mv_scale[1] / f.render_h;
			cb.scale[2] = f.mv_scale[0] / (std::max)(f.resources[1].desc.Width,1u); cb.scale[3] = f.mv_scale[1] / (std::max)(f.resources[1].desc.Height,1u);
			cb.jitter[0] = f.jitter[0] / f.render_w; cb.jitter[1] = f.jitter[1] / f.render_h;
			if (f.continuity == 0)
			{ cb.jitter[2] = g.previous.jitter[0] / g.previous.render_w - cb.jitter[0]; cb.jitter[3] = g.previous.jitter[1] / g.previous.render_h - cb.jitter[1]; }
		}
		cb.render_size[0] = static_cast<float>(f.render_w); cb.render_size[1] = static_cast<float>(f.render_h); cb.render_size[2] = f.exposure_ok ? 1.0f : 0.0f;
		cb.flags[0] = f.input_history; cb.flags[1] = f.output_history; cb.flags[2] = f.motion_ok && f.numeric; cb.flags[3] = f.depth_ok;
		cb.images[0] = f.input_ok; cb.images[1] = f.output_ok; cb.images[2] = f.final_ok;
		sampled = (f.id - 1) % sample_interval == 0 || f.continuity != 0; cb.images[3] = sampled;
		ID3D11ShaderResourceView *views[] = { f.input_ok?g.input[g.current].view:nullptr, f.input_history?g.input[prev].view:nullptr,
			f.output_ok?g.output[g.current].view:nullptr,f.output_history?g.output[prev].view:nullptr,f.motion_ok?g.motion.view:nullptr,f.depth_ok?g.depth.view:nullptr,f.final_ok?g.final_image.view:nullptr,f.exposure_ok?g.exposure.view:nullptr };
		if (auto *slot = queue_probe(ctx, cb, views, f, 0)) probe = slot->event;
	}
	if (!probe) { ++g.st.dropped; fprintf(g.notes,"probe_drop frame=%llu\n",static_cast<unsigned long long>(f.id)); }
	frame_csv(nr,probe,probe != 0 && sampled);
	g.previous = f; g.have_previous = f.input_ok && f.output_ok && succeeded(f.result);
	g.current ^= 1; g.in_flight = false; g.output_source = nullptr;
	if (f.id % 64 == 0) for (FILE *file : { g.frames,g.regions_file,g.samples_file,g.notes,g.surfaces }) fflush(file);
	for (FILE *file : { g.frames,g.regions_file,g.samples_file,g.notes,g.surfaces }) if (ferror(file)) issue("capture file write failed; stop the capture and check disk space");
	publish();
}

void forge::motion_diagnostics::present(ID3D11DeviceContext *ctx, const settings &s, ID3D11Resource *image, unsigned int color_space)
{
	configure(s);
	if (g.st.active)
	{
		configuration(s);
		drain(false);
		g.color_space = color_space;
		surface_probe(ctx, image, 1);
		if (!g.saw_evaluate)
		{
			++g.st.gaps;
			fprintf(g.notes,"present_without_evaluate present=%llu\n",static_cast<unsigned long long>(g.present_id));
			g.have_previous = false;
		}
		g.saw_evaluate = false; publish();
	}
	++g.present_id;
}

void forge::motion_diagnostics::after_effects(ID3D11DeviceContext *ctx, ID3D11Resource *image)
{
	if (!g.st.active) return;
	drain(false);
	surface_probe(ctx, image, 2);
	publish();
}

bool forge::motion_diagnostics::active()
{
	return g.requested || g.st.active;
}

void forge::motion_diagnostics::detach()
{
	close_capture(); g.requested = false;
}

forge::motion_diagnostics::status forge::motion_diagnostics::get_status()
{
	AcquireSRWLockShared(&status_lock); status result = published_status; ReleaseSRWLockShared(&status_lock); return result;
}

forge::motion_diagnostics::measurement forge::motion_diagnostics::last_measurement()
{
	AcquireSRWLockShared(&status_lock); measurement result = published_measurement; ReleaseSRWLockShared(&status_lock); return result;
}
