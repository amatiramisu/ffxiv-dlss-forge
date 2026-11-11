#pragma once

#include "nr_config.h"
#include "dsr_control.h"
#include <cstdint>

namespace forge
{
	enum class render_lock : int
	{
		game = 0,   // game and NGX decide, including dynamic resolution
		native = 1, // render == output, fixed (pure DLAA)
		fixed = 2,  // DLSS upscaling: render == output * render_scale
	};

	enum class exposure_mode : int
	{
		game = 0,
		on = 1,
		off = 2,
	};

	struct settings
	{
		bool enabled = true;
		bool guest_mode = false;     // another add-on handles NR; Forge retains DLSS/DLAA controls
		render_lock lock = render_lock::native;
		float render_scale = 100.0f; // percent, used by render_lock::fixed
		unsigned int preset = 11;    // NGX preset value; 0 = game/NVIDIA default, 11 = K
		exposure_mode exposure = exposure_mode::game;
		unsigned int desktop_w = 0, desktop_h = 0; // desktop mode to run the game on; 0 x 0 = the display's registry mode
		unsigned int dsr_w = 0, dsr_h = 0, dldsr_w = 0, dldsr_h = 0; // last mode chosen with each filter
		unsigned int dsr_method = WINE_DSR_AUTO;
		int dsr_smoothness = -1, dldsr_smoothness = -1; // -1 = filter default, see filter_smoothness()
		bool indicator = false;      // NVIDIA DLSS on-screen indicator
		bool motion_capture = false; // session-only NGX metadata and GPU measurements
		int motion_capture_test = 0;

		nr::config nr;               // DLSS 5 neural rendering; applied per frame, never re-creates the DLSS feature
	};

	// Smoothness a filter runs with. The DSR default is the Wine startup value, the DL-DSR default is 33 %.
	inline int filter_smoothness(const settings &s, unsigned int method, int startup)
	{
		const int stored = method == WINE_DSR_NEURAL ? s.dldsr_smoothness : s.dsr_smoothness;
		if (stored >= 0) return stored;
		if (method == WINE_DSR_NEURAL) return 33;
		return startup >= 0 ? startup : 0;
	}

	// The game runs under Wine (the prefix has HKCU\Software\Wine); desktop modes are offered only there.
	bool wine();

	settings get_settings();
	// Stores new settings. Bumps the feature generation when a value that needs a DLSS feature re-create changed.
	void set_settings(const settings &s);
	uint64_t feature_generation();
	// Forces a re-create of every live DLSS feature on its next evaluation.
	void bump_feature_generation();
}
