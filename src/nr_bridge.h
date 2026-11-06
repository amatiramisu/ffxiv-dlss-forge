#pragma once

#include "ngx_abi.h"
#include "nr_config.h"
#include <d3d11.h>
#include <cstdint>

// DLSS Neural Rendering (NGX feature 18) for the game's D3D11 frame, inside the game's DLSS evaluate.
//
// nvngx_dlssnr.dll has no D3D11 backend. On DXVK the model runs through NGX's Vulkan API on DXVK's device and
// queue: the DLSS output, depth and motion vectors are packed on the game's context, DXVK submits that work, the
// model follows on the same queue, and the game's context composes the result back. Otherwise, or with
// FORGE_NR_BACKEND=d3d12, it runs on a private D3D12 device synchronised through a shared fence.
namespace forge::nr
{
	struct frame
	{
		ID3D11DeviceContext *ctx = nullptr;
		const void *source = nullptr;       // SR feature identity; compared only, never dereferenced
		ID3D11Resource *image = nullptr;      // DLSS output, read and written back in place
		unsigned int x = 0, y = 0, w = 0, h = 0; // region of `image`
		ID3D11Resource *depth = nullptr;
		ID3D11Resource *motion = nullptr;
		unsigned int depth_x = 0, depth_y = 0, motion_x = 0, motion_y = 0;
		unsigned int guide_w = 0, guide_h = 0; // rendered region of rasterized depth
		unsigned int motion_w = 0, motion_h = 0; // active vector region; zero uses the depth region
		float jitter_x = 0.0f, jitter_y = 0.0f; // current raster jitter in render-resolution pixels
		bool motion_jittered = false;
		float mv_scale_x = 1.0f, mv_scale_y = 1.0f; // the game's DLSS.Input.MV.Scale
		bool depth_inverted = false;
		bool hdr = false;                     // the frame is linear HDR (DLSS IsHDR); otherwise display-referred
		bool reset = false;
	};

	struct pass_status
	{
		bool live = false;
		tuning built;
		char network[64] = {}; // what the snippet reported building for this pass
	};

	struct status
	{
		bool ready = false;  // bridge device and NGX D3D12 are up
		bool active = false; // the last frame ran the model
		bool failed = false; // stopped; `reason` says why
		bool rebuilding = false;
		char reason[200] = {};
		char backend[64] = {};
		int device = -1;     // config::device of the model device that runs, -1 before the first frame
		bool d3d12_builtin = false; // D3D12 was selected, Wine's builtin d3d12.dll loaded, and Vulkan runs the model
		unsigned int work_w = 0, work_h = 0;
		unsigned int frame_w = 0, frame_h = 0; // region of the DLSS output the model edits
		unsigned int guide_w = 0, guide_h = 0; // depth and motion vectors handed to the model
		bool hdr = false;
		bool mask = false;       // the last frame handed the model a control mask
		unsigned int mask_w = 0, mask_h = 0;
		unsigned int passes = 0; // passes that ran last frame
		pass_status pass[max_passes];
		uint64_t frames = 0;
		uint64_t creates = 0;
		double gpu_ms = -1.0;  // all model passes on the GPU
		double sync_ms = -1.0; // Vulkan: CPU time of the per-frame DXVK flush in front of the model
		ngx::Result last_result = ngx::result_success;
	};

	// Runs the configured passes over f.image in place. Returns false when the model did not run this frame.
	// run, release and detach belong to the thread that evaluates DLSS.
	bool run(const frame &f, const config &c);
	// Releases the model features and the per-size textures. The bridge device and NGX stay up for the
	// session: NGX's D3D12 shutdown faults under Proton.
	void release();
	// release() plus everything tied to the game's D3D11 device (shaders, shared fence, device reference)
	void detach();
	// Lets a stopped bridge try again on its next frame; safe from any thread
	void retry();
	// Presentation-thread continuity marker; model/history work stays on the evaluate thread.
	void end_frame();
	status get_status();

	// Test hook: the D3D12 device comes from D3D12CreateDevice directly, the route taken where the D3D12 runtime
	// has no device factory. Call before the first frame.
	void set_skip_device_factory(bool skip);
	// Test hook: with `all_live` every model parameter is written at evaluate and none triggers a rebuild,
	// so the harness can tell which ones the snippet reads per evaluate.
	void set_probe(bool all_live);
	// Test hook: 0 the frame copy, 1 the model input, 2 the first model output, 3 the second model output
	// (pass 2, or the automask estimate), 4 the control mask
	ID3D11Texture2D *debug_texture(int which);
	// Test hook: hands the model this picture as its control mask instead of the painted one. `rgba` holds
	// w * h pixels of four floats, converted to `format` (R8, R8G8, R16, R16G16, R16G16B16A16 float, R32 float,
	// R8G8B8A8); nullptr returns to the painted mask. A probe picture switches the control mask on by itself.
	void set_probe_mask(const float *rgba, unsigned int w, unsigned int h, DXGI_FORMAT format);
	// Parameters the snippet reads at every evaluate; a change to any other one rebuilds the feature.
	bool is_live_parameter(const char *name);
}
