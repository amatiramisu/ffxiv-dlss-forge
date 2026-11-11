// FFXIV DLSS Forge: ReShade add-on that takes control of FFXIV's own DLSS (D3D11 NGX).
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include "desktop.h"
#include "log.h"
#include "motion_vectors.h"
#include "motion_diagnostics.h"
#include "ngx_hook.h"
#include "nr_bridge.h"
#include "nr_dll.h"
#include "settings.h"

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

extern "C" __declspec(dllexport) const char *NAME = "FFXIV DLSS Forge";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
	"Runs FFXIV's native DLSS the way you configure it: locked DLAA, model presets, DSR desktop resolutions, DLSS 5 neural rendering.";

namespace
{
	constexpr const char *section = "FFXIV_DLSS_FORGE";
	bool g_registered = false;

	float read_float(const char *key, float fallback)
	{
		char buf[32] = {};
		size_t size = sizeof(buf);
		if (!reshade::get_config_value(nullptr, section, key, buf, &size))
			return fallback;
		char *end = nullptr;
		const float v = std::strtof(buf, &end);
		return end != buf ? v : fallback;
	}

	int read_int(const char *key, int fallback)
	{
		int v = fallback;
		return reshade::get_config_value(nullptr, section, key, v) ? v : fallback;
	}

	void write_float(const char *key, float v)
	{
		char buf[32];
		snprintf(buf, sizeof(buf), "%.4f", v);
		reshade::set_config_value(nullptr, section, key, static_cast<const char *>(buf));
	}

	// Per-pass overrides as "preset,style,intensity,structure,tone,skin,mask", "-" where the pass inherits
	void read_profile(const char *key, forge::nr::profile &p)
	{
		char buf[128] = {};
		size_t size = sizeof(buf);
		if (!reshade::get_config_value(nullptr, section, key, buf, &size))
			return;
		char *field[7] = {};
		int n = 0;
		for (char *c = buf; n < 7;)
		{
			field[n++] = c;
			c = std::strchr(c, ',');
			if (c == nullptr)
				break;
			*c++ = '\0';
		}
		if (n != 7)
			return;
		const auto is_set = [](const char *f) { return f[0] != '\0' && std::strcmp(f, "-") != 0; };
		p.preset = is_set(field[0]) ? std::atoi(field[0]) : -1;
		p.style = is_set(field[1]) ? std::atoi(field[1]) : -1;
		p.intensity = is_set(field[2]) ? std::strtof(field[2], nullptr) : NAN;
		p.structure = is_set(field[3]) ? std::strtof(field[3], nullptr) : NAN;
		p.tone = is_set(field[4]) ? std::strtof(field[4], nullptr) : NAN;
		p.skin = is_set(field[5]) ? std::strtof(field[5], nullptr) : NAN;
		p.auto_mask = is_set(field[6]) ? std::atoi(field[6]) : -1;
	}

	void write_profile(const char *key, const forge::nr::profile &p)
	{
		char f[7][16];
		const auto put_i = [](char *b, int v) { if (v < 0) std::strcpy(b, "-"); else snprintf(b, 16, "%d", v); };
		const auto put_f = [](char *b, float v) { if (std::isnan(v)) std::strcpy(b, "-"); else snprintf(b, 16, "%.4f", v); };
		put_i(f[0], p.preset);
		put_i(f[1], p.style);
		put_f(f[2], p.intensity);
		put_f(f[3], p.structure);
		put_f(f[4], p.tone);
		put_f(f[5], p.skin);
		put_i(f[6], p.auto_mask);
		char buf[128];
		snprintf(buf, sizeof(buf), "%s,%s,%s,%s,%s,%s,%s", f[0], f[1], f[2], f[3], f[4], f[5], f[6]);
		reshade::set_config_value(nullptr, section, key, static_cast<const char *>(buf));
	}

	void load_nr(forge::nr::config &c)
	{
		c.enabled = read_int("NREnabled", c.enabled ? 1 : 0) != 0;
		c.position = static_cast<forge::nr::stage>(read_int("NRStage", static_cast<int>(c.position)));
		c.position = c.selected_stage();
		c.temporal = read_int("NRTemporal", c.temporal ? 1 : 0) != 0;
		const char *backend = getenv("FORGE_NR_BACKEND");
		c.device = backend != nullptr && _stricmp(backend, "d3d12") == 0 ? 1 : 0;
		c.passes = static_cast<unsigned int>(read_int("NRPasses", static_cast<int>(c.passes)));
		c.unlock_passes = read_int("NRUnlockPasses", c.unlock_passes ? 1 : 0) != 0;
		forge::nr::profile &b = c.pass[0];
		b.preset = read_int("NRPreset", b.preset);
		b.style = read_int("NRModel", b.style);
		b.intensity = read_float("NRIntensity", b.intensity);
		b.structure = read_float("NRStructure", b.structure);
		b.tone = read_float("NRTone", b.tone);
		b.skin = read_float("NRCharacterStructure", b.skin);
		b.auto_mask = read_int("NRAutoMask", b.auto_mask);
		for (unsigned int i = 1; i < forge::nr::max_passes; ++i)
		{
			char key[16];
			snprintf(key, sizeof(key), "NRPass%u", i + 1);
			read_profile(key, c.pass[i]);
		}
		for (forge::nr::profile &q : c.pass)
			for (float *v : { &q.structure, &q.tone, &q.skin })
				if (*v > 1.0f)
					*v = 1.0f;
		c.work_scale = read_float("NRModelResolution", c.work_scale);
		c.paper_white = read_float("NRPaperWhite", c.paper_white);
		forge::nr::masking &m = c.mask;
		m.enabled = read_int("NRMask", m.enabled ? 1 : 0) != 0;
		m.blend = read_int("NRMaskBlend", m.blend);
		const auto read_group = [&](const char *name, forge::nr::mask_group &g) {
			char key[48];
			snprintf(key, sizeof(key), "NRMask%s", name);
			g.on = read_int(key, g.on ? 1 : 0) != 0;
			snprintf(key, sizeof(key), "NRMask%sStructure", name);
			g.structure = read_float(key, g.structure);
			snprintf(key, sizeof(key), "NRMask%sTone", name);
			g.tone = read_float(key, g.tone);
		};
		read_group("Near", m.near_band);
		read_group("Far", m.far_band);
		read_group("Environment", m.environment);
		m.near_end = read_float("NRMaskNearEnd", m.near_end);
		m.far_start = read_float("NRMaskFarStart", m.far_start);
		m.feather = read_float("NRMaskFeather", m.feather);
		c.apply = read_int("NRApply", c.apply ? 1 : 0) != 0;
		c.compare = read_int("NRCompare", c.compare);
		c.split = read_float("NRCompareSplit", c.split);
		c.swap = read_int("NRCompareSwap", c.swap ? 1 : 0) != 0;
	}

	// Mask preview and debug views are not saved
	void save_nr(const forge::nr::config &c)
	{
		reshade::set_config_value(nullptr, section, "NREnabled", c.enabled ? 1 : 0);
		reshade::set_config_value(nullptr, section, "NRStage", static_cast<int>(c.selected_stage()));
		reshade::set_config_value(nullptr, section, "NRTemporal", c.temporal ? 1 : 0);
		reshade::set_config_value(nullptr, section, "NRPasses", static_cast<int>(c.passes));
		reshade::set_config_value(nullptr, section, "NRUnlockPasses", c.unlock_passes ? 1 : 0);
		const forge::nr::profile &b = c.pass[0];
		reshade::set_config_value(nullptr, section, "NRPreset", b.preset);
		reshade::set_config_value(nullptr, section, "NRModel", b.style);
		write_float("NRIntensity", b.intensity);
		write_float("NRStructure", b.structure);
		write_float("NRTone", b.tone);
		write_float("NRCharacterStructure", b.skin);
		reshade::set_config_value(nullptr, section, "NRAutoMask", b.auto_mask);
		for (unsigned int i = 1; i < forge::nr::max_passes; ++i)
		{
			char key[16];
			snprintf(key, sizeof(key), "NRPass%u", i + 1);
			write_profile(key, c.pass[i]);
		}
		write_float("NRModelResolution", c.work_scale);
		write_float("NRPaperWhite", c.paper_white);
		const forge::nr::masking &m = c.mask;
		reshade::set_config_value(nullptr, section, "NRMask", m.enabled ? 1 : 0);
		reshade::set_config_value(nullptr, section, "NRMaskBlend", m.blend);
		const auto write_group = [&](const char *name, const forge::nr::mask_group &g) {
			char key[48];
			snprintf(key, sizeof(key), "NRMask%s", name);
			reshade::set_config_value(nullptr, section, key, g.on ? 1 : 0);
			snprintf(key, sizeof(key), "NRMask%sStructure", name);
			write_float(key, g.structure);
			snprintf(key, sizeof(key), "NRMask%sTone", name);
			write_float(key, g.tone);
		};
		write_group("Near", m.near_band);
		write_group("Far", m.far_band);
		write_group("Environment", m.environment);
		write_float("NRMaskNearEnd", m.near_end);
		write_float("NRMaskFarStart", m.far_start);
		write_float("NRMaskFeather", m.feather);
		reshade::set_config_value(nullptr, section, "NRApply", c.apply ? 1 : 0);
		reshade::set_config_value(nullptr, section, "NRCompare", c.compare);
		write_float("NRCompareSplit", c.split);
		reshade::set_config_value(nullptr, section, "NRCompareSwap", c.swap ? 1 : 0);
	}

	void load_settings()
	{
		forge::settings s;
		s.enabled = read_int("Enabled", s.enabled ? 1 : 0) != 0;
		s.guest_mode = read_int("GuestMode", s.guest_mode ? 1 : 0) != 0;
		s.lock = static_cast<forge::render_lock>(read_int("RenderLock", static_cast<int>(s.lock)));
		if (static_cast<int>(s.lock) < 0 || static_cast<int>(s.lock) > 2)
			s.lock = forge::render_lock::native;
		s.render_scale = read_float("RenderScale", s.render_scale);
		s.preset = static_cast<unsigned int>(read_int("Preset", static_cast<int>(s.preset)));
		s.exposure = static_cast<forge::exposure_mode>(read_int("Exposure", static_cast<int>(s.exposure)));
		if (static_cast<int>(s.exposure) < 0 || static_cast<int>(s.exposure) > 2)
			s.exposure = forge::exposure_mode::game;
		s.desktop_w = static_cast<unsigned int>(read_int("DesktopWidth", static_cast<int>(s.desktop_w)));
		s.desktop_h = static_cast<unsigned int>(read_int("DesktopHeight", static_cast<int>(s.desktop_h)));
		s.dsr_method = static_cast<unsigned int>(read_int("DsrMethod", s.dsr_method));
		s.dsr_smoothness = read_int("DsrSmoothness", s.dsr_smoothness);
		s.dldsr_smoothness = read_int("DldsrSmoothness", s.dldsr_smoothness);
		s.dsr_w = static_cast<unsigned int>(read_int("DsrWidth", static_cast<int>(s.dsr_w)));
		s.dsr_h = static_cast<unsigned int>(read_int("DsrHeight", static_cast<int>(s.dsr_h)));
		s.dldsr_w = static_cast<unsigned int>(read_int("DldsrWidth", static_cast<int>(s.dldsr_w)));
		s.dldsr_h = static_cast<unsigned int>(read_int("DldsrHeight", static_cast<int>(s.dldsr_h)));
		s.indicator = read_int("Indicator", s.indicator ? 1 : 0) != 0;
		load_nr(s.nr);
		forge::set_settings(s);
	}

	void save_settings(const forge::settings &s)
	{
		reshade::set_config_value(nullptr, section, "Enabled", s.enabled ? 1 : 0);
		reshade::set_config_value(nullptr, section, "GuestMode", s.guest_mode ? 1 : 0);
		reshade::set_config_value(nullptr, section, "RenderLock", static_cast<int>(s.lock));
		write_float("RenderScale", s.render_scale);
		reshade::set_config_value(nullptr, section, "Preset", static_cast<int>(s.preset));
		reshade::set_config_value(nullptr, section, "Exposure", static_cast<int>(s.exposure));
		reshade::set_config_value(nullptr, section, "DesktopWidth", static_cast<int>(s.desktop_w));
		reshade::set_config_value(nullptr, section, "DesktopHeight", static_cast<int>(s.desktop_h));
		reshade::set_config_value(nullptr, section, "DsrMethod", static_cast<int>(s.dsr_method));
		reshade::set_config_value(nullptr, section, "DsrSmoothness", s.dsr_smoothness);
		reshade::set_config_value(nullptr, section, "DldsrSmoothness", s.dldsr_smoothness);
		reshade::set_config_value(nullptr, section, "DsrWidth", static_cast<int>(s.dsr_w));
		reshade::set_config_value(nullptr, section, "DsrHeight", static_cast<int>(s.dsr_h));
		reshade::set_config_value(nullptr, section, "DldsrWidth", static_cast<int>(s.dldsr_w));
		reshade::set_config_value(nullptr, section, "DldsrHeight", static_cast<int>(s.dldsr_h));
		reshade::set_config_value(nullptr, section, "Indicator", s.indicator ? 1 : 0);
		save_nr(s.nr);
	}

	struct preset_item { unsigned int value; const char *label; };
	constexpr preset_item presets[] = {
		{ 0, "Game / NVIDIA default" },
		{ 11, "K - transformer (DLAA and Quality default)" },
		{ 10, "J - transformer" },
		{ 13, "M - transformer 2 (Performance default)" },
		{ 12, "L - transformer 2 (Ultra Performance default)" },
		{ 6, "F - CNN" },
		{ 5, "E - CNN" },
	};

	struct level_item { float scale; const char *label; };
	constexpr level_item levels[] = {
		{ 66.7f, "Quality (66.7 %)" },
		{ 58.0f, "Balanced (58 %)" },
		{ 50.0f, "Performance (50 %)" },
		{ 33.4f, "Ultra Performance (33.3 %)" },
	};

	void preset_text(char *buf, size_t size, const unsigned int p[6])
	{
		using namespace ngx;
		snprintf(buf, size, "DLAA %c  Q %c  B %c  P %c  UP %c  UQ %c",
			preset_letter(p[perf_dlaa]), preset_letter(p[perf_max_quality]), preset_letter(p[perf_balanced]),
			preset_letter(p[perf_max_perf]), preset_letter(p[perf_ultra_performance]), preset_letter(p[perf_ultra_quality]));
	}

	void flags_text(char *buf, size_t size, int f)
	{
		using namespace ngx;
		snprintf(buf, size, "0x%X%s%s%s%s%s", f,
			(f & flag_is_hdr) ? " HDR" : " LDR",
			(f & flag_mv_low_res) ? " MV-render-res" : "",
			(f & flag_mv_jittered) ? " MV-jittered" : "",
			(f & flag_depth_inverted) ? " depth-inverted" : "",
			(f & flag_auto_exposure) ? " auto-exposure" : "");
	}

	constexpr ImVec4 colour_ok(0.55f, 0.9f, 0.55f, 1.0f);
	constexpr ImVec4 colour_issue(1.0f, 0.6f, 0.35f, 1.0f);
	constexpr float nr_width = 260.0f;

	// SliderFloat that snaps to `step` while dragged; Ctrl+click types any value
	bool snap_slider(const char *label, float *v, float lo, float hi, const char *format, ImGuiSliderFlags flags, float step = 0.05f)
	{
		const bool changed = ImGui::SliderFloat(label, v, lo, hi, format, flags);
		if (changed && ImGui::IsMouseDown(ImGuiMouseButton_Left) && step > 0.0f)
		{
			const float snapped = std::round(*v / step) * step;
			*v = snapped < lo ? lo : (snapped > hi ? hi : snapped);
		}
		return changed;
	}

	// Network list from the installed snippet: Default, then its weight sets. `presets` receives the
	// DLSSNR.Hint.Render.Preset value of each item.
	struct network_items
	{
		char text[16 * 80];
		int presets[16];
		int count = 0;

		network_items(int current)
		{
			const forge::nr::dll_info &d = forge::nr::snippet_info();
			size_t at = 0;
			const auto add = [&](int preset, const char *item) {
				const size_t len = std::strlen(item);
				if (count >= 16 || at + len + 2 > sizeof(text))
					return;
				std::memcpy(text + at, item, len + 1);
				at += len + 1;
				presets[count++] = preset;
			};
			add(0, "Default");
			bool listed = current <= 0;
			char item[96];
			if (d.table)
			{
				for (unsigned int i = 0; i < d.count; ++i)
				{
					const forge::nr::network &n = d.networks[i];
					if (n.preset <= 0)
						continue;
					snprintf(item, sizeof(item), "%d: %s, %.0f MB", n.preset, n.resource, static_cast<double>(n.bytes) / 1e6);
					add(n.preset, item);
					listed |= n.preset == current;
				}
			}
			else
			{
				for (int p = 1; p <= 3; ++p)
				{
					snprintf(item, sizeof(item), "Preset %d", p);
					add(p, item);
					listed |= p == current;
				}
			}
			if (!listed)
			{
				snprintf(item, sizeof(item), "Preset %d", current);
				add(current, item);
			}
			text[at] = 0;
		}

		int index(int preset) const
		{
			for (int i = 0; i < count; ++i)
				if (presets[i] == preset)
					return i;
			return 0;
		}
	};

	void network_tooltip()
	{
		const forge::nr::dll_info &d = forge::nr::snippet_info();
		char tip[16 * 112 + 160];
		size_t at = static_cast<size_t>(snprintf(tip, sizeof(tip), "DLSSNR.Hint.Render.Preset"));
		if (d.found)
			at += static_cast<size_t>(snprintf(tip + at, sizeof(tip) - at, "\nnvngx_dlssnr.dll %s", d.version[0] ? d.version : ""));
		for (unsigned int i = 0; i < d.count && at < sizeof(tip); ++i)
		{
			const forge::nr::network &n = d.networks[i];
			if (n.preset > 0 && n.name[0])
				at += static_cast<size_t>(snprintf(tip + at, sizeof(tip) - at, "\n%d: %s", n.preset, n.name));
		}
		ImGui::SetItemTooltip("%s", tip);
	}

	// Pass values. Later passes show inherited values dimmed until changed.
	void dim_begin(bool inherited)
	{
		if (inherited)
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	}

	void dim_end(bool inherited)
	{
		if (inherited)
			ImGui::PopStyleColor();
	}

	bool value_slider(const char *label, float &v, float inherited, float lo, float hi, const char *tip)
	{
		const bool inh = std::isnan(v);
		float shown = inh ? inherited : v;
		dim_begin(inh);
		ImGui::SetNextItemWidth(nr_width);
		const bool changed = snap_slider(label, &shown, lo, hi, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		dim_end(inh);
		if (tip != nullptr)
			ImGui::SetItemTooltip("%s", tip);
		if (changed)
			v = shown;
		return changed;
	}

	// Base Character: NaN inherits, -1 follows Structure Intensity, other values are the slider value
	bool character_control(float &v, float inherited, float structure)
	{
		const bool inh = std::isnan(v);
		const float value = inh ? inherited : v;
		bool follow = value < 0.0f;
		float shown = follow ? structure : value;
		bool changed = false;
		dim_begin(inh);
		ImGui::BeginDisabled(follow);
		ImGui::SetNextItemWidth(nr_width);
		if (snap_slider("Base Character", &shown, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp))
		{
			v = shown;
			changed = true;
		}
		ImGui::SetItemTooltip("DLSSNR.SkinStructureStrength. Range 0 to 1.");
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Checkbox("Same as Structure Intensity", &follow))
		{
			v = follow ? -1.0f : structure;
			changed = true;
		}
		dim_end(inh);
		return changed;
	}

	// A sub-heading inside the DLSS 5 block, named as in NVIDIA's DLSS 5 Developer Controls panel
	void nr_heading(const char *text)
	{
		ImGui::Spacing();
		ImGui::TextColored(ImVec4(0.75f, 0.85f, 1.0f, 1.0f), "%s", text);
	}

	// The controls of one pass, the same for every pass
	void draw_pass(forge::nr::config &c, unsigned int i, bool &changed)
	{
		forge::nr::config plain = c;
		if (i > 0)
			plain.pass[i] = forge::nr::profile {};
		const forge::nr::tuning inh = plain.resolve(i);
		const forge::nr::tuning now = c.resolve(i);
		forge::nr::profile &p = c.pass[i];

		nr_heading("Global Controls");
		changed |= value_slider("Intensity", p.intensity, inh.intensity, 0.0f, 1.0f, "DLSSNR.Intensity. 0 gives the DLSS frame. 1 gives the full model output.");
		changed |= value_slider("Structure Intensity", p.structure, inh.structure, 0.0f, 1.0f, "DLSSNR.LocalStructureStrength. Range 0 to 1.");
		changed |= value_slider("Tone Intensity", p.tone, inh.tone, 0.0f, 1.0f, "DLSSNR.LocalToneStrength. Range 0 to 1.");

		nr_heading("Model Automask");
		{
			const bool inh_mask = p.auto_mask < 0;
			bool mask = inh_mask ? inh.auto_mask : p.auto_mask != 0;
			dim_begin(inh_mask);
			if (ImGui::Checkbox("Automask", &mask))
			{
				p.auto_mask = mask ? 1 : 0;
				changed = true;
			}
			dim_end(inh_mask);
			ImGui::SetItemTooltip("DLSSNR.UseAutoMask. The model finds characters. It applies Base Character to them.");
		}
		if (i == 0)
		{
			ImGui::SameLine();
			ImGui::BeginDisabled(!now.auto_mask);
			changed |= ImGui::Checkbox("Show Mask", &c.show_automask);
			ImGui::SetItemTooltip("Shows the character mask of pass 1 in red. This uses one more model pass. Other passes stop while the mask shows.");
			ImGui::EndDisabled();
		}
		ImGui::BeginDisabled(!now.auto_mask);
		changed |= character_control(p.skin, inh.skin, now.structure);
		ImGui::EndDisabled();

		nr_heading("Models");
		{
			const bool inh_style = p.style < 0;
			int style = static_cast<int>(inh_style ? inh.style : static_cast<unsigned int>(p.style));
			dim_begin(inh_style);
			bool style_changed = ImGui::RadioButton("Model A", &style, 0);
			ImGui::SameLine();
			style_changed |= ImGui::RadioButton("Model B", &style, 1);
			ImGui::SameLine();
			style_changed |= ImGui::RadioButton("Model C", &style, 2);
			dim_end(inh_style);
			ImGui::SameLine();
			ImGui::TextDisabled("(?)");
			ImGui::SetItemTooltip("DLSSNR.Style");
			if (style_changed)
			{
				p.style = style;
				changed = true;
			}
		}
		{
			const bool inh_net = p.preset < 0;
			const int current = inh_net ? static_cast<int>(inh.preset) : p.preset;
			const network_items items(current);
			int index = items.index(current);
			dim_begin(inh_net);
			ImGui::SetNextItemWidth(nr_width);
			if (ImGui::Combo("Network", &index, items.text, -1))
			{
				p.preset = items.presets[index];
				changed = true;
			}
			dim_end(inh_net);
			network_tooltip();
		}

		ImGui::Spacing();
		if (ImGui::Button("Reset pass to defaults"))
		{
			c.reset_pass(i);
			changed = true;
		}
		ImGui::SetItemTooltip("%s", i == 0 ?
			"Default network, Model A, Intensity 1, Structure Intensity 1, Tone Intensity 1, Automask on. Base Character follows Structure Intensity. Later passes inherit the reset values where inheritance is selected." :
			"Restores inheritance from pass 1. Tone Intensity returns to 0.");
	}

	// One row of the Developer Masking table: the group's switch, its structure and tone values, and an
	// optional distance control
	bool mask_row(const char *name, forge::nr::mask_group &g, bool switchable, float *distance, const char *distance_label, const char *tip)
	{
		bool changed = false;
		ImGui::PushID(name);
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		if (switchable)
			changed |= ImGui::Checkbox(name, &g.on);
		else
			ImGui::TextUnformatted(name);
		if (tip != nullptr)
			ImGui::SetItemTooltip("%s", tip);
		ImGui::BeginDisabled(!g.on);
		ImGui::TableSetColumnIndex(1);
		ImGui::SetNextItemWidth(-FLT_MIN);
		changed |= snap_slider("##structure", &g.structure, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::TableSetColumnIndex(2);
		ImGui::SetNextItemWidth(-FLT_MIN);
		changed |= snap_slider("##tone", &g.tone, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::TableSetColumnIndex(3);
		if (distance != nullptr)
		{
			ImGui::SetNextItemWidth(-FLT_MIN);
			changed |= ImGui::SliderFloat("##distance", distance, 1.0f, 10000.0f, distance_label, ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
			ImGui::SetItemTooltip("Distance in multiples of the camera near plane.");
		}
		ImGui::EndDisabled();
		ImGui::PopID();
		return changed;
	}

	void draw_nr(forge::nr::config &c, bool &changed)
	{
		const forge::nr::status st = forge::nr::get_status();
		ImGui::SeparatorText("DLSS 5 neural rendering");
		changed |= ImGui::Checkbox("DLSS 5", &c.enabled);
		ImGui::SameLine();
		if (st.failed)
		{
			ImGui::TextColored(colour_issue, "stopped: %s", st.reason);
			ImGui::SameLine();
			if (ImGui::SmallButton("Retry"))
				forge::nr::retry();
		}
		else if (!c.enabled)
			ImGui::TextDisabled("off");
		else if (st.active)
		{
			char gpu[64] = "";
			if (st.gpu_ms >= 0.0)
				snprintf(gpu, sizeof(gpu), ", %.2f ms GPU", st.gpu_ms);
			if (st.sync_ms >= 0.0)
				snprintf(gpu + std::strlen(gpu), sizeof(gpu) - std::strlen(gpu), ", %.2f ms sync", st.sync_ms);
			ImGui::TextColored(colour_ok, "running: %u pass%s, model %u x %u%s%s%s", st.passes, st.passes == 1 ? "" : "es", st.work_w, st.work_h, gpu,
				st.mask ? ", control mask" : "", st.rebuilding ? ", rebuilding" : "");
		}
		else if (st.reason[0] != '\0')
			ImGui::TextColored(colour_issue, "%s", st.reason);
		else
			ImGui::TextDisabled("starts at the next DLSS frame");

		if (st.d3d12_builtin)
			ImGui::TextColored(colour_issue, "FORGE_NR_BACKEND=d3d12 needs the DLL overrides d3d12,d3d12core=n. The model runs on Vulkan.");

		if (ImGui::Button("Reset DLSS 5 to defaults"))
		{
			c.reset_defaults();
			changed = true;
		}
		ImGui::SetItemTooltip("Restores all DLSS 5 controls and all 30 pass profiles. DLSS 5 off, one pass, After upscaling, temporal input on, model resolution 100%%. Clears masks, comparison views and Frame log. Keeps the environment-selected backend.");

		ImGui::BeginDisabled(!c.enabled);
		int position = static_cast<int>(c.selected_stage());
		ImGui::SetNextItemWidth(nr_width);
		if (ImGui::Combo("DLSS 5 stage", &position, "Before upscaling\0After upscaling\0Present\0"))
		{
			c.position = static_cast<forge::nr::stage>(position);
			changed = true;
		}
		ImGui::SetItemTooltip("Before: processes the DLSS input. After: processes the DLSS output. Present: processes the final game image before ReShade effects and overlay.");

		int passes = static_cast<int>(c.pass_count());
		ImGui::SetNextItemWidth(nr_width);
		if (ImGui::SliderInt("Passes", &passes, 1, static_cast<int>(c.pass_limit()), "%d", ImGuiSliderFlags_AlwaysClamp))
		{
			c.passes = static_cast<unsigned int>(passes);
			changed = true;
		}
		ImGui::SetItemTooltip("Each pass is one more model. It uses the output of the pass before it.");
		ImGui::SameLine();
		if (ImGui::Checkbox("Up to 30", &c.unlock_passes))
		{
			c.passes = c.pass_count();
			changed = true;
		}
		if (ImGui::BeginTabBar("##passes", ImGuiTabBarFlags_FittingPolicyScroll))
		{
			for (unsigned int i = 0; i < c.pass_count(); ++i)
			{
				char name[16];
				snprintf(name, sizeof(name), "Pass %u", i + 1);
				if (ImGui::BeginTabItem(name))
				{
					ImGui::PushID(static_cast<int>(i));
					draw_pass(c, i, changed);
					ImGui::PopID();
					ImGui::EndTabItem();
				}
			}
			ImGui::EndTabBar();
		}

		// Developer Masking
		nr_heading("Developer Masking");
		forge::nr::masking &m = c.mask;
		changed |= ImGui::Checkbox("Developer masking", &m.enabled);
		ImGui::SetItemTooltip("DLSSNR.ControlMask. Forge makes the mask from the game depth. Each group sets the structure and tone values of its pixels.\n"
			"The model multiplies these values with the values of the pass. The mask applies to all passes.");
		ImGui::SameLine();
		ImGui::BeginDisabled(!m.enabled);
		changed |= ImGui::Checkbox("Show Masks", &m.show);
		ImGui::SetItemTooltip("Near band: blue. Far band: violet.");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(220.0f);
		changed |= ImGui::Combo("##blend", &m.blend, "Mask only (automask off)\0Average with automask\0Minimum with automask\0Maximum with automask\0", -1);
		ImGui::SetItemTooltip("DLSSNR.MaskBlendMode. 0 replaces the automask. 1 averages, 2 takes the minimum, 3 takes the maximum.");
		if (ImGui::BeginTable("##groups", 4, ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("Group", ImGuiTableColumnFlags_WidthFixed, 150.0f);
			ImGui::TableSetupColumn("Structure", ImGuiTableColumnFlags_WidthStretch, 1.0f);
			ImGui::TableSetupColumn("Tone", ImGuiTableColumnFlags_WidthStretch, 1.0f);
			ImGui::TableSetupColumn("Distance", ImGuiTableColumnFlags_WidthStretch, 1.0f);
			ImGui::TableHeadersRow();
			changed |= mask_row("Near", m.near_band, true, &m.near_end, "up to %.0f", "Pixels closer than the distance.");
			changed |= mask_row("Far", m.far_band, true, &m.far_start, "from %.0f", "Pixels farther than the distance.");
			changed |= mask_row("Environment", m.environment, false, nullptr, nullptr, "All other pixels.");
			ImGui::EndTable();
		}
		ImGui::SetNextItemWidth(nr_width);
		changed |= snap_slider("Band feather", &m.feather, 0.0f, 2.0f, "%.2f octaves", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SetItemTooltip("Edge width of the bands, in octaves of distance.");
		ImGui::EndDisabled();

		if (ImGui::TreeNode("Input and resolution"))
		{
			changed |= ImGui::Checkbox("Motion-guided history", &c.temporal);
			ImGui::SetItemTooltip("Uses the game motion vectors for NVIDIA temporal history. Off selects colour-only evaluation.");
			if (st.hdr)
			{
				ImGui::SetNextItemWidth(nr_width);
				changed |= ImGui::SliderFloat("Paper white", &c.paper_white, 0.1f, 16.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
				ImGui::SetItemTooltip("The scene value that the model shows as white.");
			}
			float pct = c.work_scale * 100.0f;
			ImGui::SetNextItemWidth(nr_width);
			if (ImGui::SliderFloat("Model resolution", &pct, 25.0f, 200.0f, "%.0f %%", ImGuiSliderFlags_AlwaysClamp))
			{
				c.work_scale = pct / 100.0f;
				changed = true;
			}
			ImGui::SetItemTooltip("100%% preserves the model output pixel for pixel. Other sizes resize the model output to the selected stage image.");
			ImGui::TreePop();
		}

		if (ImGui::TreeNode("Compare and inspect"))
		{
			changed |= ImGui::Checkbox("Apply model", &c.apply);
			ImGui::SetItemTooltip("Off shows the original stage image. The model continues to run.");
			ImGui::SetNextItemWidth(nr_width);
			changed |= ImGui::Combo("Compare", &c.compare, "Off\0Side by side\0Wipe\0", -1);
			if (c.compare != 0)
			{
				ImGui::SetNextItemWidth(nr_width);
				changed |= snap_slider("Split", &c.split, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp, 0.01f);
				ImGui::SameLine();
				changed |= ImGui::Checkbox("Swap sides", &c.swap);
			}
			ImGui::SetNextItemWidth(nr_width);
			changed |= ImGui::Combo("Debug view", &c.debug_view, "Off\0Model input\0Model output\0Difference x20\0", -1);
			changed |= ImGui::Checkbox("Frame log", &c.frame_log);
			ImGui::SetItemTooltip("Writes per-frame values to ffxiv-dlss-forge.log.");
			if (st.ready)
				ImGui::TextDisabled("%s, frame %u x %u, guides %u x %u%s, %s, %llu frames, %llu model builds, last 0x%08X", st.backend, st.frame_w, st.frame_h, st.guide_w, st.guide_h,
					st.mask ? ", control mask" : "", st.hdr ? "HDR" : "display-referred", static_cast<unsigned long long>(st.frames), static_cast<unsigned long long>(st.creates), st.last_result);
			ImGui::TreePop();
		}
		ImGui::EndDisabled();
	}

	void draw_desktop(forge::settings &s, bool &changed)
	{
		ImGui::SeparatorText("Display supersampling");
		const auto ds = forge::desktop::get();
		if (ds.native.w)
			ImGui::Text("Display  %u x %u @ %u Hz", ds.native.w, ds.native.h, ds.native.hz);
		else
			ImGui::TextDisabled("Waiting for the display");

		struct choice { unsigned int w = 0, h = 0; int smoothness = 0; };
		static unsigned int method = WINE_DSR_OFF, width = 0, height = 0;
		static choice last[WINE_DSR_NEURAL + 1]; // last choice per filter, by method
		static int smoothness = 0;
		static bool pending = false, smooth_active = false;
		// Without the Wine control, the driver owns the filter and Forge only picks the desktop mode.
		const bool live = ds.dsr_control;
		const int startup_smoothness = ds.dsr_startup_smoothness;
		const unsigned int selected_w = s.desktop_w ? s.desktop_w : ds.original.w;
		const unsigned int selected_h = s.desktop_h ? s.desktop_h : ds.original.h;
		unsigned int current_method = s.dsr_method;
		if (current_method == WINE_DSR_AUTO)
			current_method = !selected_w || (selected_w == ds.native.w && selected_h == ds.native.h) ? WINE_DSR_OFF :
				ds.dsr.active_method == WINE_DSR_NEURAL ? WINE_DSR_NEURAL : WINE_DSR_GAUSSIAN;
		const unsigned int wanted_w = current_method == WINE_DSR_OFF ? ds.native.w : selected_w;
		const unsigned int wanted_h = current_method == WINE_DSR_OFF ? ds.native.h : selected_h;
		const auto saved_smoothness = [&](unsigned int value) { return forge::filter_smoothness(s, value, startup_smoothness); };
		if (!pending && !smooth_active)
		{
			method = current_method;
			width = selected_w;
			height = selected_h;
			smoothness = saved_smoothness(method);
			last[WINE_DSR_GAUSSIAN] = { s.dsr_w, s.dsr_h, saved_smoothness(WINE_DSR_GAUSSIAN) };
			last[WINE_DSR_NEURAL] = { s.dldsr_w, s.dldsr_h, saved_smoothness(WINE_DSR_NEURAL) };
		}
		const auto remember = [&] {
			if (method != WINE_DSR_OFF) last[method] = { width, height, smoothness };
		};
		const auto eligible = [&](const forge::desktop::mode &m) {
			return m.w > ds.native.w && m.h > ds.native.h &&
				(method != WINE_DSR_NEURAL || forge::desktop::neural_mode(m, ds.native));
		};
		const auto select_resolution = [&] {
			for (unsigned int i = 0; i < ds.count; ++i)
				if (eligible(ds.modes[i]) && ds.modes[i].w == width && ds.modes[i].h == height) return;
			width = height = 0;
			for (unsigned int i = 0; i < ds.count; ++i)
			{
				const auto &m = ds.modes[i];
				if (!eligible(m)) continue;
				if (!width || (m.w == (static_cast<uint64_t>(ds.native.w) * 15000 + 5000) / 10000 &&
					m.h == (static_cast<uint64_t>(ds.native.h) * 15000 + 5000) / 10000))
				{
					width = m.w;
					height = m.h;
				}
			}
		};
		const bool neural = ds.dsr_control && ds.physical && (ds.dsr.capabilities & WINE_DSR_CAP_NEURAL);
		const char *names[] = {"Off", "DSR", "DL-DSR"};
		for (unsigned int i = WINE_DSR_OFF; live && i <= WINE_DSR_NEURAL; ++i)
		{
			if (i != WINE_DSR_OFF) ImGui::SameLine();
			ImGui::BeginDisabled(i == WINE_DSR_NEURAL && !neural);
			if (ImGui::RadioButton(names[i - WINE_DSR_OFF], method == i))
			{
				remember();
				method = i;
				pending = true;
				if (method == WINE_DSR_OFF)
					smoothness = saved_smoothness(method);
				else
				{
					smoothness = last[method].smoothness;
					if (last[method].w)
					{
						width = last[method].w;
						height = last[method].h;
					}
					select_resolution();
				}
			}
			ImGui::EndDisabled();
			if (i == WINE_DSR_NEURAL && !neural && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip("DL-DSR needs a supported NVIDIA device. Proton Wineland FFXIV releases include the assets. Source builds need user-provided assets before the game starts.");
		}

		bool valid = method == WINE_DSR_OFF, larger = false;
		for (unsigned int i = 0; i < ds.count; ++i)
		{
			valid |= eligible(ds.modes[i]) && ds.modes[i].w == width && ds.modes[i].h == height;
			larger |= ds.modes[i].w > ds.native.w && ds.modes[i].h > ds.native.h;
		}
		char preview[96];
		const auto resolution_text = [&](char *text, size_t size, unsigned int w, unsigned int h) {
			const double factor = ds.native.w && ds.native.h ? static_cast<double>(w) * h / (static_cast<double>(ds.native.w) * ds.native.h) : 0.0;
			snprintf(text, size, "%u x %u - %.2fx", w, h, factor);
		};
		if (method == WINE_DSR_OFF) snprintf(preview, sizeof(preview), "Native");
		else
		{
			resolution_text(preview, sizeof(preview), width, height);
			if (!valid) std::strcat(preview, " (unavailable)");
		}
		ImGui::BeginDisabled(live && method == WINE_DSR_OFF);
		ImGui::SetNextItemWidth(420.0f);
		if (ImGui::BeginCombo("Game output", preview))
		{
			if (!live && ImGui::Selectable("Native", method == WINE_DSR_OFF))
			{
				method = WINE_DSR_OFF;
				valid = true;
				pending = true;
			}
			for (unsigned int i = 0; i < ds.count; ++i)
			{
				const auto &m = ds.modes[i];
				if (!eligible(m)) continue;
				char label[96];
				resolution_text(label, sizeof(label), m.w, m.h);
				if (ImGui::Selectable(label, m.w == width && m.h == height))
				{
					if (!live) method = WINE_DSR_GAUSSIAN;
					width = m.w;
					height = m.h;
					valid = true;
					pending = true;
				}
			}
			ImGui::EndCombo();
		}
		ImGui::EndDisabled();
		ImGui::SetItemTooltip("Pixel-count factors of the physical display. The game uses Borderless Windowed mode.");

		smooth_active = false;
		if (live)
		{
			ImGui::BeginDisabled(method == WINE_DSR_OFF);
			ImGui::SetNextItemWidth(420.0f);
			const bool smooth_changed = ImGui::SliderInt("Smoothness", &smoothness, 0, 100, "%d %%", ImGuiSliderFlags_AlwaysClamp);
			smooth_active = ImGui::IsItemActive();
			if (!pending && (ImGui::IsItemDeactivatedAfterEdit() || (smooth_changed && !smooth_active)))
			{
				s.dsr_method = method;
				(method == WINE_DSR_NEURAL ? s.dldsr_smoothness : s.dsr_smoothness) = smoothness;
				changed = true;
			}
			ImGui::SetItemTooltip("%s Changes apply when you release the slider.", method == WINE_DSR_NEURAL ?
				"Higher values reduce native sharpening." : "Higher values broaden the Gaussian filter.");
			ImGui::EndDisabled();
		}

		if (pending)
		{
			ImGui::BeginDisabled(!valid || (method == WINE_DSR_NEURAL && !neural));
			if (ImGui::Button("Apply display settings"))
			{
				remember();
				s.dsr_method = method;
				s.desktop_w = width;
				s.desktop_h = height;
				s.dsr_w = last[WINE_DSR_GAUSSIAN].w;
				s.dsr_h = last[WINE_DSR_GAUSSIAN].h;
				s.dsr_smoothness = last[WINE_DSR_GAUSSIAN].smoothness;
				s.dldsr_w = last[WINE_DSR_NEURAL].w;
				s.dldsr_h = last[WINE_DSR_NEURAL].h;
				s.dldsr_smoothness = last[WINE_DSR_NEURAL].smoothness;
				changed = true;
				pending = false;
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button("Cancel display changes")) pending = false;
		}

		if (live && ds.dsr_error == ERROR_NOT_READY)
			ImGui::TextDisabled("Waiting for the game's Vulkan surface");
		else if (live && ds.dsr_error)
			ImGui::TextColored(colour_issue, "Display control failed with error %lu", ds.dsr_error);
		else if (live && ds.dsr.applied_generation != ds.dsr.generation)
			ImGui::TextDisabled("Applying display settings");
		else if (ds.switched && !ds.borderless)
			ImGui::TextColored(colour_issue, "The game needs Borderless Windowed mode.");
		else if (wanted_w && (ds.current.w != wanted_w || ds.current.h != wanted_h ||
			 ds.buffer_w != wanted_w || ds.buffer_h != wanted_h))
			ImGui::TextDisabled("Updating game output");
		else if (!live)
		{
			if (ds.attached && !larger)
				ImGui::TextDisabled("%ls lists no mode above %u x %u", ds.device, ds.native.w, ds.native.h);
		}
		else if (ds.dsr.reason != WINE_DSR_READY && ds.dsr.reason != WINE_DSR_NATIVE && ds.dsr.reason != WINE_DSR_DISABLED)
			ImGui::TextColored(colour_issue, "%s: %s", ds.dsr.active_method == WINE_DSR_GAUSSIAN ? "DSR fallback" : "Downsampling unavailable",
				forge::desktop::dsr_reason(ds.dsr.reason));
		else if (ds.dsr.active_method == WINE_DSR_NEURAL || ds.dsr.active_method == WINE_DSR_GAUSSIAN)
			ImGui::TextColored(colour_ok, "%s active", ds.dsr.active_method == WINE_DSR_NEURAL ? "DL-DSR" : "DSR");
		else if (current_method != WINE_DSR_OFF)
			ImGui::TextColored(colour_issue, "Downsampling is inactive on this presentation path");
		if (ds.reason[0]) ImGui::TextColored(colour_issue, "%s", ds.reason);
	}

	// FileVersion of the loaded DLSS SR snippet, read once it is loaded
	const char *dlss_version()
	{
		static char version[32] = {};
		static ULONGLONG last_try = 0;
		if (version[0] != 0)
			return version;
		const ULONGLONG now = GetTickCount64();
		if (now - last_try < 2000)
			return "";
		last_try = now;
		const HMODULE m = GetModuleHandleW(L"nvngx_dlss.dll");
		wchar_t path[MAX_PATH] = {};
		if (m == nullptr || GetModuleFileNameW(m, path, MAX_PATH) == 0)
			return "";
		DWORD handle = 0;
		const DWORD n = GetFileVersionInfoSizeW(path, &handle);
		if (n == 0)
			return "";
		std::vector<uint8_t> buf(n);
		VS_FIXEDFILEINFO *info = nullptr;
		UINT len = 0;
		if (!GetFileVersionInfoW(path, 0, n, buf.data()) || !VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void **>(&info), &len) || info == nullptr)
			return "";
		snprintf(version, sizeof(version), "%u.%u.%u", static_cast<unsigned>(HIWORD(info->dwFileVersionMS)), static_cast<unsigned>(LOWORD(info->dwFileVersionMS)),
			static_cast<unsigned>(HIWORD(info->dwFileVersionLS)));
		return version;
	}

	// The on-screen indicator, bottom left, every frame after the effects
	void on_reshade_overlay(reshade::api::effect_runtime *)
	{
		const forge::settings s = forge::get_settings();
		if (!s.indicator)
			return;
		const forge::ngx_hook::stats st = forge::ngx_hook::get_stats();
		const forge::nr::status ns = forge::nr::get_status();
		char line1[200] = {}, line2[200] = {};
		const auto &f = st.feature;
		if (f.valid)
		{
			const int perf = f.eff_perf >= 0 && f.eff_perf < 6 ? f.eff_perf : 0;
			if (st.render_w == f.eff_out_w && st.render_h == f.eff_out_h)
				snprintf(line1, sizeof(line1), "DLSS %s  preset %c  %u x %u  %s", dlss_version(), ngx::preset_letter(f.eff_presets[perf]),
					f.eff_out_w, f.eff_out_h, ngx::perf_quality_label(f.eff_perf));
			else
				snprintf(line1, sizeof(line1), "DLSS %s  preset %c  %u x %u -> %u x %u  %s", dlss_version(), ngx::preset_letter(f.eff_presets[perf]),
					st.render_w, st.render_h, f.eff_out_w, f.eff_out_h, ngx::perf_quality_label(f.eff_perf));
		}
		else
		{
			snprintf(line1, sizeof(line1), "DLSS %s  no feature", dlss_version());
		}
		if (s.guest_mode)
			snprintf(line2, sizeof(line2), "Guest mode  external neural rendering");
		else if (ns.active)
		{
			const char *network = ns.pass[0].network[0] ? ns.pass[0].network : "?";
			snprintf(line2, sizeof(line2), "DLSS 5  %.40s  %u pass%s  %u x %u  %.1f ms", network, ns.passes, ns.passes == 1 ? "" : "es", ns.work_w, ns.work_h, ns.gpu_ms);
		}
		else if (s.nr.enabled)
		{
			snprintf(line2, sizeof(line2), "DLSS 5  %s", ns.failed ? ns.reason : "starting");
		}
		ImDrawList *dl = ImGui::GetForegroundDrawList(nullptr);
		if (dl == nullptr)
			return;
		const float line_h = ImGui::GetFontSize() + 2.0f;
		const ImVec2 display = ImGui::GetIO().DisplaySize;
		float y = display.y - 10.0f - line_h * (line2[0] ? 2.0f : 1.0f);
		const char *lines[2] = { line1, line2 };
		for (int i = 0; i < 2; ++i)
		{
			if (lines[i][0] == 0)
				continue;
			dl->AddText(ImVec2(11.0f, y + 1.0f), IM_COL32(0, 0, 0, 220), lines[i], nullptr);
			dl->AddText(ImVec2(10.0f, y), IM_COL32(120, 255, 120, 255), lines[i], nullptr);
			y += line_h;
		}
	}

	void draw_overlay(reshade::api::effect_runtime *)
	{
		forge::settings s = forge::get_settings();
		const forge::ngx_hook::stats st = forge::ngx_hook::get_stats();
		bool changed = false;

		if (forge::desktop::available())
			draw_desktop(s, changed);
		ImGui::SeparatorText("DLSS");

		changed |= ImGui::Checkbox("Forge active", &s.enabled);
		ImGui::SetItemTooltip("Off: the game DLSS calls go through without change.");
		ImGui::SameLine();
		if (!st.hooked)
			ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "NGX hook not installed. See ffxiv-dlss-forge.log.");
		else if (!st.feature.valid)
			ImGui::TextDisabled("waiting for DLSS. Select NVIDIA DLSS in Graphics Settings.");
		else
			ImGui::TextColored(ImVec4(0.55f, 0.9f, 0.55f, 1.0f), "active");

		ImGui::BeginDisabled(!s.enabled);
		changed |= ImGui::Checkbox("Guest mode", &s.guest_mode);
		ImGui::SetItemTooltip("Delegate neural rendering to another NGX-aware add-on. Forge keeps the DLSS/DLAA, resolution, preset and exposure controls.");

		ImGui::SeparatorText("Render resolution");
		int lock = static_cast<int>(s.lock);
		changed |= ImGui::RadioButton("DLAA (render = output)", &lock, 1);
		changed |= ImGui::RadioButton("DLSS upscaling", &lock, 2);
		ImGui::SameLine();
		ImGui::SetNextItemWidth(200.0f);
		int level = -1;
		for (int i = 0; i < 4; ++i)
			if (std::fabs(levels[i].scale - s.render_scale) < 0.05f)
				level = i;
		if (ImGui::Combo("##level", &level, [](void *, int i) { return levels[i].label; }, nullptr, 4, -1))
		{
			lock = 2;
			s.render_scale = levels[level].scale;
			changed = true;
		}
		ImGui::SameLine();
		ImGui::SetNextItemWidth(160.0f);
		if (ImGui::SliderFloat("##scale", &s.render_scale, 33.4f, 100.0f, "%.1f %%", ImGuiSliderFlags_AlwaysClamp))
		{
			lock = 2;
			changed = true;
		}
		ImGui::SetItemTooltip("Render size as a percentage of the output size.");
		changed |= ImGui::RadioButton("Game decides (in-game scale + dynamic resolution)", &lock, 0);
		s.lock = static_cast<forge::render_lock>(lock);
		if (st.feature.valid && (st.render_w != st.feature.eff_out_w || st.render_h != st.feature.eff_out_h))
			ImGui::TextDisabled("Scene resolution  %u x %u", st.render_w, st.render_h);

		ImGui::SeparatorText("DLSS model");
		int preset_index = 0;
		for (int i = 0; i < static_cast<int>(sizeof(presets) / sizeof(presets[0])); ++i)
			if (presets[i].value == s.preset)
				preset_index = i;
		ImGui::SetNextItemWidth(420.0f);
		if (ImGui::Combo("Preset", &preset_index, [](void *, int i) { return presets[i].label; }, nullptr, static_cast<int>(sizeof(presets) / sizeof(presets[0])), -1))
		{
			s.preset = presets[preset_index].value;
			changed = true;
		}
		int exposure = static_cast<int>(s.exposure);
		ImGui::SetNextItemWidth(420.0f);
		if (ImGui::Combo("Exposure", &exposure, "As the game sets it\0DLSS auto-exposure on\0DLSS auto-exposure off\0", -1))
		{
			s.exposure = static_cast<forge::exposure_mode>(exposure);
			changed = true;
		}

		changed |= ImGui::Checkbox("DLSS on-screen indicator", &s.indicator);
		ImGui::SetItemTooltip("Shows DLSS and DLSS 5 data at the bottom left, after the effects. The NVIDIA indicator stays off.");

		ImGui::EndDisabled();

		if (s.guest_mode)
			ImGui::TextDisabled("Guest mode: neural rendering is handled by the external add-on.");
		ImGui::BeginDisabled(!s.enabled || s.guest_mode);
		draw_nr(s.nr, changed);
		ImGui::EndDisabled();
		ImGui::Spacing();
		if (changed)
		{
			const forge::settings before = forge::get_settings();
			forge::set_settings(s);
			save_settings(forge::get_settings());
			if (before.lock != s.lock || before.render_scale != s.render_scale || before.enabled != s.enabled)
				forge::desktop::resetup();
		}

		if (ImGui::TreeNode("Diagnostics"))
		{
			const auto ds = forge::desktop::get();
			if (ds.attached)
			{
				ImGui::Text("Desktop %u x %u, window %u x %u, buffer %u x %u", ds.current.w, ds.current.h,
					ds.client_w, ds.client_h, ds.buffer_w, ds.buffer_h);
				if (ds.dsr_control)
					ImGui::Text("Wine: method %u, active %u, smoothness %u%%, image %u x %u -> %u x %u, generation %u/%u, result %d",
						ds.dsr.method, ds.dsr.active_method, ds.dsr.active_smoothness, ds.dsr.source_width, ds.dsr.source_height,
						ds.dsr.target_width, ds.dsr.target_height, ds.dsr.applied_generation, ds.dsr.generation, ds.dsr.result);
			}
			const auto &f = st.feature;
			char buf[160];
			if (f.valid)
			{
				ImGui::Text("Render now     %u x %u  (%.1f %% of output)", st.render_w, st.render_h, f.game_out_w ? 100.0 * st.render_w / f.game_out_w : 0.0);
				float lo = 1000.0f, hi = 0.0f;
				for (int i = 0; i < st.history_count; ++i)
				{
					const float v = st.history[(st.history_pos - 1 - i + forge::ngx_hook::history_size) % forge::ngx_hook::history_size];
					lo = v < lo ? v : lo;
					hi = v > hi ? v : hi;
				}
				snprintf(buf, sizeof(buf), "render scale, last %d evaluations: %.1f .. %.1f %%  (%llu size changes)", st.history_count, lo, hi, static_cast<unsigned long long>(st.render_size_changes));
				ImGui::PlotLines("##hist", st.history, forge::ngx_hook::history_size, st.history_pos, buf, 0.0f, 105.0f, ImVec2(0.0f, 60.0f), sizeof(float));

				ImGui::Text("Pipeline       %u x %u  ->  DLSS %u x %u", f.game_in_w, f.game_in_h, f.eff_out_w, f.eff_out_h);
				preset_text(buf, sizeof(buf), f.eff_presets);
				const double ratio = st.render_w != 0 ? static_cast<double>(f.eff_out_w) / st.render_w : 0.0;
				ImGui::Text("Running        %s (%.2f:1), %s", ngx::perf_quality_label(f.eff_perf), ratio, buf);
				flags_text(buf, sizeof(buf), f.eff_flags);
				ImGui::Text("Flags          %s", buf);
				preset_text(buf, sizeof(buf), f.game_presets);
				ImGui::TextDisabled("Game asked for %u x %u -> %u x %u, %s, %s", f.game_in_w, f.game_in_h, f.game_out_w, f.game_out_h, ngx::perf_quality_label(f.game_perf), buf);
				flags_text(buf, sizeof(buf), f.game_flags);
				ImGui::TextDisabled("Game flags     %s%s", buf, f.game_output_subrects ? ", output subrects" : "");
				ImGui::TextDisabled("Feature        %s, re-creates %llu", f.using_own_feature ? "forge-created" : "game-created",
					static_cast<unsigned long long>(f.recreates));
			}
			if (st.optimal_calls != 0)
				ImGui::TextDisabled("Size query     %u x %u %s: NGX %u x %u (dynamic %u..%u x %u..%u) -> %s %u x %u",
					st.opt_target_w, st.opt_target_h, ngx::perf_quality_label(st.opt_perf), st.opt_ngx_w, st.opt_ngx_h,
					st.opt_ngx_min_w, st.opt_ngx_max_w, st.opt_ngx_min_h, st.opt_ngx_max_h,
					st.opt_overridden ? "locked to" : "game uses", st.opt_ret_w, st.opt_ret_h);
			ImGui::TextDisabled("Evaluations    %llu (resets %llu), jitter %.3f %.3f, last 0x%08X",
				static_cast<unsigned long long>(st.evaluations), static_cast<unsigned long long>(st.resets),
				st.jitter_x, st.jitter_y, st.last_result);
			ImGui::TextDisabled("NGX core       %s (%u entry points routed%s)", st.core_path[0] ? st.core_path : "not loaded yet", st.resolved, st.callback_wrapped ? ", size query wrapped" : "");
			if (st.last_error[0] != '\0')
				ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.35f, 1.0f), "Last issue: %s", st.last_error);

			ImGui::Spacing();
			if (ImGui::TreeNode("Debug"))
			{
				forge::settings d = forge::get_settings();
				bool debug_changed = ImGui::Checkbox("Motion capture", &d.motion_capture);
				ImGui::SetItemTooltip("Records FFXIV's NGX parameters, motion measurements, reprojection errors and image samples into forge-captures. Copies use extra GPU memory while enabled. Nothing runs while it is off. This switch lasts for this session.");
				ImGui::SetNextItemWidth(260.0f);
				debug_changed |= ImGui::Combo("Capture test", &d.motion_capture_test,
					"Static scene\0Camera pan\0Moving character\0Resolution change\0DLSS upscaling\0NR comparison\0ReShade comparison\0Other\0");
				if (debug_changed)
					forge::set_settings(d);
				const auto capture = forge::motion_diagnostics::get_status();
				if (capture.folder[0] != 0) ImGui::TextWrapped("Capture: %s", capture.folder);
				if (capture.active || capture.frames || capture.surfaces)
					ImGui::Text("Frames %llu, measured %llu, surfaces %llu, dropped %llu, copies %.0f MiB", static_cast<unsigned long long>(capture.frames),
						static_cast<unsigned long long>(capture.measured), static_cast<unsigned long long>(capture.surfaces), static_cast<unsigned long long>(capture.dropped), capture.memory_mib);
				if (capture.issue[0] != 0) ImGui::TextWrapped("Capture: %s", capture.issue);
				ImGui::TreePop();
			}
			ImGui::TreePop();
		}
	}

	// ReShade docks add-on windows only into a fresh layout; with a saved layout, join the Home tab's dock node.
	void dock_into_reshade_tabs()
	{
		size_t size = 0;
		reshade::get_config_value(nullptr, "OVERLAY", "Window", nullptr, &size);
		if (size == 0)
			return;
		std::string data(size, '\0');
		reshade::get_config_value(nullptr, "OVERLAY", "Window", data.data(), &size);
		std::vector<std::string> lines;
		for (size_t i = 0, start = 0; i <= data.size(); ++i)
			if (i == data.size() || data[i] == '\0')
			{
				if (i > start)
					lines.emplace_back(data, start, i - start);
				start = i + 1;
			}

		std::string home_dock;
		bool in_home = false, in_forge = false, forge_docked = false;
		std::vector<std::string> kept;
		int siblings = 0;
		for (const std::string &l : lines)
		{
			if (!l.empty() && l[0] == '[')
			{
				in_home = l == "[Window][###home]";
				in_forge = l == "[Window][DLSS Forge]";
			}
			else if (in_home && l.rfind("DockId=", 0) == 0)
			{
				home_dock = l.substr(7, l.find(',') == std::string::npos ? std::string::npos : l.find(',') - 7);
			}
			else if (in_forge && l.rfind("DockId=", 0) == 0)
			{
				forge_docked = true;
			}
			if (!in_forge)
				kept.push_back(l);
		}
		if (forge_docked || home_dock.empty())
			return; // docked somewhere already (the user's choice), or no tab bar to join
		lines.swap(kept);
		for (const std::string &l : lines)
			if (l.rfind("DockId=" + home_dock + ",", 0) == 0)
				siblings++;

		lines.emplace_back("[Window][DLSS Forge]");
		lines.emplace_back("Collapsed=0");
		lines.emplace_back("DockId=" + home_dock + "," + std::to_string(siblings));
		std::string out;
		for (const std::string &l : lines)
		{
			out += l;
			out += '\0';
		}
		reshade::set_config_value(nullptr, "OVERLAY", "Window", out.data(), out.size());
		FORGE_LOG("docked the DLSS Forge window into ReShade's tab bar (dock %s)", home_dock.c_str());
	}

	void control_paths(HMODULE module);

	void init(HMODULE module)
	{
		wchar_t path[MAX_PATH] = {};
		GetModuleFileNameW(module, path, MAX_PATH);
		wchar_t *dot = wcsrchr(path, L'.');
		if (dot != nullptr)
			wcscpy(dot, L".log");
		forge::log::open(path);
		FORGE_LOG("FFXIV DLSS Forge (built " __DATE__ " " __TIME__ ")");
		control_paths(module);

		forge::ngx_hook::claim_indicator();
		load_settings();
		dock_into_reshade_tabs();
		const forge::settings s = forge::get_settings();
		forge::ngx_hook::set_indicator_registry(false); // NVIDIA's indicator stays off; ours is drawn after the effects
		FORGE_LOG("settings: active=%d guest=%d lock=%d scale=%.1f preset=%c exposure=%d desktop=%ux%u indicator=%d",
			s.enabled, s.guest_mode, static_cast<int>(s.lock), s.render_scale, ngx::preset_letter(s.preset), static_cast<int>(s.exposure),
			s.desktop_w, s.desktop_h, s.indicator);
		const forge::nr::tuning t = s.nr.resolve(0);
		FORGE_LOG("DLSS 5: on=%d stage=%d temporal=%d passes=%u network=%u model=%c intensity=%.2f structure=%.2f tone=%.2f automask=%d character=%.2f resolution=%.0f%% mask=%d blend=%d",
			s.nr.enabled, static_cast<int>(s.nr.selected_stage()), s.nr.temporal, s.nr.pass_count(), t.preset, 'A' + static_cast<char>(t.style),
			t.intensity, t.structure, t.tone, t.auto_mask, t.skin, s.nr.work_scale * 100.0f, s.nr.mask.enabled, s.nr.mask.blend);

		forge::ngx_hook::install(GetModuleHandleW(nullptr));
	}

	void on_init_swapchain(reshade::api::swapchain *swapchain, bool)
	{
		if (swapchain->get_device()->get_api() == reshade::api::device_api::d3d11)
			forge::desktop::attach(static_cast<HWND>(swapchain->get_hwnd()));
	}

	// After ReShade's effects and overlay. ReShade (MSVC) returns structures through a hidden pointer and this
	// GNU-ABI build expects them in RAX, so ReShade methods returning api:: structures are never called; the
	// back buffer comes from the native swapchain.
	uint64_t g_bound_motion = UINT64_MAX; // generation of the motion vector views ReShade binds; a new runtime binds again

	void on_init_effect_runtime(reshade::api::effect_runtime *)
	{
		g_bound_motion = UINT64_MAX;
	}

	// ReShade keeps semantic bindings across effect reloads; they change only when the game's vector texture changes
	void bind_motion(reshade::api::effect_runtime *runtime)
	{
		const forge::motion::views v = forge::motion::get();
		if (v.generation == g_bound_motion)
			return;
		runtime->update_texture_bindings(forge::motion::semantic_vectors, reshade::api::resource_view { v.vectors }, reshade::api::resource_view { v.vectors });
		runtime->update_texture_bindings(forge::motion::semantic_info, reshade::api::resource_view { v.info }, reshade::api::resource_view { v.info });
		g_bound_motion = v.generation;
		forge::motion::bound(v.generation);
	}

	void on_present(reshade::api::command_queue *queue, reshade::api::swapchain *swapchain,
		const reshade::api::rect *, const reshade::api::rect *, uint32_t, const reshade::api::rect *)
	{
		if (queue->get_device()->get_api() != reshade::api::device_api::d3d11) return;
		auto *native = reinterpret_cast<IDXGISwapChain *>(swapchain->get_native());
		ID3D11Texture2D *image = nullptr;
		if (native != nullptr && SUCCEEDED(native->GetBuffer(swapchain->get_current_back_buffer_index(),
			__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&image))))
		{
			forge::ngx_hook::present_nr(reinterpret_cast<ID3D11DeviceContext *>(queue->get_native()), image,
				swapchain->get_color_space() == reshade::api::color_space::scrgb, static_cast<unsigned int>(swapchain->get_color_space()));
			image->Release();
		}
		else forge::ngx_hook::present_nr(reinterpret_cast<ID3D11DeviceContext *>(queue->get_native()), nullptr);
		forge::nr::end_frame();
	}

	void on_finish_effects(reshade::api::effect_runtime *, reshade::api::command_list *list,
		reshade::api::resource_view rtv, reshade::api::resource_view)
	{
		if (!forge::motion_diagnostics::active() || rtv.handle == 0 || list->get_device()->get_api() != reshade::api::device_api::d3d11) return;
		ID3D11Resource *image = nullptr;
		reinterpret_cast<ID3D11RenderTargetView *>(rtv.handle)->GetResource(&image);
		forge::ngx_hook::capture_after_effects(reinterpret_cast<ID3D11DeviceContext *>(list->get_native()), image);
		if (image) image->Release();
	}

	// Session control for automated test runs (tests/motion_suite.py): forge-control.txt beside the add-on holds
	// key=value lines, applied in order without writing ReShade.ini. Forge deletes the file and answers in
	// forge-control.ack with the sequence number. The first command keeps the settings and effect state for restore=1.
	struct control_state
	{
		std::wstring command, ack;
		ULONGLONG next_poll = 0;
		bool saved = false, effects = true;
		forge::settings original;
	} g_control;

	void control_paths(HMODULE module)
	{
		wchar_t path[MAX_PATH] = {};
		GetModuleFileNameW(module, path, MAX_PATH);
		wchar_t *slash = wcsrchr(path, L'\\');
		if (slash == nullptr)
			return;
		slash[1] = 0;
		g_control.command = std::wstring(path) + L"forge-control.txt";
		g_control.ack = std::wstring(path) + L"forge-control.ack";
	}

	void poll_control(reshade::api::effect_runtime *runtime)
	{
		const ULONGLONG now = GetTickCount64();
		if (g_control.command.empty() || now < g_control.next_poll)
			return;
		g_control.next_poll = now + 250;
		FILE *file = _wfopen(g_control.command.c_str(), L"rb");
		if (file == nullptr)
			return;
		std::string text;
		char chunk[512];
		for (size_t n; text.size() < 8192 && (n = fread(chunk, 1, sizeof(chunk), file)) > 0;)
			text.append(chunk, n);
		fclose(file);
		DeleteFileW(g_control.command.c_str());

		const forge::settings before = forge::get_settings();
		if (!g_control.saved)
		{
			g_control.saved = true;
			g_control.original = before;
			g_control.effects = runtime->get_effects_state();
		}
		forge::settings s = before;
		std::string seq = "0", issues;
		int effects = -1;
		bool close_overlay = false;
		for (size_t pos = 0; pos < text.size();)
		{
			size_t end = text.find('\n', pos);
			if (end == std::string::npos)
				end = text.size();
			std::string line = text.substr(pos, end - pos);
			pos = end + 1;
			while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
				line.pop_back();
			if (line.empty() || line[0] == '#')
				continue;
			const size_t eq = line.find('=');
			const std::string key = line.substr(0, eq), value = eq == std::string::npos ? std::string() : line.substr(eq + 1);
			const int v = atoi(value.c_str());
			if (key == "seq") seq = value;
			else if (key == "restore" && v != 0) { s = g_control.original; effects = g_control.effects ? 1 : 0; }
			else if (key == "dlss5") s.nr.enabled = v != 0;
			else if (key == "reset_dlss5") { const bool on = s.nr.enabled; s.nr.reset_defaults(); s.nr.enabled = on; }
			else if (key == "stage" && (value == "before" || value == "after" || value == "present"))
				s.nr.position = value == "before" ? forge::nr::stage::before_upscaling : value == "present" ? forge::nr::stage::present : forge::nr::stage::after_upscaling;
			else if (key == "temporal") s.nr.temporal = v != 0;
			else if (key == "passes" && v >= 1 && static_cast<unsigned int>(v) <= forge::nr::max_passes) { s.nr.passes = static_cast<unsigned int>(v); s.nr.unlock_passes |= s.nr.passes > forge::nr::default_pass_limit; }
			else if (key == "frame_log") s.nr.frame_log = v != 0;
			else if (key == "lock" && (value == "game" || value == "dlaa" || value == "upscaling"))
				s.lock = value == "game" ? forge::render_lock::game : value == "dlaa" ? forge::render_lock::native : forge::render_lock::fixed;
			else if (key == "scale") { const float f = static_cast<float>(atof(value.c_str())); if (f >= 33.4f && f <= 100.0f) s.render_scale = f; else issues += " scale"; }
			else if (key == "capture") s.motion_capture = v != 0;
			else if (key == "test" && v >= 0 && v <= 7) s.motion_capture_test = v;
			else if (key == "effects") effects = v != 0 ? 1 : 0;
			else if (key == "overlay") close_overlay = v == 0;
			else issues += " " + key;
		}
		forge::set_settings(s);
		if (before.lock != s.lock || before.render_scale != s.render_scale || before.enabled != s.enabled)
			forge::desktop::resetup();
		if (effects >= 0)
			runtime->set_effects_state(effects != 0);
		if (close_overlay)
			runtime->open_overlay(false, reshade::api::input_source::none);
		FORGE_LOG("control %s: lock=%d scale=%.1f DLSS5=%d stage=%d temporal=%d passes=%u capture=%d test=%d effects=%d%s%s", seq.c_str(),
			static_cast<int>(s.lock), s.render_scale, s.nr.enabled, static_cast<int>(s.nr.selected_stage()), s.nr.temporal, s.nr.pass_count(),
			s.motion_capture, s.motion_capture_test, effects, issues.empty() ? "" : " rejected:", issues.c_str());

		const std::wstring staged = g_control.ack + L".tmp";
		if (FILE *ack = _wfopen(staged.c_str(), L"wb"))
		{
			fprintf(ack, "seq=%s\nstatus=%s\n", seq.c_str(), issues.empty() ? "ok" : ("rejected" + issues).c_str());
			fclose(ack);
			MoveFileExW(staged.c_str(), g_control.ack.c_str(), MOVEFILE_REPLACE_EXISTING);
		}
	}

	void on_reshade_present(reshade::api::effect_runtime *runtime)
	{
		if (runtime->get_device()->get_api() != reshade::api::device_api::d3d11)
			return;
		poll_control(runtime);
		forge::motion::end_frame();
		bind_motion(runtime);
		auto *swapchain = reinterpret_cast<IDXGISwapChain *>(runtime->get_native());
		ID3D11Texture2D *back_buffer = nullptr;
		if (swapchain == nullptr || FAILED(swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&back_buffer))) || back_buffer == nullptr)
			return;
		D3D11_TEXTURE2D_DESC bd = {};
		back_buffer->GetDesc(&bd);
		back_buffer->Release(); // no reference outlives the present, so the game can resize its buffers
		forge::desktop::present(bd.Width, bd.Height);
	}

	void on_destroy_device(reshade::api::device *)
	{
		forge::desktop::release();
	}

	// ReShade raises these for the immediate context only; get_native is the context below ReShade's wrapper
	void on_init_command_queue(reshade::api::command_queue *queue)
	{
		if (queue->get_device()->get_api() == reshade::api::device_api::d3d11)
			forge::ngx_hook::set_native_context(reinterpret_cast<ID3D11DeviceContext *>(queue->get_native()));
	}

	// D3D11 shares one state object per description, so another add-on's pipeline can hold one of the game's. REST
	// releases its pipelines in destroy_device after freeing its tracking data; if that release is the last one,
	// ReShade's destruction callback reaches the freed data. One extra reference on each state object the game
	// creates prevents that; the references end with the process.
	SRWLOCK g_states_lock = SRWLOCK_INIT;
	std::unordered_set<uint64_t> g_held_states;

	void on_init_pipeline(reshade::api::device *device, reshade::api::pipeline_layout, uint32_t count,
		const reshade::api::pipeline_subobject *subobjects, reshade::api::pipeline pipeline)
	{
		if (pipeline.handle == 0 || device->get_api() != reshade::api::device_api::d3d11)
			return;
		bool state = false;
		for (uint32_t i = 0; i < count; ++i)
		{
			const reshade::api::pipeline_subobject_type t = subobjects[i].type;
			state |= t == reshade::api::pipeline_subobject_type::blend_state || t == reshade::api::pipeline_subobject_type::rasterizer_state ||
				t == reshade::api::pipeline_subobject_type::depth_stencil_state;
		}
		if (!state)
			return;
		AcquireSRWLockExclusive(&g_states_lock);
		const bool added = g_held_states.insert(pipeline.handle).second;
		ReleaseSRWLockExclusive(&g_states_lock);
		if (added)
			reinterpret_cast<IUnknown *>(pipeline.handle)->AddRef();
	}

	void on_destroy_command_queue(reshade::api::command_queue *queue)
	{
		if (queue->get_device()->get_api() != reshade::api::device_api::d3d11)
			return;
		forge::ngx_hook::clear_native_context(reinterpret_cast<ID3D11DeviceContext *>(queue->get_native()));
		forge::motion::release();
		AcquireSRWLockShared(&g_states_lock);
		const size_t held = g_held_states.size();
		ReleaseSRWLockShared(&g_states_lock);
		FORGE_LOG("device teardown: %zu game state objects kept until the process ends", held);
	}
}

// The game calls the NGX wrappers until it exits, also after ReShade unloads add-ons at device teardown, so
// the module is pinned and registration happens in AddonInit/AddonUninit on every ReShade load cycle.
extern "C" __declspec(dllexport) bool AddonInit(HMODULE module, HMODULE reshade_module)
{
	static bool once = false;
	if (!once)
	{
		HMODULE pinned = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&AddonInit), &pinned);
	}
	if (!reshade::register_addon(module, reshade_module))
		return false;
	g_registered = true;
	if (!once)
	{
		once = true;
		init(module);
	}
	reshade::register_event<reshade::addon_event::init_swapchain>(&on_init_swapchain);
	reshade::register_event<reshade::addon_event::init_effect_runtime>(&on_init_effect_runtime);
	reshade::register_event<reshade::addon_event::present>(&on_present);
	reshade::register_event<reshade::addon_event::reshade_finish_effects>(&on_finish_effects);
	reshade::register_event<reshade::addon_event::reshade_present>(&on_reshade_present);
	reshade::register_event<reshade::addon_event::destroy_device>(&on_destroy_device);
	reshade::register_event<reshade::addon_event::init_command_queue>(&on_init_command_queue);
	reshade::register_event<reshade::addon_event::destroy_command_queue>(&on_destroy_command_queue);
	reshade::register_event<reshade::addon_event::init_pipeline>(&on_init_pipeline);
	reshade::register_event<reshade::addon_event::reshade_overlay>(&on_reshade_overlay);
	reshade::register_overlay("DLSS Forge", &draw_overlay);
	return true;
}

extern "C" __declspec(dllexport) void AddonUninit(HMODULE module, HMODULE reshade_module)
{
	if (!g_registered)
		return;
	forge::ngx_hook::stop_motion_capture();
	reshade::unregister_overlay("DLSS Forge", &draw_overlay);
	reshade::unregister_event<reshade::addon_event::reshade_overlay>(&on_reshade_overlay);
	reshade::unregister_event<reshade::addon_event::init_pipeline>(&on_init_pipeline);
	reshade::unregister_event<reshade::addon_event::destroy_command_queue>(&on_destroy_command_queue);
	reshade::unregister_event<reshade::addon_event::init_command_queue>(&on_init_command_queue);
	reshade::unregister_event<reshade::addon_event::destroy_device>(&on_destroy_device);
	reshade::unregister_event<reshade::addon_event::present>(&on_present);
	reshade::unregister_event<reshade::addon_event::reshade_finish_effects>(&on_finish_effects);
	reshade::unregister_event<reshade::addon_event::reshade_present>(&on_reshade_present);
	reshade::unregister_event<reshade::addon_event::init_effect_runtime>(&on_init_effect_runtime);
	reshade::unregister_event<reshade::addon_event::init_swapchain>(&on_init_swapchain);
	forge::desktop::release();
	reshade::unregister_addon(module, reshade_module);
	g_registered = false;
	FORGE_LOG("ReShade unloaded the add-on; NGX routing stays active");
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_DETACH)
		forge::log::close();
	return TRUE;
}
