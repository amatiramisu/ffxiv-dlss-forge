#pragma once

#include <d3d11.h>
#include <cstdint>

// The game's DLSS motion vectors for ReShade effects. The vector texture comes from the DLSS evaluate, so an effect
// that runs before DLSS in the frame reads the vectors of the same frame. A 2x1 info texture describes them:
//   texel 0: scale from a stored vector to a UV offset (x, y), valid (1 or 0), 0
//   texel 1: the render rectangle in the vector texture, UV offset (x, y) and UV size (w, h)
// An effect samples FORGE_MOTIONVECTORS at info1.xy + uv * info1.zw and multiplies by info0.xy. The result points from
// the current pixel to its position in the previous frame. info0.z is 0 when the game evaluated no DLSS in the last
// frame; without the add-on, ReShade binds an empty texture and it reads 0 too.
namespace forge::motion
{
	constexpr const char *semantic_vectors = "FORGE_MOTIONVECTORS";
	constexpr const char *semantic_info = "FORGE_MOTIONINFO";

	// From the DLSS evaluate, with the context the DLSS feature runs on. (x, y, w, h) is the vector rectangle in pixels,
	// (scale_x, scale_y) the game's MV.Scale: a stored vector times the scale is the offset in pixels of that rectangle.
	void publish(ID3D11DeviceContext *ctx, ID3D11Resource *vectors, unsigned int x, unsigned int y, unsigned int w, unsigned int h,
		float scale_x, float scale_y);

	// After ReShade's effects of a frame (reshade_present): clears the valid flag when no DLSS evaluate came since the last call
	void end_frame();

	// Views to bind (0 when none) and a counter that changes with them
	struct views
	{
		uint64_t vectors = 0, info = 0, generation = 0;
	};
	views get();
	// ReShade binds the views of `generation` now: the views they replaced can go
	void bound(uint64_t generation);

	// The game's device is going away
	void release();
}
