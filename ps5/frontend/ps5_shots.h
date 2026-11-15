// Crux2 - its own screenshots: the game's picture (never the menus over it), as PNG.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// From Porpoise src/porpoise_shots.cpp (GPL-3.0-or-later). Options + Square (the hotkey "Save
// Screenshot", Settings > Controllers > Hotkeys; Porpoise's touch pad + Square would be a PS2
// Select or Start with Square here, since the touch pad is those in a game) takes the picture the
// game shows, as the emulator draws it at its internal resolution with the shape it has on the
// screen: the quick menu and the notices are not in it. It is written on a thread of its own to
// the snapshots folder (snaps/ of the title folder), as snaps/<serial>/<date>_<time>.png. When the
// game has no picture of play yet (pictures/<serial>.snap.png, ps5_online.h), a copy 640 pixels
// wide becomes it.

#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace PS5Shots
{
/// The hotkey's binding when the settings have none yet: Options and Square together (chords take
/// the place of the single Options, which opens the quick menu as it is let go, so the menu does not
/// open). The Square still reaches the game.
inline constexpr const char* DEFAULT_BINDING = "SDL-0/Start & SDL-0/FaceWest";

/// The game's picture now, written as told above. Any thread; nothing without a game.
void Take();

// ---- what can be tried on the PC

/// "2026-11-15_10-00-00" for a moment (local time), "_2"... after it when that is taken.
inline std::string Name(time_t when, int nth)
{
	char out[48] = "shot";
	struct tm local;
	if (localtime_r(&when, &local) != nullptr)
		strftime(out, sizeof(out), "%Y-%m-%d_%H-%M-%S", &local);
	return nth <= 1 ? std::string(out) : std::string(out) + "_" + std::to_string(nth);
}

/// The folder of a game's shots: its serial, made safe for a file name; "BIOS" for none.
inline std::string FolderName(const std::string& serial)
{
	std::string out;
	for (const char c : serial)
		out += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') ? c : '_';
	return out.empty() ? std::string("BIOS") : out;
}

/// A picture made smaller (never larger) to `most_width` across, its shape kept: each pixel the
/// average of those it covers. Pixels are RGBA, one uint32_t each, as the emulator gives them.
inline std::vector<uint32_t> Smaller(const std::vector<uint32_t>& pixels, uint32_t width, uint32_t height, uint32_t most_width,
	uint32_t* out_width, uint32_t* out_height)
{
	if (width == 0 || height == 0 || pixels.size() < static_cast<size_t>(width) * height || width <= most_width)
	{
		*out_width = width;
		*out_height = height;
		return pixels;
	}
	const uint32_t w = most_width;
	const uint32_t h = std::max<uint32_t>(1, static_cast<uint32_t>((static_cast<uint64_t>(height) * w + width / 2) / width));
	std::vector<uint32_t> out(static_cast<size_t>(w) * h);
	for (uint32_t y = 0; y < h; y++)
	{
		const uint32_t y0 = static_cast<uint32_t>(static_cast<uint64_t>(y) * height / h);
		const uint32_t y1 = std::max(y0 + 1, static_cast<uint32_t>(static_cast<uint64_t>(y + 1) * height / h));
		for (uint32_t x = 0; x < w; x++)
		{
			const uint32_t x0 = static_cast<uint32_t>(static_cast<uint64_t>(x) * width / w);
			const uint32_t x1 = std::max(x0 + 1, static_cast<uint32_t>(static_cast<uint64_t>(x + 1) * width / w));
			uint64_t sum[4] = {0, 0, 0, 0};
			uint64_t n = 0;
			for (uint32_t yy = y0; yy < y1; yy++)
			{
				for (uint32_t xx = x0; xx < x1; xx++)
				{
					const uint32_t p = pixels[static_cast<size_t>(yy) * width + xx];
					for (int c = 0; c < 4; c++)
						sum[c] += (p >> (8 * c)) & 0xff;
					n++;
				}
			}
			uint32_t q = 0;
			for (int c = 0; c < 4; c++)
				q |= static_cast<uint32_t>((sum[c] + n / 2) / n) << (8 * c);
			out[static_cast<size_t>(y) * w + x] = q | 0xff000000u; // (opaque: a game's alpha is no picture's)
		}
	}
	*out_width = w;
	*out_height = h;
	return out;
}
} // namespace PS5Shots
