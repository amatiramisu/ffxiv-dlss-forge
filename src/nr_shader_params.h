#pragma once

#include <cstdint>

namespace forge::nr
{
	struct shader_params
	{
		uint32_t rect[4];
		uint32_t guides[4];
		uint32_t sizes[4];
		uint32_t motion_size[4];
		float encoding[4];
		float motion_map[4];
		uint32_t modes[4];
		float compare[4];
		float mask_env[4];
		float mask_near[4];
		float mask_far[4];
		float mask_depth[4];
	};
	static_assert(sizeof(shader_params) == 192);
}
