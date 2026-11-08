#pragma once

#include <cstdint>

namespace forge::motion_diagnostics
{
	struct shader_params
	{
		uint32_t input_size[4];  // current width/height, previous width/height
		uint32_t output_size[4];
		uint32_t guide_size[4];  // motion width/height, depth width/height
		float scale[4];         // raw motion to UV: render units, texture units
		float jitter[4];        // current UV jitter, previous minus current UV jitter
		float render_size[4];   // current render width/height, exposure present, 0
		uint32_t flags[4];      // input history, output history, motion present, depth present
		uint32_t images[4];     // input present, output present, final present, write samples
		uint32_t depth_origin[4]; // depth-stencil copies keep the full source texture
	};
	static_assert(sizeof(shader_params) == 144);
}
