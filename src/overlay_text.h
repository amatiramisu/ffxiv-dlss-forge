#pragma once

#include <imgui.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace forge::overlay
{
	inline void align_text(ImDrawList &list, int first_vertex, const ImFontBaked &font,
		float font_size, float pixel_w, float pixel_h)
	{
		if (pixel_w <= 1.0f && pixel_h <= 1.0f)
			return;
		const auto key = [](float u, float v) {
			return (uint64_t(std::bit_cast<uint32_t>(u)) << 32) | std::bit_cast<uint32_t>(v);
		};
		std::unordered_map<uint64_t, const ImFontGlyph *> glyphs;
		glyphs.reserve(font.Glyphs.Size);
		for (const ImFontGlyph &g : font.Glyphs)
			if (g.Visible)
				glyphs.emplace(key(g.U0, g.V0), &g);
		const float scale = font_size / font.Size;
		for (int i = first_vertex; i + 3 < list.VtxBuffer.Size; ++i)
		{
			ImDrawVert *v = list.VtxBuffer.Data + i;
			const auto found = glyphs.find(key(v[0].uv.x, v[0].uv.y));
			if (found == glyphs.end())
				continue;
			const ImFontGlyph &g = *found->second;
			if (v[1].uv.x != g.U1 || v[1].uv.y != g.V0 ||
				v[2].uv.x != g.U1 || v[2].uv.y != g.V1 ||
				v[3].uv.x != g.U0 || v[3].uv.y != g.V1 ||
				v[0].pos.y != v[1].pos.y || v[1].pos.x != v[2].pos.x ||
				v[2].pos.y != v[3].pos.y || v[3].pos.x != v[0].pos.x)
				continue;
			const float x = v[0].pos.x - g.X0 * scale;
			const float y = v[0].pos.y - g.Y0 * scale;
			const float dx = pixel_w > 1.0f ? std::round(x / pixel_w) * pixel_w - x : 0.0f;
			const float dy = pixel_h > 1.0f ? std::round(y / pixel_h) * pixel_h - y : 0.0f;
			for (int j = 0; j < 4; ++j)
			{
				v[j].pos.x += dx;
				v[j].pos.y += dy;
			}
			i += 3;
		}
	}
}
