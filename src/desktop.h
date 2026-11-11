#pragma once

#include <windows.h>
#include "dsr_control.h"

// DSR desktop resolution. The display lists modes above its native one (NVIDIA DSR on Windows,
// proton-wineland-ffxiv under Wine); the selected mode is set with ChangeDisplaySettingsEx and the borderless
// game window fills it. The display driver reduces the desktop to the monitor. CDS_FULLSCREEN modes end with
// the process.
namespace forge::desktop
{
	struct mode
	{
		unsigned int w = 0, h = 0, hz = 0;
	};

	constexpr unsigned int max_modes = 24;

	struct status
	{
		bool attached = false;      // game window known
		wchar_t device[32] = {};    // display device the game window is on
		mode native;                // physical display mode, or the registry fallback
		mode original;              // registry mode to restore
		bool physical = false;
		bool dsr_control = false;
		wine_dsr_control dsr = {};
		DWORD dsr_error = 0;
		int dsr_startup_smoothness = -1; // Wine's value before Forge changed it
		mode current;               // the mode the desktop runs
		unsigned int count = 0;     // native and every larger mode, ascending
		mode modes[max_modes];
		unsigned int client_w = 0, client_h = 0;  // the game window's client area
		unsigned int buffer_w = 0, buffer_h = 0;  // the game's back buffer at the last present
		bool borderless = false;    // popup window without frame
		bool switched = false;      // the desktop runs the selected mode
		unsigned long long requests = 0;          // window and swapchain nudges sent to the game
		char reason[160] = {};      // last problem, empty when there is none
	};

	// Desktop modes are switched only under Wine; on Windows the driver's DSR settings choose the desktop and
	// only resetup() runs
	bool available();
	// The game window; safe to call repeatedly
	void attach(HWND window);
	// Per present with the size of the presented back buffer: switches the desktop to the selected mode,
	// then asks the game to fill the monitor when its window or swapchain lags behind
	void present(unsigned int buffer_w, unsigned int buffer_h);
	// Makes the game run its DLSS setup again at its current size: a WM_SIZE with the client size, and when no
	// size query follows within 1.5 s, a WM_SIZE 8 rows shorter and then the client size again
	void resetup();
	// Back to the registry mode after a switch
	void release();
	status get();
	// Match NVIDIA's rounded per-axis scales against the physical display.
	bool neural_mode(const mode &m, const mode &native);
	const char *dsr_reason(unsigned int reason);
}
