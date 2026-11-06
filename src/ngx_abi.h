// The NGX ABI this add-on uses. The NGX core (_nvngx.dll) is an MSVC build, and MSVC lays out
// NVSDK_NGX_Parameter's overloaded virtual functions grouped and in reverse declaration order. This GNU-ABI
// build calls them through an explicit MSVC slot table with `this` as the first argument.
#pragma once

#include <cstdint>
#include <d3d11.h>

namespace ngx
{
	using Result = uint32_t;
	constexpr Result result_success = 0x1;
	constexpr Result result_fail = 0xBAD00000;
	inline bool succeeded(Result r) { return (r & 0xFFF00000u) != result_fail; }

	struct Handle { unsigned int Id; };
	struct Parameter; // opaque object owned by _nvngx.dll

	constexpr int feature_super_sampling = 1;
	constexpr int sdk_version_api = 0x15;

	enum perf_quality : int
	{
		perf_max_perf = 0,
		perf_balanced = 1,
		perf_max_quality = 2,
		perf_ultra_performance = 3,
		perf_ultra_quality = 4,
		perf_dlaa = 5,
	};

	enum feature_flags : int
	{
		flag_is_hdr = 1 << 0,
		flag_mv_low_res = 1 << 1,
		flag_mv_jittered = 1 << 2,
		flag_depth_inverted = 1 << 3,
		flag_do_sharpening = 1 << 5,
		flag_auto_exposure = 1 << 6,
		flag_alpha_upscaling = 1 << 7,
	};

	// MSVC vtable slots of NVSDK_NGX_Parameter (overload groups reversed).
	namespace slot
	{
		constexpr int set_void_ptr = 0;
		constexpr int set_d3d12 = 1;
		constexpr int set_d3d11 = 2;
		constexpr int set_i = 3;
		constexpr int set_ui = 4;
		constexpr int set_d = 5;
		constexpr int set_f = 6;
		constexpr int set_ull = 7;
		constexpr int get_void_ptr = 8;
		constexpr int get_d3d12 = 9;
		constexpr int get_d3d11 = 10;
		constexpr int get_i = 11;
		constexpr int get_ui = 12;
		constexpr int get_d = 13;
		constexpr int get_f = 14;
		constexpr int get_ull = 15;
		constexpr int reset = 16;
	}

	template <typename Fn>
	inline Fn vfn(const Parameter *p, int index)
	{
		return reinterpret_cast<Fn>((*reinterpret_cast<void *const *const *>(p))[index]);
	}

	inline void set_ui(Parameter *p, const char *name, unsigned int v) { vfn<void (*)(Parameter *, const char *, unsigned int)>(p, slot::set_ui)(p, name, v); }
	inline void set_i(Parameter *p, const char *name, int v) { vfn<void (*)(Parameter *, const char *, int)>(p, slot::set_i)(p, name, v); }
	inline void set_f(Parameter *p, const char *name, float v) { vfn<void (*)(Parameter *, const char *, float)>(p, slot::set_f)(p, name, v); }
	inline void set_d3d11(Parameter *p, const char *name, ID3D11Resource *v) { vfn<void (*)(Parameter *, const char *, ID3D11Resource *)>(p, slot::set_d3d11)(p, name, v); }
	inline void set_ptr(Parameter *p, const char *name, void *v) { vfn<void (*)(Parameter *, const char *, void *)>(p, slot::set_void_ptr)(p, name, v); }

	inline Result get_ui(const Parameter *p, const char *name, unsigned int *v) { return vfn<Result (*)(const Parameter *, const char *, unsigned int *)>(p, slot::get_ui)(p, name, v); }
	inline Result get_i(const Parameter *p, const char *name, int *v) { return vfn<Result (*)(const Parameter *, const char *, int *)>(p, slot::get_i)(p, name, v); }
	inline Result get_f(const Parameter *p, const char *name, float *v) { return vfn<Result (*)(const Parameter *, const char *, float *)>(p, slot::get_f)(p, name, v); }
	inline Result get_d3d11(const Parameter *p, const char *name, ID3D11Resource **v) { return vfn<Result (*)(const Parameter *, const char *, ID3D11Resource **)>(p, slot::get_d3d11)(p, name, v); }
	inline Result get_ptr(const Parameter *p, const char *name, void **v) { return vfn<Result (*)(const Parameter *, const char *, void **)>(p, slot::get_void_ptr)(p, name, v); }

	// Exported C entry points of _nvngx.dll (signatures as the static NGX library calls them).
	using PFN_Init = Result (*)(unsigned long long app_id, const wchar_t *data_path, ID3D11Device *device, const void *feature_info, int sdk_version);
	using PFN_Init_ProjectID = Result (*)(const char *project_id, int engine_type, const char *engine_version, const wchar_t *data_path, ID3D11Device *device, int sdk_version, const void *feature_info);
	using PFN_Init_Ext = Result (*)(unsigned long long app_id, const wchar_t *data_path, ID3D11Device *device, int sdk_version, const void *feature_info);
	using PFN_Shutdown = Result (*)();
	using PFN_Shutdown1 = Result (*)(ID3D11Device *device);
	using PFN_GetParams = Result (*)(Parameter **out); // GetParameters / AllocateParameters / GetCapabilityParameters
	using PFN_DestroyParams = Result (*)(Parameter *p);
	using PFN_CreateFeature = Result (*)(ID3D11DeviceContext *ctx, int feature, Parameter *p, Handle **out);
	using PFN_ReleaseFeature = Result (*)(Handle *h);
	using PFN_EvaluateFeature = Result (*)(ID3D11DeviceContext *ctx, const Handle *h, const Parameter *p, void *callback);
	using PFN_OptimalSettingsCallback = Result (*)(Parameter *p);

	// Parameter names
	namespace name
	{
		constexpr const char *width = "Width";
		constexpr const char *height = "Height";
		constexpr const char *out_width = "OutWidth";
		constexpr const char *out_height = "OutHeight";
		constexpr const char *perf_quality = "PerfQualityValue";
		constexpr const char *rtx_value = "RTXValue";
		constexpr const char *creation_node_mask = "CreationNodeMask";
		constexpr const char *visibility_node_mask = "VisibilityNodeMask";
		constexpr const char *create_flags = "DLSS.Feature.Create.Flags";
		constexpr const char *enable_output_subrects = "DLSS.Enable.Output.Subrects";
		constexpr const char *optimal_settings_callback = "DLSSOptimalSettingsCallback";
		constexpr const char *dyn_max_w = "DLSS.Get.Dynamic.Max.Render.Width";
		constexpr const char *dyn_max_h = "DLSS.Get.Dynamic.Max.Render.Height";
		constexpr const char *dyn_min_w = "DLSS.Get.Dynamic.Min.Render.Width";
		constexpr const char *dyn_min_h = "DLSS.Get.Dynamic.Min.Render.Height";
		constexpr const char *sharpness = "Sharpness";
		constexpr const char *color = "Color";
		constexpr const char *output = "Output";
		constexpr const char *depth = "Depth";
		constexpr const char *motion_vectors = "MotionVectors";
		constexpr const char *jitter_x = "Jitter.Offset.X";
		constexpr const char *jitter_y = "Jitter.Offset.Y";
		constexpr const char *mv_scale_x = "MV.Scale.X";
		constexpr const char *mv_scale_y = "MV.Scale.Y";
		constexpr const char *reset = "Reset";
		constexpr const char *render_subrect_w = "DLSS.Render.Subrect.Dimensions.Width";
		constexpr const char *render_subrect_h = "DLSS.Render.Subrect.Dimensions.Height";
		constexpr const char *output_subrect_x = "DLSS.Output.Subrect.Base.X";
		constexpr const char *output_subrect_y = "DLSS.Output.Subrect.Base.Y";
		constexpr const char *depth_subrect_x = "DLSS.Input.Depth.Subrect.Base.X";
		constexpr const char *depth_subrect_y = "DLSS.Input.Depth.Subrect.Base.Y";
		constexpr const char *mv_subrect_x = "DLSS.Input.MV.Subrect.Base.X";
		constexpr const char *mv_subrect_y = "DLSS.Input.MV.Subrect.Base.Y";
		constexpr const char *ss_available = "SuperSampling.Available";

		// Indexed by perf_quality value (MaxPerf, Balanced, MaxQuality, UltraPerformance, UltraQuality, DLAA)
		constexpr const char *preset_hint[6] = {
			"DLSS.Hint.Render.Preset.Performance",
			"DLSS.Hint.Render.Preset.Balanced",
			"DLSS.Hint.Render.Preset.Quality",
			"DLSS.Hint.Render.Preset.UltraPerformance",
			"DLSS.Hint.Render.Preset.UltraQuality",
			"DLSS.Hint.Render.Preset.DLAA",
		};
	}

	inline const char *perf_quality_label(int v)
	{
		switch (v)
		{
		case perf_max_perf: return "Performance";
		case perf_balanced: return "Balanced";
		case perf_max_quality: return "Quality";
		case perf_ultra_performance: return "Ultra Performance";
		case perf_ultra_quality: return "Ultra Quality";
		case perf_dlaa: return "DLAA";
		default: return "unknown";
		}
	}

	inline char preset_letter(unsigned int v)
	{
		return v == 0 ? '-' : (v <= 26 ? static_cast<char>('A' + v - 1) : '?');
	}
}
