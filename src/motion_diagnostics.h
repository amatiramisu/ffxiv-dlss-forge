#pragma once

#include "ngx_abi.h"
#include <cstdint>

namespace forge { struct settings; namespace nr { struct status; } }

namespace forge::motion_diagnostics
{
	constexpr unsigned int regions = 9;
	constexpr unsigned int columns = 28;
	constexpr unsigned int sample_w = 72, sample_h = 36, sample_planes = 5;
	constexpr unsigned int sample_interval = 8;

	struct feature_info
	{
		const void *game_handle = nullptr, *live_handle = nullptr;
		unsigned int in_w = 0, in_h = 0, out_w = 0, out_h = 0;
		unsigned int effective_w = 0, effective_h = 0;
		int game_flags = 0, effective_flags = 0, game_perf = -1, effective_perf = -1;
		bool output_subrects = false, forced_reset = false;
		int game_reset = 0;
		uint64_t generation = 0, recreates = 0;
		unsigned int game_presets[6] = {}, effective_presets[6] = {};
	};

	struct status
	{
		bool active = false;
		uint64_t frames = 0, measured = 0, dropped = 0, sample_frames = 0, gaps = 0, surfaces = 0;
		double memory_mib = 0.0;
		char folder[128] = {};
		char issue[160] = {};
	};

	struct measurement
	{
		uint64_t frame = 0;
		bool input_history = false, output_history = false;
		float values[regions][columns][4] = {};
	};

	// These calls share the NGX hook's feature lock. Source images are copied at their named stages.
	void begin(ID3D11DeviceContext *ctx, const ngx::Parameter *parameters, const feature_info &feature, const settings &settings);
	void after_sr(ID3D11DeviceContext *ctx, ngx::Result result, bool effective_reset);
	void finish(ID3D11DeviceContext *ctx, const settings &settings, const nr::status &nr);
	void present(ID3D11DeviceContext *ctx, const settings &settings, ID3D11Resource *image = nullptr, unsigned int color_space = 0);
	void after_effects(ID3D11DeviceContext *ctx, ID3D11Resource *image);
	void detach();
	// A capture is requested or still open; every other call can be skipped while this is false
	bool active();
	status get_status();
	measurement last_measurement();
}
