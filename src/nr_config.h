#pragma once

#include <cmath>

// DLSS 5 Neural Rendering settings. The model parameters follow NVIDIA's names (DLSSNR.*).
namespace forge::nr
{
	constexpr unsigned int max_passes = 30;
	constexpr unsigned int default_pass_limit = 3;

	enum class stage { before_upscaling, after_upscaling, present };

	// One model pass. Pass 1 holds explicit values; later passes inherit pass 1 wherever a value is
	// "inherit" (negative preset/style/mask, NaN floats), except local tone, which later passes default to 0.
	struct profile
	{
		int preset = -1;       // DLSSNR.Hint.Render.Preset: the weight set, 0 the DLL's default; the DLL's own list is in nr_dll.h
		int style = -1;        // DLSSNR.Style: 0 Model A, 1 Model B, 2 Model C
		// 0 .. 1 (NVIDIA panel range)
		float intensity = NAN; // DLSSNR.Intensity
		float structure = NAN; // DLSSNR.LocalStructureStrength ("structure intensity")
		float tone = NAN;      // DLSSNR.LocalToneStrength ("tone intensity")
		float skin = NAN;      // DLSSNR.SkinStructureStrength ("base character", with the automask), -1 follows structure
		int auto_mask = -1;    // DLSSNR.UseAutoMask
	};

	// A pass with every value resolved. Only the preset is fixed when the model is built; the snippet
	// reads every other value at each evaluate.
	struct tuning
	{
		unsigned int preset = 0;
		unsigned int style = 0;
		float intensity = 1.0f;
		float structure = 1.0f;
		float tone = 1.0f;
		float skin = -1.0f;
		bool auto_mask = true;

		bool operator==(const tuning &o) const
		{
			return preset == o.preset && style == o.style && intensity == o.intensity && structure == o.structure &&
				tone == o.tone && skin == o.skin && auto_mask == o.auto_mask;
		}
		bool operator!=(const tuning &o) const { return !(*this == o); }
	};

	// One group of the developer mask: the structure and tone values written into the control mask for the
	// pixels the group selects. NVIDIA's panel offers both as 0 .. 1 sliders per group.
	struct mask_group
	{
		bool on = true;
		float structure = 1.0f;
		float tone = 1.0f;
	};

	// Developer masking (DLSSNR.ControlMask + DLSSNR.MaskBlendMode), painted from the game's depth: a near band,
	// a far band, and the environment for everything else. A group's values multiply the global structure and
	// tone intensities for its pixels. In third person and gpose the player is inside the near band.
	struct masking
	{
		bool enabled = false;
		int blend = 0;            // DLSSNR.MaskBlendMode: 0 the mask replaces the automask, 1 .. 3 combine with it
		bool show = false;        // overlay of the groups on the frame
		mask_group near_band;     // closer than `near_end`
		mask_group far_band;      // farther than `far_start`
		mask_group environment;   // everything else
		float near_end = 20.0f;   // distances in multiples of the camera's near plane, 1 .. 10000 (log)
		float far_start = 400.0f;
		float feather = 0.5f;     // width of a band's edge in octaves of distance, 0 .. 2

		masking()
		{
			near_band.on = false;
			far_band.on = false;
			environment.on = true;
		}
	};

	struct config
	{
		bool enabled = false;
		stage position = stage::after_upscaling;
		bool temporal = true;
		int device = 0;           // 0 Vulkan on DXVK's device (D3D12 without DXVK), 1 D3D12; from FORGE_NR_BACKEND
		unsigned int passes = 1;
		bool unlock_passes = false;
		profile pass[max_passes];

		float work_scale = 1.0f;  // model resolution relative to the frame, 0.25 .. 2.0

		float paper_white = 1.0f; // HDR inputs only: scene value shown to the model as white

		bool show_automask = false; // estimate of the model's own character mask: a second model pass with the base character at 0 or 1
		masking mask;

		bool apply = true;        // false keeps the model running and shows the clean frame
		int compare = 0;          // 0 off, 1 side by side, 2 wipe
		float split = 0.5f;
		bool swap = false;
		int debug_view = 0;       // 0 off, 1 model input, 2 model output, 3 difference x20
		bool frame_log = false;   // per-frame values in the add-on's log, for this session

		static profile default_profile(unsigned int i)
		{
			return i == 0 ? profile { 0, 0, 1.0f, 1.0f, 1.0f, -1.0f, 1 } : profile {};
		}

		config()
		{
			pass[0] = default_profile(0);
		}

		void reset_pass(unsigned int i)
		{
			if (i < max_passes) pass[i] = default_profile(i);
		}

		void reset_defaults()
		{
			// The backend comes from the process environment.
			const int backend = device;
			*this = config {};
			device = backend;
		}

		stage selected_stage() const
		{
			return position == stage::before_upscaling || position == stage::present ? position : stage::after_upscaling;
		}

		unsigned int pass_limit() const { return unlock_passes ? max_passes : default_pass_limit; }
		unsigned int pass_count() const
		{
			const unsigned int lim = pass_limit();
			return passes < 1 ? 1 : (passes > lim ? lim : passes);
		}

		tuning resolve(unsigned int i) const
		{
			const profile &b = pass[0];
			const profile &p = pass[i < max_passes ? i : 0];
			const auto pick_f = [&](float v, float base) { return std::isnan(v) ? base : v; };
			const auto clamp = [](float v, float lo, float hi, float fallback) { return std::isfinite(v) ? (v < lo ? lo : (v > hi ? hi : v)) : fallback; };
			tuning t;
			t.preset = static_cast<unsigned int>(i == 0 || p.preset < 0 ? (b.preset < 0 ? 0 : b.preset) : p.preset);
			t.style = static_cast<unsigned int>(i == 0 || p.style < 0 ? (b.style < 0 ? 0 : b.style) : p.style);
			t.intensity = clamp(i == 0 ? b.intensity : pick_f(p.intensity, b.intensity), 0.0f, 1.0f, 1.0f);
			t.structure = clamp(i == 0 ? b.structure : pick_f(p.structure, b.structure), 0.0f, 1.0f, 1.0f);
			t.tone = clamp(i == 0 ? b.tone : pick_f(p.tone, 0.0f), 0.0f, 1.0f, i == 0 ? 1.0f : 0.0f);
			t.skin = clamp(i == 0 ? b.skin : pick_f(p.skin, b.skin), -1.0f, 1.0f, -1.0f);
			if (t.skin < 0.0f)
				t.skin = -1.0f;
			t.auto_mask = (i == 0 || p.auto_mask < 0 ? b.auto_mask : p.auto_mask) != 0;
			if (t.style > 2)
				t.style = 0;
			return t;
		}
	};
}
