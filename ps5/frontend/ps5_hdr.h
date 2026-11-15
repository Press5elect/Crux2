// Crux2 - HDR output and debanding: the settings, and the "Adjust HDR brightness" test screen.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// As OGBox (Press5elect) has them. The passes are PCSX2's Vulkan device's last before the screen
// (vendor/pcsx2 GSDeviceVK_PS5.cpp, the PS5Final passes; the shader is
// ps5/package/resources/shaders/vulkan/ps5_final.glsl), which also defines the functions below.
// The settings are the console's own, for every game, in [PS5] of inis/PCSX2.ini:
//   HDR       false   the picture sent as HDR10 when the console's output is HDR
//   HDRPeak   1000    the game's highlights raised up to this many nits (0: off; 400 to 4000)
//   HDRWhite  203     white's nits (ITU-R BT.2408's reference), the menus' too (80 to 400)
//   Deband    0       0 off, 1 low (32-bit colour's fine steps), 2 high (16-bit colour's wider ones)
//   ColourFilter 0    the game's picture for colour blindness: 0 off, 1 red-weak, 2 green-weak,
//                     3 blue-weak, 4 greyscale (SDR and HDR; the menus keep their own colours)
// HDR is on only while the setting is on, the console's output reports HDR, and the graphics driver
// took the framebuffers as HDR10; otherwise everything is drawn as before, SDR.

#pragma once

#include <cstdlib>
#include <vector>

namespace PS5Hdr
{
	inline constexpr int DEFAULT_PEAK = 1000;
	inline constexpr int DEFAULT_WHITE = 203;
	inline constexpr int MIN_WHITE = 80;
	inline constexpr int MAX_WHITE = 400;

	/// The highlights' choices: Off (0), 400 to 1000 in 100s, then 1250 to 4000.
	inline std::vector<int> PeakChoices()
	{
		std::vector<int> v{0};
		for (int n = 400; n <= 1000; n += 100)
			v.push_back(n);
		for (int n : {1250, 1500, 1750, 2000, 2500, 3000, 4000})
			v.push_back(n);
		return v;
	}

	/// The white level's choices: 80 to 400 in 10s, with 203 (the reference) in its place.
	inline std::vector<int> WhiteChoices()
	{
		std::vector<int> v;
		for (int n = MIN_WHITE; n <= MAX_WHITE; n += 10)
		{
			if (n > DEFAULT_WHITE && (v.empty() || v.back() < DEFAULT_WHITE))
				v.push_back(DEFAULT_WHITE);
			v.push_back(n);
		}
		return v;
	}

	/// value stepped by `by` places in list (0, Off, left out), from the nearest choice when it is none of them.
	inline int Step(const std::vector<int>& list, int value, int by)
	{
		const int n = static_cast<int>(list.size());
		if (n == 0)
			return value;
		const int lo = (n > 1 && list[0] == 0) ? 1 : 0;
		int at = lo, best = 1 << 30;
		for (int i = lo; i < n; i++)
		{
			const int d = std::abs(list[static_cast<size_t>(i)] - value);
			if (d < best)
			{
				best = d;
				at = i;
			}
		}
		const int to = at + by;
		return list[static_cast<size_t>(to < lo ? lo : (to > n - 1 ? n - 1 : to))];
	}

	/// The white level a setting's value stands for (clamped), and the highlights' peak (0: none).
	inline int WhiteOf(int value) { return value < MIN_WHITE ? MIN_WHITE : (value > MAX_WHITE ? MAX_WHITE : value); }
	inline int PeakOf(int value) { return value <= 0 ? 0 : (value > 10000 ? 10000 : value); }

	/// Debanding's numbers for its setting (1 low, 2 high) and a screen of `height` lines: a band's step,
	/// the dither (half a level of 255) and the reach (32 lines of 480).
	struct Deband
	{
		float step, dither, reach;
	};
	inline Deband DebandOf(int setting, int height)
	{
		return Deband{(setting >= 2 ? 5.0f : 2.0f) / 255.0f, 0.5f / 255.0f, 32.0f * static_cast<float>(height) / 480.0f};
	}

	/// The colour filter's choices, as Porpoise has them (From Porpoise aa6ec1f, GPL-3.0-or-later): the
	/// setting's value is the place in this list. Red-weak and green-weak keep reds and greens apart,
	/// blue-weak blues and yellows.
	inline constexpr const char* FILTER_CHOICES[] = {"Off", "Red-weak", "Green-weak", "Blue-weak", "Greyscale"};
	inline constexpr int FILTER_COUNT = static_cast<int>(sizeof(FILTER_CHOICES) / sizeof(FILTER_CHOICES[0]));
	/// The filter a setting's value stands for (anything else: off).
	inline int FilterOf(int value) { return (value > 0 && value < FILTER_COUNT) ? value : 0; }

	// ---- defined in GSDeviceVK_PS5.cpp (vendor/pcsx2) ----

	/// Settings > Graphics > Adjust HDR brightness: opens the test screen (a toast instead while HDR is not on).
	void OpenCalibration();
	/// FullscreenUI::Render, each frame: while the test screen is up it draws its words, reads the controller
	/// and answers true (the other screens then neither draw nor read the controller this frame).
	bool CalibrationFrame();
	/// What HDR output is doing, for the settings row: "On", "Off", "The console is sending SDR", ...
	const char* Status();
} // namespace PS5Hdr
