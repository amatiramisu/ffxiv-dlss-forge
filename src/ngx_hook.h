#pragma once

#include "ngx_abi.h"
#include <windows.h>
#include <cstdint>

namespace forge::ngx_hook
{
	// Patches GetProcAddress in the import table of `module` (the game executable) so its static NGX library
	// resolves the D3D11 entry points to these wrappers.
	bool install(HMODULE module);

	constexpr int history_size = 600;

	struct feature_view
	{
		bool valid = false;
		// What the game asked for
		unsigned int game_in_w = 0, game_in_h = 0, game_out_w = 0, game_out_h = 0;
		int game_perf = -1, game_flags = 0, game_output_subrects = 0;
		unsigned int game_presets[6] = {};
		// What runs
		unsigned int eff_out_w = 0, eff_out_h = 0;
		int eff_perf = -1, eff_flags = 0;
		unsigned int eff_presets[6] = {};
		bool using_own_feature = false;
		uint64_t recreates = 0;
	};

	struct stats
	{
		bool hooked = false;
		char core_path[260] = {};
		unsigned int resolved = 0; // NGX D3D11 entry points routed through the wrappers
		bool callback_wrapped = false;

		uint64_t optimal_calls = 0;
		unsigned int opt_target_w = 0, opt_target_h = 0;
		int opt_perf = -1;
		unsigned int opt_ngx_w = 0, opt_ngx_h = 0, opt_ngx_min_w = 0, opt_ngx_min_h = 0, opt_ngx_max_w = 0, opt_ngx_max_h = 0;
		unsigned int opt_ret_w = 0, opt_ret_h = 0;
		bool opt_overridden = false;

		uint64_t creates = 0;
		uint64_t evaluations = 0;
		uint64_t resets = 0;
		ngx::Result last_result = ngx::result_success;
		ngx::Result last_create_result = ngx::result_success;
		unsigned int render_w = 0, render_h = 0;
		float jitter_x = 0.0f, jitter_y = 0.0f;
		uint64_t render_size_changes = 0;
		float history[history_size] = {}; // render width as % of the game's DLSS output width
		int history_pos = 0;
		int history_count = 0;

		feature_view feature;
		char last_error[160] = {};
	};

	stats get_stats();

	// The resources the game handed its last DLSS evaluate (raw pointers for identity checks only, never dereferenced)
	struct inputs
	{
		void *color = nullptr, *depth = nullptr, *motion = nullptr, *output = nullptr;
		uint64_t evaluation = 0; // ordinal of that evaluate
		unsigned long thread = 0;
	};
	inputs last_inputs();

	// The game's immediate context below ReShade's wrapper. NGX objects created through the wrapper get ReShade
	// destruction callbacks, which can reach another add-on after its destroy_device (D3D11 shares equal samplers
	// and state objects); on the native context they get none.
	void set_native_context(ID3D11DeviceContext *ctx);
	void clear_native_context(ID3D11DeviceContext *ctx);

	// The NGX core the game resolved its entry points from, and its exports through the real GetProcAddress
	HMODULE core_module();
	FARPROC core_proc(const char *name);

	// Writes NGXCore's ShowDlssIndicator registry value (1024 on, 0 off). The snippets read it when NGX
	// initialises them: the game's SR at startup, NR when the bridge starts.
	bool set_indicator_registry(bool on);
	// Proton sets DXVK_NVAPI_SET_NGX_DEBUG_OPTIONS=DLSSIndicator=0 unless PROTON_DLSS_INDICATOR is set, and
	// dxvk-nvapi writes it during NGX init, over the value above. Removing it from the environment first keeps
	// the value written here.
	void claim_indicator();
}
