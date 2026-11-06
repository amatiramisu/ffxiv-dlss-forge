#pragma once

#include <cmath>

// DLSS 5 Neural Rendering settings. The model parameters follow NVIDIA's names (DLSSNR.*).
namespace forge::nr
{
	constexpr unsigned int max_passes = 30;
	constexpr unsigned int default_pass_limit = 3;

	// One model pass. Pass 1 holds explicit values; later passes inherit pass 1 wherever a value is
	// "inherit" (negative preset/style/mask, NaN floats), except local tone, which later passes default to 0.
	struct profile
	{
		int preset = -1;       // DLSSNR.Hint.Render.Preset: the weight set, 0 the DLL's default; the DLL's own list is in nr_dll.h
		int style = -1;        // DLSSNR.Style: 0 Model A, 1 Model B, 2 Model C
		// 310.9.16: intensity saturates at 1; structure, tone and character structure respond up to 2.
		float intensity = NAN; // DLSSNR.Intensity, 0 .. 1
		float structure = NAN; // DLSSNR.LocalStructureStrength ("structure intensity"), 0 .. 2
		float tone = NAN;      // DLSSNR.LocalToneStrength ("tone intensity"), 0 .. 2
		float skin = NAN;      // DLSSNR.SkinStructureStrength ("base character", with the automask), 0 .. 2; -1 is exactly structure
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
		int device = 0;           // 0 Vulkan on DXVK's device (D3D12 without DXVK), 1 D3D12; from FORGE_NR_BACKEND
		unsigned int passes = 1;
		bool unlock_passes = false;
		profile pass[max_passes];

		float work_scale = 1.0f;  // model resolution relative to the frame, 0.25 .. 2.0

		float detail = 1.0f;      // how much of the model's edit reaches the frame, 0 .. 2
		float colour = 1.0f;      // 0 keeps the game's colour, 1 the model's, above 1 more saturated, 0 .. 4
		float guard = 2.0f;       // highlight guard: largest brightening/darkening factor, 1 .. 8
		float paper_white = 1.0f; // HDR inputs only: scene value shown to the model as white
		// Display-referred frames (no IsHDR): 1 runs them through the sRGB decode (metered divisor,
		// sRGB-encoded model input, ratio transfer in stops); 0 hands the frame to the model as stored
		bool codec = true;
		float codec_luma = 2.0f;   // sRGB decode: largest brightness change, in stops, 0 .. 4
		float codec_chroma = 1.0f; // sRGB decode: largest colour change around it, in stops, 0 .. 2
		// sRGB decode with several passes: true bounds and re-encodes each pass (one submission per pass),
		// false chains the raw model outputs and bounds the result once (one submission per frame)
		bool per_pass = true;

		bool regions = false;     // own strengths for skin-coloured pixels (a colour test, see SkinColourWeight) and the rest
		bool skin_tone = true;    // allow colour changes on skin
		float skin_detail = 1.0f, skin_colour = 1.0f, env_detail = 1.0f, env_colour = 1.0f;
		bool show_mask = false;

		bool show_automask = false; // estimate of the model's own character mask: a second model pass with the base character at 0 or 1
		masking mask;

		bool apply = true;        // false keeps the model running and shows the clean frame
		int compare = 0;          // 0 off, 1 side by side, 2 wipe
		float split = 0.5f;
		bool swap = false;
		int debug_view = 0;       // 0 off, 1 model input, 2 model output, 3 difference x20

		config()
		{
			pass[0] = { 0, 0, 1.0f, 1.0f, 1.0f, -1.0f, 1 };
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
			t.structure = clamp(i == 0 ? b.structure : pick_f(p.structure, b.structure), 0.0f, 2.0f, 1.0f);
			t.tone = clamp(i == 0 ? b.tone : pick_f(p.tone, 0.0f), 0.0f, 2.0f, i == 0 ? 1.0f : 0.0f);
			t.skin = clamp(i == 0 ? b.skin : pick_f(p.skin, b.skin), -1.0f, 2.0f, -1.0f);
			if (t.skin < 0.0f)
				t.skin = -1.0f;
			t.auto_mask = (i == 0 || p.auto_mask < 0 ? b.auto_mask : p.auto_mask) != 0;
			if (t.style > 2)
				t.style = 0;
			return t;
		}
	};
}
