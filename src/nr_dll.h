#pragma once

#include <cstdint>

// The weight sets in the installed nvngx_dlssnr.dll. The snippet's descriptor table maps each
// DLSSNR.Hint.Render.Preset value to an RCDATA resource (CG2RFindWeightByPreset); unknown presets fall back to
// the default. Reading the table lets the Network list show what this DLL has: 310.8.0 has preset 1 (WEIGHTS_HT),
// 310.9.16 adds preset 2 (WEIGHTS_CC_SMALL_FP8).
namespace forge::nr
{
	struct network
	{
		int preset = 0;           // DLSSNR.Hint.Render.Preset value
		char name[96] = {};       // descriptor name, what the snippet logs as "preset=%d -> %s"
		char resource[32] = {};   // RCDATA resource with the weights
		uint64_t bytes = 0;       // size of that resource
	};

	struct dll_info
	{
		bool found = false;       // file opened and parsed
		bool table = false;       // descriptor table located (false: only the resources are known)
		wchar_t path[260] = {};
		char version[32] = {};    // FileVersion from the version resource, empty when absent
		unsigned int count = 0;
		network networks[8];
	};

	// Reads one file: headers, the resource directory (the weights stay on disk) and .rdata for the table.
	void parse_snippet(const wchar_t *path, dll_info &out);

	// Parses the snippet once per path: the loaded module when NGX has it, otherwise nvngx_dlssnr.dll next
	// to the game executable. Cheap after the first call.
	const dll_info &snippet_info();
}
