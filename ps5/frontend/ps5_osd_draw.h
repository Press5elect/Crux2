// Crux2 - the on-screen display's panel, drawn: header-only, ImGui alone, for ps5_osd.cpp and the
// interface's PC preview (ps5/tests/bigpicture), which pictures it with made-up figures.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

#pragma once

#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace PS5OsdDraw
{
	// The colours of the sections' names; the numbers of the first two are orange, the frames'
	// white, as the PC overlays the panel is drawn after have them.
	constexpr ImU32 GPU_NAME = IM_COL32(112, 214, 84, 255);
	constexpr ImU32 CPU_NAME = IM_COL32(86, 172, 240, 255);
	constexpr ImU32 FRAME_NAME = IM_COL32(244, 214, 192, 255);
	constexpr ImU32 HOT_VALUE = IM_COL32(255, 158, 64, 255);
	constexpr ImU32 COOL_VALUE = IM_COL32(255, 255, 255, 255);
	constexpr ImU32 SLOW_VALUE = IM_COL32(255, 96, 86, 255);

	struct Row
	{
		std::string name;
		std::string value;
		const char* unit = "";
		ImU32 name_colour = FRAME_NAME;
		ImU32 value_colour = COOL_VALUE;
	};

	struct Panel
	{
		std::vector<Row> rows;
		std::vector<size_t> sections; // the rows a section starts at (a gap above each)
		bool graph = false;
		const float* history = nullptr; // frame times in ms, a ring
		uint32_t history_size = 0;
		uint32_t history_pos = 0; // the oldest
	};

	/// place: PCSX2's OsdOverlayPos (1 top left ... 9 bottom right). Returns the y under the panel.
	inline float Draw(ImDrawList* dl, ImFont* font, float size, const Panel& p, int place, float scale, float margin, float position_y,
		float screen_w, float screen_h)
	{
		if (p.rows.empty() && !p.graph)
			return position_y;
		const float unit_size = std::round(size * 0.48f);
		const float line = std::round(size * 1.02f);
		const float gap = std::round(size * 0.6f);
		const float pad = std::round(10.0f * scale);
		const float shadow = std::max(1.0f, std::round(scale * 1.5f));
		const float col_gap = std::round(size * 0.6f);
		const float unit_gap = std::round(size * 0.15f);

		// The columns: the widest name, the widest number, the widest unit.
		float name_w = 0, value_w = 0, unit_w = 0;
		for (const Row& r : p.rows)
		{
			name_w = std::max(name_w, font->CalcTextSizeA(size, FLT_MAX, 0, r.name.c_str()).x);
			value_w = std::max(value_w, font->CalcTextSizeA(size, FLT_MAX, 0, r.value.c_str()).x);
			unit_w = std::max(unit_w, font->CalcTextSizeA(unit_size, FLT_MAX, 0, r.unit).x);
		}
		const float graph_h = p.graph ? std::round(size * 2.0f) : 0.0f;
		const float inner_w = std::max(name_w + col_gap + value_w + unit_gap + unit_w, std::round(size * 7.0f));
		float inner_h = static_cast<float>(p.rows.size()) * line + static_cast<float>(p.sections.size()) * gap;
		if (p.graph)
			inner_h += (p.rows.empty() ? 0.0f : gap * 0.5f) + graph_h;
		const float w = inner_w + 2 * pad, h = inner_h + 2 * pad;

		// Where the place says.
		float x = margin, y = position_y;
		if (place == 2 || place == 5 || place == 8)
			x = std::round((screen_w - w) * 0.5f);
		else if (place == 3 || place == 6 || place == 9)
			x = screen_w - margin - w;
		if (place >= 4 && place <= 6)
			y = std::round((screen_h - h) * 0.5f);
		else if (place >= 7)
			y = screen_h - margin - h;

		dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), IM_COL32(8, 10, 16, 120), std::round(8.0f * scale));
		const auto text = [&](float tx, float ty, float tsize, ImU32 colour, const char* s) {
			dl->AddText(font, tsize, ImVec2(tx + shadow, ty + shadow), IM_COL32(0, 0, 0, 150), s);
			dl->AddText(font, tsize, ImVec2(tx, ty), colour, s);
		};

		float cy = y + pad;
		size_t next = 0;
		const float value_right = x + pad + name_w + col_gap + value_w;
		for (size_t i = 0; i < p.rows.size(); i++)
		{
			if (next < p.sections.size() && p.sections[next] == i)
			{
				cy += gap;
				next++;
			}
			const Row& r = p.rows[i];
			text(x + pad, cy, size, r.name_colour, r.name.c_str());
			const float vw = font->CalcTextSizeA(size, FLT_MAX, 0, r.value.c_str()).x;
			text(value_right - vw, cy, size, r.value_colour, r.value.c_str());
			if (r.unit && r.unit[0])
				text(value_right + unit_gap, cy + std::round(size * 0.08f), unit_size, r.value_colour, r.unit);
			cy += line;
		}

		// The frame times, the newest at the right: each a bar as tall as the frame was long.
		if (p.graph && p.history && p.history_size > 0)
		{
			if (!p.rows.empty())
				cy += gap * 0.5f;
			float top = 1000.0f / 30.0f; // a 33 ms frame fills it; a longer one is cut
			for (uint32_t k = 0; k < p.history_size; k++)
				top = std::max(top, std::min(p.history[k], 100.0f));
			const float gx = x + pad, gw = inner_w, step = gw / static_cast<float>(p.history_size);
			const float base = cy + graph_h;
			dl->AddLine(ImVec2(gx, base), ImVec2(gx + gw, base), IM_COL32(255, 255, 255, 40), 1.0f);
			const float l60 = base - graph_h * std::min(1.0f, (1000.0f / 60.0f) / top);
			dl->AddLine(ImVec2(gx, l60), ImVec2(gx + gw, l60), IM_COL32(255, 255, 255, 34), 1.0f);
			for (uint32_t k = 0; k < p.history_size; k++)
			{
				const float t = p.history[(p.history_pos + k) % p.history_size];
				if (t <= 0.01f)
					continue;
				const float bh = std::max(1.0f, graph_h * std::min(1.0f, t / top));
				const float bx = gx + step * static_cast<float>(k);
				dl->AddRectFilled(ImVec2(bx, base - bh), ImVec2(bx + std::max(1.0f, step * 0.85f), base), IM_COL32(244, 196, 176, 235));
			}
		}
		return y + h;
	}
} // namespace PS5OsdDraw
