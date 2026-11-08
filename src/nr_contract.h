#pragma once

#include "nr_config.h"
#include <array>
#include <cstdint>

namespace forge::nr
{
	// SR scales motion to render pixels. Convert those pixels to UV before NR guide packing.
	inline float motion_to_uv(float scale, unsigned int extent)
	{
		return extent != 0 && std::isfinite(scale) ? scale / static_cast<float>(extent) : 0.0f;
	}

	// Raster positions include jitter. Resolved positions use the stable image grid.
	inline float motion_jitter_offset(stage position, bool motion_jittered, float current, float previous)
	{
		const int target = position == stage::before_upscaling ? 1 : 0;
		return static_cast<float>(target - (motion_jittered ? 1 : 0)) * (previous - current);
	}

	struct history_key
	{
		uintptr_t source = 0, context = 0;
		std::array<unsigned int, 4> image = {}, depth = {}, motion = {};
		std::array<unsigned int, 2> render = {};
		std::array<float, 2> motion_scale = {};
		std::array<float, 12> mask_values = {};
		unsigned int format = 0, mask_blend = 0;
		uint64_t mask_revision = 0;
		stage position = stage::after_upscaling;
		float white = 1.0f;
		bool hdr = false, temporal = false, motion_jittered = false, mask = false, estimate = false;

		bool operator==(const history_key &) const = default;
	};

	inline unsigned int history_reset(bool game_reset, bool valid, uint64_t present, uint64_t previous_present,
		const history_key &current, const history_key &previous)
	{
		return (game_reset ? 1u : 0u) | (!valid ? 2u : 0u) |
			(valid && present != previous_present && present != previous_present + 1 ? 4u : 0u) |
			(valid && current != previous ? 8u : 0u);
	}

	// An upstream output change resets its consumers. The DLL handles each feature's own controls.
	inline bool output_changed(const tuning &a, const tuning &b)
	{
		const auto changed = [](float x, float y) { return std::fabs(x - y) > 1e-5f; };
		return a.preset != b.preset || a.style != b.style || a.auto_mask != b.auto_mask ||
			changed(a.intensity, b.intensity) || changed(a.structure, b.structure) ||
			changed(a.tone, b.tone) || changed(a.skin, b.skin);
	}
}
