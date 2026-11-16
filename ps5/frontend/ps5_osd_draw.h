// Crux2 - the performance overlay, drawn: header-only, ImGui alone, for ps5_osd.cpp and the
// interface's PC preview (ps5/tests/bigpicture), which pictures it with made-up figures.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Three sizes and Custom ([PS5] OsdSize, ps5_osd_logic.h):
//   Minimal  a small pill: "60 FPS"
//   Compact  one rounded bar: FPS, GPU, EE, GS, VU, each a small grey label, a bold number, a unit
//   Full     a card 280 wide: the FPS large, the frame time and speed, the 1% and 0.1% lows, a
//            frame-time graph, GPU/EE/GS/VU with thin bars, a quiet footer (resolution, temperature,
//            memory)
//   Custom   Full's card with what the switches give (and the details Full leaves out)
// On a dark translucent ground, text in warm white, labels in grey, digits all of one width so that
// nothing moves as they change. Colour only for meaning: amber close to a limit, red over it.
// Sizes are in design pixels times `u` (the OSD scale).

#pragma once

#include "imgui.h"
#include "ps5_osd_logic.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace PS5OsdDraw
{
	using PS5OsdLogic::Level;

	constexpr ImU32 GROUND = IM_COL32(12, 11, 10, 204);   // rgba(12, 11, 10, 0.80)
	constexpr ImU32 EDGE = IM_COL32(255, 255, 255, 18);   // a hairline round the ground
	constexpr ImU32 TEXT = IM_COL32(243, 236, 228, 255);  // #f3ece4
	constexpr ImU32 LABEL = IM_COL32(185, 176, 165, 255); // #b9b0a5
	constexpr ImU32 AMBER = IM_COL32(255, 180, 84, 255);  // #ffb454
	constexpr ImU32 RED = IM_COL32(255, 122, 107, 255);   // #ff7a6b
	constexpr ImU32 TRACK = IM_COL32(255, 255, 255, 30);  // a bar's empty part
	constexpr ImU32 GRAPH_BAR = IM_COL32(185, 176, 165, 150);
	constexpr ImU32 RULE = IM_COL32(255, 255, 255, 26);   // the graph's lines, the footer's rule

	constexpr ImU32 Colour(Level level)
	{
		return level == Level::Over ? RED : level == Level::Near ? AMBER : TEXT;
	}

	struct Fonts
	{
		ImFont* regular = nullptr; // labels and units
		ImFont* bold = nullptr;    // numbers
	};

	/// GPU, EE, GS, VU...: a label, a number (and a bar in the card).
	struct Meter
	{
		std::string label;
		float percent = 0.0f; // the bar, 0..100
		std::string value;    // "72"
		const char* unit = "%";
		Level level = Level::Fine;
	};

	/// A detail in Custom's card: a label and a value.
	struct Stat
	{
		std::string label, value;
		const char* unit = "";
		Level level = Level::Fine;
	};

	struct Overlay
	{
		PS5OsdLogic::Size size = PS5OsdLogic::Compact;
		bool fps = false;
		float game_fps = 0.0f;
		float screen_fps = 0.0f; // frame generation's pictures a second on the screen (0: not running)
		Level fps_level = Level::Fine;

		bool frame_ms = false; // the last frame's time, the lows
		float last_ms = 0.0f, low1_fps = 0.0f, low01_fps = 0.0f;
		Level frame_level = Level::Fine;
		bool speed = false;
		float speed_percent = 0.0f;
		Level speed_level = Level::Fine;
		bool vps = false;
		float vps_value = 0.0f;

		bool graph = false;
		const float* history = nullptr; // frame times in ms, a ring
		uint32_t history_size = 0;
		uint32_t history_pos = 0; // the oldest
		float budget_ms = 1000.0f / 60.0f;

		std::vector<Meter> meters;
		std::vector<Stat> stats;          // Custom's details, two to a line
		std::string footer_left, footer_right; // "3840x2688 · 6x", "61 °C · 7.4 GB"
		std::string version;              // under the footer

		bool Empty() const
		{
			return !fps && !frame_ms && !speed && !vps && !graph && meters.empty() && stats.empty() && footer_left.empty() &&
				   footer_right.empty() && version.empty();
		}
	};

	// ---- text with digits of one width

	inline float DigitWidth(ImFont* font, float size)
	{
		float w = 0.0f;
		char d[2] = {'0', 0};
		for (char c = '0'; c <= '9'; c++)
		{
			d[0] = c;
			w = std::max(w, font->CalcTextSizeA(size, FLT_MAX, 0, d).x);
		}
		return w;
	}

	/// Draws (dl non-null) or measures s, each digit in a cell of the widest digit's width. Returns the width.
	inline float Tab(ImDrawList* dl, ImFont* font, float size, float x, float y, ImU32 colour, const std::string& s)
	{
		const float dw = DigitWidth(font, size);
		float cx = x;
		size_t i = 0;
		while (i < s.size())
		{
			if (s[i] >= '0' && s[i] <= '9')
			{
				const char d[2] = {s[i], 0};
				const float w = font->CalcTextSizeA(size, FLT_MAX, 0, d).x;
				if (dl)
					dl->AddText(font, size, ImVec2(std::round(cx + (dw - w) * 0.5f), y), colour, d);
				cx += dw;
				i++;
				continue;
			}
			size_t j = i;
			while (j < s.size() && !(s[j] >= '0' && s[j] <= '9'))
				j++;
			const char* b = s.data() + i;
			const char* e = s.data() + j;
			if (dl)
				dl->AddText(font, size, ImVec2(std::round(cx), y), colour, b, e);
			cx += font->CalcTextSizeA(size, FLT_MAX, 0, b, e).x;
			i = j;
		}
		return cx - x;
	}

	inline float TabWidth(ImFont* font, float size, const std::string& s)
	{
		return Tab(nullptr, font, size, 0, 0, 0, s);
	}

	inline float Width(ImFont* font, float size, const char* s)
	{
		return (s && s[0]) ? font->CalcTextSizeA(size, FLT_MAX, 0, s).x : 0.0f;
	}

	/// Pieces of a line, each its own face, size and colour (numbers drawn with Tab).
	struct Piece
	{
		std::string s;
		ImFont* font;
		float size;
		ImU32 colour;
		float rise = 0.0f; // its top below the line's top
	};

	inline float PiecesWidth(const std::vector<Piece>& pieces)
	{
		float w = 0.0f;
		for (const Piece& p : pieces)
			w += TabWidth(p.font, p.size, p.s);
		return w;
	}

	inline void DrawPieces(ImDrawList* dl, const std::vector<Piece>& pieces, float x, float y)
	{
		for (const Piece& p : pieces)
			x += Tab(dl, p.font, p.size, x, y + p.rise, p.colour, p.s);
	}

	/// The arrow between the game's rate and the screen's: drawn, not a glyph (the faces have none).
	inline float ArrowWidth(float size)
	{
		return std::round(size * 0.9f);
	}
	inline void Arrow(ImDrawList* dl, float x, float mid_y, float size, ImU32 colour)
	{
		const float w = ArrowWidth(size), pad = std::round(size * 0.18f);
		const float x0 = x + pad, x1 = x + w - pad, head = std::max(2.0f, std::round(size * 0.2f));
		const float thick = std::max(1.0f, size * 0.07f);
		dl->AddLine(ImVec2(x0, mid_y), ImVec2(x1 - thick, mid_y), colour, thick);
		dl->AddTriangleFilled(ImVec2(x1, mid_y), ImVec2(x1 - head * 1.3f, mid_y - head), ImVec2(x1 - head * 1.3f, mid_y + head), colour);
	}

	inline std::string Whole(float v)
	{
		char out[32];
		std::snprintf(out, sizeof(out), "%.0f", static_cast<double>(std::max(0.0f, v)));
		return out;
	}

	/// The FPS figure: "60", or with frame generation "30", an arrow, "60" (the game's rate, then the
	/// screen's). Its width; drawn when dl is set.
	inline float Rates(ImDrawList* dl, ImFont* font, float size, float x, float y, const Overlay& o, ImU32 colour)
	{
		const std::string game = Whole(o.game_fps);
		const bool two = PS5OsdLogic::ShowsTwoRates(o.game_fps, o.screen_fps);
		float w = Tab(dl, font, size, x, y, colour, game);
		if (!two)
			return w;
		if (dl)
			Arrow(dl, x + w, y + std::round(size * 0.55f), size, LABEL);
		w += ArrowWidth(size);
		w += Tab(dl, font, size, x + w, y, colour, Whole(o.screen_fps));
		return w;
	}

	/// Where a box of w x h goes for PCSX2's OsdOverlayPos (1 top left ... 9 bottom right).
	inline ImVec2 Place(int place, float w, float h, float margin, float position_y, float screen_w, float screen_h)
	{
		float x = margin, y = position_y;
		if (place == 2 || place == 5 || place == 8)
			x = std::round((screen_w - w) * 0.5f);
		else if (place == 3 || place == 6 || place == 9)
			x = screen_w - margin - w;
		if (place >= 4 && place <= 6)
			y = std::round((screen_h - h) * 0.5f);
		else if (place >= 7)
			y = screen_h - margin - h;
		return ImVec2(x, y);
	}

	inline void Ground(ImDrawList* dl, ImVec2 a, ImVec2 b, float radius)
	{
		dl->AddRectFilled(a, b, GROUND, radius);
		dl->AddRect(a, b, EDGE, radius, 1.0f);
	}

	// ---- Minimal: the pill

	inline float DrawMinimal(ImDrawList* dl, const Fonts& f, float u, const Overlay& o, int place, float margin, float position_y, float sw,
		float sh)
	{
		const float num = std::round(17.0f * u), small = std::round(12.0f * u);
		const float pad_x = std::round(14.0f * u), h = std::round(30.0f * u), gap = std::round(5.0f * u);
		const float nw = Rates(nullptr, f.bold, num, 0, 0, o, TEXT);
		const float w = pad_x * 2 + nw + gap + Width(f.regular, small, "FPS");
		const ImVec2 at = Place(place, w, h, margin, position_y, sw, sh);
		Ground(dl, at, ImVec2(at.x + w, at.y + h), h * 0.5f);
		const float ny = at.y + std::round((h - num) * 0.5f);
		Rates(dl, f.bold, num, at.x + pad_x, ny, o, Colour(o.fps_level));
		dl->AddText(f.regular, small, ImVec2(at.x + pad_x + nw + gap, ny + std::round((num - small) * 0.78f)), LABEL, "FPS");
		return at.y + h;
	}

	// ---- Compact: the bar

	inline float DrawCompact(ImDrawList* dl, const Fonts& f, float u, const Overlay& o, int place, float margin, float position_y, float sw,
		float sh)
	{
		const float num = std::round(16.0f * u), small = std::round(12.0f * u);
		const float pad_x = std::round(16.0f * u), h = std::round(34.0f * u), item_gap = std::round(16.0f * u);
		const float label_gap = std::round(5.0f * u), unit_gap = std::round(2.0f * u);
		const float digit = DigitWidth(f.bold, num);
		struct Item
		{
			std::string label, value;
			const char* unit;
			Level level;
			bool rates;
			float value_w;
		};
		std::vector<Item> items;
		if (o.fps)
			items.push_back({"", "", "FPS", o.fps_level, true, Rates(nullptr, f.bold, num, 0, 0, o, TEXT)});
		for (const Meter& m : o.meters)
			items.push_back({m.label, m.value, m.unit, m.level, false, std::max(TabWidth(f.bold, num, m.value), digit * 3.0f)});
		float w = pad_x * 2;
		for (size_t i = 0; i < items.size(); i++)
		{
			const Item& it = items[i];
			w += (i ? item_gap : 0.0f) + (it.label.empty() ? 0.0f : Width(f.regular, small, it.label.c_str()) + label_gap) + it.value_w +
				 (it.rates ? label_gap : unit_gap) + Width(f.regular, small, it.unit);
		}
		const ImVec2 at = Place(place, w, h, margin, position_y, sw, sh);
		Ground(dl, at, ImVec2(at.x + w, at.y + h), h * 0.5f);
		const float ny = at.y + std::round((h - num) * 0.5f);
		const float sy = ny + std::round((num - small) * 0.78f);
		float x = at.x + pad_x;
		for (size_t i = 0; i < items.size(); i++)
		{
			const Item& it = items[i];
			if (i)
			{
				x += item_gap;
				const float rx = std::round(x - item_gap * 0.5f);
				dl->AddLine(ImVec2(rx, at.y + h * 0.3f), ImVec2(rx, at.y + h * 0.7f), RULE, 1.0f);
			}
			if (!it.label.empty())
			{
				dl->AddText(f.regular, small, ImVec2(x, sy), LABEL, it.label.c_str());
				x += Width(f.regular, small, it.label.c_str()) + label_gap;
			}
			// The number beside its label and the unit beside the number; the room kept for a third
			// digit after them, so that the bar keeps its width.
			const float used = it.rates ? it.value_w : TabWidth(f.bold, num, it.value);
			if (it.rates)
				Rates(dl, f.bold, num, x, ny, o, Colour(it.level));
			else
				Tab(dl, f.bold, num, x, ny, Colour(it.level), it.value);
			dl->AddText(f.regular, small, ImVec2(x + used + (it.rates ? label_gap : unit_gap), sy), LABEL, it.unit);
			x += it.value_w + (it.rates ? label_gap : unit_gap) + Width(f.regular, small, it.unit);
		}
		return at.y + h;
	}

	// ---- Full and Custom: the card

	inline float DrawCard(ImDrawList* dl, const Fonts& f, float u, const Overlay& o, int place, float margin, float position_y, float sw,
		float sh)
	{
		const float pad = std::round(16.0f * u), w = std::round(280.0f * u), inner = w - 2 * pad;
		const float big = std::round(40.0f * u), small = std::round(13.0f * u), tiny = std::round(12.0f * u);
		const float num = std::round(14.0f * u), meter_h = std::round(22.0f * u), graph_h = std::round(44.0f * u);
		const float section_gap = std::round(12.0f * u), line_h = std::round(18.0f * u);

		// The small lines beside the FPS: "16.7 ms · 100% speed", "1% low 43 · 0.1% 32".
		const auto num_piece = [&](std::string s, Level level) { return Piece{std::move(s), f.bold, small, Colour(level)}; };
		const auto word = [&](std::string s) { return Piece{std::move(s), f.regular, small, LABEL}; };
		std::vector<std::vector<Piece>> side;
		{
			std::vector<Piece> l;
			char b[32];
			if (o.frame_ms)
			{
				std::snprintf(b, sizeof(b), "%.1f", static_cast<double>(o.last_ms));
				l.push_back(num_piece(b, o.frame_level));
				l.push_back(word(" ms"));
			}
			if (o.speed)
			{
				if (!l.empty())
					l.push_back(word("  \xc2\xb7  "));
				l.push_back(num_piece(Whole(o.speed_percent) + "%", o.speed_level));
				l.push_back(word(" speed"));
			}
			if (!l.empty())
				side.push_back(std::move(l));
			std::vector<Piece> m;
			if (o.frame_ms)
			{
				m.push_back(word("1% low "));
				m.push_back(num_piece(Whole(o.low1_fps), Level::Fine));
				m.push_back(word("  \xc2\xb7  0.1% "));
				m.push_back(num_piece(Whole(o.low01_fps), Level::Fine));
			}
			if (o.vps)
			{
				if (!m.empty())
					m.push_back(word("  \xc2\xb7  "));
				m.push_back(num_piece(Whole(o.vps_value), Level::Fine));
				m.push_back(word(" VPS"));
			}
			if (!m.empty())
				side.push_back(std::move(m));
		}

		// The height, part by part.
		// The FPS and the small lines side by side when they fit (a smaller FPS if need be, as with
		// frame generation's two rates); else the small lines under the FPS.
		const bool top = o.fps || !side.empty();
		float side_w = 0.0f;
		for (const std::vector<Piece>& l : side)
			side_w = std::max(side_w, PiecesWidth(l));
		const float fps_gap = std::round(6.0f * u), side_gap = std::round(12.0f * u);
		float fps_size = big;
		const auto fps_width = [&](float size) { return Rates(nullptr, f.bold, size, 0, 0, o, TEXT) + fps_gap + Width(f.regular, small, "FPS"); };
		bool stacked = false;
		if (o.fps && !side.empty() && fps_width(fps_size) + side_gap + side_w > inner)
		{
			fps_size = std::round(30.0f * u);
			stacked = fps_width(fps_size) + side_gap + side_w > inner;
			if (stacked)
				fps_size = big;
		}
		const float side_h = static_cast<float>(side.size()) * line_h;
		const float top_h = !o.fps ? side_h : stacked ? fps_size + std::round(6.0f * u) + side_h : std::max(fps_size, side_h);
		const size_t stat_lines = (o.stats.size() + 1) / 2;
		const bool footer = !o.footer_left.empty() || !o.footer_right.empty();
		float h = pad;
		int parts = 0;
		const auto part = [&](float ph) {
			h += (parts++ ? section_gap : 0.0f) + ph;
		};
		if (top)
			part(top_h);
		if (o.graph && o.history && o.history_size)
			part(graph_h);
		if (!o.meters.empty())
			part(static_cast<float>(o.meters.size()) * meter_h - std::round(6.0f * u));
		if (stat_lines)
			part(static_cast<float>(stat_lines) * line_h);
		if (footer || !o.version.empty())
			part(std::round(9.0f * u) + (footer ? line_h : 0.0f) + (o.version.empty() ? 0.0f : line_h));
		h += pad;

		const ImVec2 at = Place(place, w, h, margin, position_y, sw, sh);
		Ground(dl, at, ImVec2(at.x + w, at.y + h), std::round(16.0f * u));
		const float x = at.x + pad, right = at.x + w - pad;
		float y = at.y + pad;
		parts = 0;
		const auto next = [&]() {
			if (parts++)
				y += section_gap;
		};

		if (top)
		{
			next();
			if (o.fps)
			{
				// The figure's top is where the font's line starts; the label sits on its baseline.
				const float fy = y + (stacked ? 0.0f : std::round((top_h - fps_size) * 0.5f));
				const float fw = Rates(dl, f.bold, fps_size, x, fy - std::round(fps_size * 0.08f), o, Colour(o.fps_level));
				dl->AddText(f.regular, small, ImVec2(x + fw + fps_gap, fy + fps_size - small - std::round(fps_size * 0.2f)), LABEL, "FPS");
			}
			const bool beside = o.fps && !stacked;
			const float side_top = !o.fps ? y : stacked ? y + fps_size + std::round(6.0f * u) : y + std::round((top_h - side_h) * 0.5f);
			for (size_t i = 0; i < side.size(); i++)
			{
				const float lw = PiecesWidth(side[i]);
				DrawPieces(dl, side[i], beside ? right - lw : x, side_top + static_cast<float>(i) * line_h);
			}
			y += top_h;
		}

		// The frame times, the newest at the right: thin bars as tall as the frame was long, grey,
		// amber or red against the budget; the budget a faint line.
		if (o.graph && o.history && o.history_size)
		{
			next();
			const float budget = o.budget_ms > 0.0f ? o.budget_ms : 1000.0f / 60.0f;
			const float top_ms = budget * 2.0f; // a frame of two budgets fills it; longer ones are cut and marked
			const float base = y + graph_h;
			dl->AddRectFilled(ImVec2(x - std::round(4.0f * u), y - std::round(4.0f * u)), ImVec2(right + std::round(4.0f * u), base + 1.0f),
				IM_COL32(255, 255, 255, 8), std::round(6.0f * u));
			dl->AddLine(ImVec2(x, base), ImVec2(right, base), RULE, 1.0f);
			const float by = std::round(base - graph_h * (budget / top_ms)) + 0.5f;
			for (float dx = x; dx < right; dx += std::round(6.0f * u))
				dl->AddLine(ImVec2(dx, by), ImVec2(std::min(right, dx + std::round(3.0f * u)), by), RULE, 1.0f);
			const float step = inner / static_cast<float>(o.history_size);
			const float bar_w = std::max(1.0f, std::round(step * 0.6f));
			for (uint32_t k = 0; k < o.history_size; k++)
			{
				const float t = o.history[(o.history_pos + k) % o.history_size];
				if (!(t > 0.01f))
					continue;
				const Level level = PS5OsdLogic::FrameLevel(t, budget);
				const ImU32 c = level == Level::Over ? RED : level == Level::Near ? AMBER : GRAPH_BAR;
				const float bh = std::max(1.0f, std::round(graph_h * std::min(1.0f, t / top_ms)));
				const float bx = std::round(x + step * static_cast<float>(k));
				dl->AddRectFilled(ImVec2(bx, base - bh), ImVec2(bx + bar_w, base), c);
				if (t > top_ms) // cut: a mark above it
					dl->AddRectFilled(ImVec2(bx - 1.0f, y - std::round(2.0f * u)), ImVec2(bx + bar_w + 1.0f, y), RED);
			}
			y += graph_h;
		}

		// The loads: a label, a thin bar, the value.
		if (!o.meters.empty())
		{
			next();
			float label_w = 0.0f;
			for (const Meter& m : o.meters)
				label_w = std::max(label_w, Width(f.regular, tiny, m.label.c_str()));
			const float value_w = DigitWidth(f.bold, num) * 3.0f + Width(f.regular, tiny, "%") + std::round(2.0f * u);
			const float bar_x0 = x + label_w + std::round(10.0f * u), bar_x1 = right - value_w - std::round(10.0f * u);
			const float thick = std::max(2.0f, std::round(4.0f * u));
			for (size_t i = 0; i < o.meters.size(); i++)
			{
				const Meter& m = o.meters[i];
				const float my = y + static_cast<float>(i) * meter_h;
				const float mid = my + std::round(num * 0.55f);
				dl->AddText(f.regular, tiny, ImVec2(x, mid - std::round(tiny * 0.55f)), LABEL, m.label.c_str());
				dl->AddRectFilled(ImVec2(bar_x0, mid - thick * 0.5f), ImVec2(bar_x1, mid + thick * 0.5f), TRACK, thick * 0.5f);
				const float fill = (bar_x1 - bar_x0) * std::clamp(m.percent, 0.0f, 100.0f) / 100.0f;
				if (fill >= 1.0f)
					dl->AddRectFilled(ImVec2(bar_x0, mid - thick * 0.5f), ImVec2(bar_x0 + fill, mid + thick * 0.5f),
						m.level == Level::Fine ? IM_COL32(243, 236, 228, 200) : Colour(m.level), thick * 0.5f);
				const float uw = Width(f.regular, tiny, m.unit);
				const float vw = TabWidth(f.bold, num, m.value);
				Tab(dl, f.bold, num, right - uw - std::round(2.0f * u) - vw, mid - std::round(num * 0.55f), Colour(m.level), m.value);
				if (uw > 0.0f)
					dl->AddText(f.regular, tiny, ImVec2(right - uw, mid - std::round(tiny * 0.55f) + std::round(1.0f * u)), LABEL, m.unit);
			}
			y += static_cast<float>(o.meters.size()) * meter_h - std::round(6.0f * u);
		}

		// Custom's details, two to a line: a label at the left of its half, the value at its right.
		if (stat_lines)
		{
			next();
			const float half = std::round((inner - std::round(16.0f * u)) * 0.5f);
			for (size_t i = 0; i < o.stats.size(); i++)
			{
				const Stat& s = o.stats[i];
				const float sx = x + static_cast<float>(i % 2) * (half + std::round(16.0f * u));
				const float sy = y + static_cast<float>(i / 2) * line_h;
				dl->AddText(f.regular, tiny, ImVec2(sx, sy + std::round(1.0f * u)), LABEL, s.label.c_str());
				const float uw = Width(f.regular, tiny, s.unit);
				const float vw = TabWidth(f.bold, tiny, s.value);
				const float vx = sx + half - uw - (uw > 0.0f ? std::round(2.0f * u) : 0.0f) - vw;
				Tab(dl, f.bold, tiny, vx, sy + std::round(1.0f * u), Colour(s.level), s.value);
				if (uw > 0.0f)
					dl->AddText(f.regular, tiny, ImVec2(sx + half - uw, sy + std::round(1.0f * u)), LABEL, s.unit);
			}
			y += static_cast<float>(stat_lines) * line_h;
		}

		// The footer: a rule, then the resolution at the left, temperature and memory at the right.
		if (footer || !o.version.empty())
		{
			next();
			dl->AddLine(ImVec2(x, std::round(y) + 0.5f), ImVec2(right, std::round(y) + 0.5f), RULE, 1.0f);
			y += std::round(9.0f * u);
			if (footer)
			{
				Tab(dl, f.regular, tiny, x, y, LABEL, o.footer_left);
				Tab(dl, f.regular, tiny, right - TabWidth(f.regular, tiny, o.footer_right), y, LABEL, o.footer_right);
				y += line_h;
			}
			if (!o.version.empty())
				Tab(dl, f.regular, tiny, x, y, LABEL, o.version);
		}
		return at.y + h;
	}

	/// place: PCSX2's OsdOverlayPos (1 top left ... 9 bottom right). u: design pixels to the screen's.
	/// Returns the y under the overlay.
	inline float Draw(ImDrawList* dl, const Fonts& fonts, float u, const Overlay& o, int place, float margin, float position_y, float screen_w,
		float screen_h)
	{
		if (o.Empty() || !fonts.regular || !fonts.bold)
			return position_y;
		switch (o.size)
		{
			case PS5OsdLogic::Minimal:
				return DrawMinimal(dl, fonts, u, o, place, margin, position_y, screen_w, screen_h);
			case PS5OsdLogic::Compact:
				return DrawCompact(dl, fonts, u, o, place, margin, position_y, screen_w, screen_h);
			default:
				return DrawCard(dl, fonts, u, o, place, margin, position_y, screen_w, screen_h);
		}
	}
} // namespace PS5OsdDraw
