// Crux2 - the performance overlay's decisions: when a figure is fine, close to its limit or over
// it, and what each of the overlay's sizes ([PS5] OsdSize) switches on. Header-only and plain C++,
// for ps5_osd.cpp, the settings (bp_pcsx2.cpp, ps5_host.cpp), the PC preview and the host test
// (ps5/tests/osd/osd_logic_test.cpp).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Colour is for meaning only: white = fine, amber = close to the limit, red = over it.
//
//   figure                          amber (Near)                  red (Over)
//   a thread's or the GPU's load    >= 80 %                       >= 95 %
//   frames (the speed against the   under 98 % of the target      under 90 % of the target
//     speed asked for)
//   a frame's time (against the     > 110 % of the budget         > 150 % of the budget
//     screen's frame at the target  (a little over)               (a spike: a frame or more
//     speed, its "budget")                                        dropped or late)
//
// The sizes are presets of the overlay's existing switches (EmuCore/GS OsdShow*):
//
//   switch             Minimal  Compact  Full
//   OsdShowFPS            x        x       x
//   OsdShowGPU                     x       x
//   OsdShowCPU                     x       x     (EE, GS, VU)
//   OsdShowSpeed                           x
//   OsdShowFrameTimes                      x     (frame time, 1% / 0.1% lows, the graph)
//   OsdShowResolution                      x
//   OsdShowHardwareInfo                    x     (temperature, memory)
//   OsdShowVPS, OsdShowGSStats, OsdShowGPUStats, OsdShowVersion: off in every preset
//
// Custom: the switches as they are decide what is shown (laid out as Full's card). A preset whose
// switches were changed afterwards is Custom (the settings write Custom then: SizeAfterSwitch).

#pragma once

#include <cstddef>

namespace PS5OsdLogic
{
	// ---- the levels

	enum class Level
	{
		Fine,
		Near,
		Over,
	};

	constexpr float LOAD_NEAR = 80.0f; // percent
	constexpr float LOAD_OVER = 95.0f;
	constexpr float SPEED_NEAR = 0.98f; // of the target speed
	constexpr float SPEED_OVER = 0.90f;
	constexpr float FRAME_NEAR = 1.10f; // of the frame's budget
	constexpr float FRAME_OVER = 1.50f;

	/// A thread's or the graphics chip's load, in percent.
	constexpr Level LoadLevel(float percent)
	{
		return percent >= LOAD_OVER ? Level::Over : percent >= LOAD_NEAR ? Level::Near : Level::Fine;
	}

	/// The frames: the emulation's speed (percent, PerformanceMetrics::GetSpeed) against the speed
	/// asked for (VMManager::GetTargetSpeed: 1 = normal; 0 = unlimited, which is never slow). A game
	/// that runs at 30 FPS by design is at 100 % speed, so it is fine.
	constexpr Level SpeedLevel(float speed_percent, float target_speed)
	{
		if (!(target_speed > 0.0f))
			return Level::Fine;
		const float ratio = speed_percent / (100.0f * target_speed);
		return ratio < SPEED_OVER ? Level::Over : ratio < SPEED_NEAR ? Level::Near : Level::Fine;
	}

	/// One frame's time in ms against the budget (FrameBudgetMs).
	constexpr Level FrameLevel(float ms, float budget_ms)
	{
		if (!(budget_ms > 0.0f))
			return Level::Fine;
		return ms > budget_ms * FRAME_OVER ? Level::Over : ms > budget_ms * FRAME_NEAR ? Level::Near : Level::Fine;
	}

	/// The time a frame of the screen may take: the game's vertical rate (Hz, 59.94 or 50) at the
	/// target speed. 16.7 ms when either is unknown.
	constexpr float FrameBudgetMs(float vertical_hz, float target_speed)
	{
		if (!(vertical_hz > 1.0f))
			vertical_hz = 60.0f;
		if (!(target_speed > 0.0f))
			target_speed = 1.0f;
		return 1000.0f / (vertical_hz * target_speed);
	}

	/// The worse of two levels.
	constexpr Level Worse(Level a, Level b)
	{
		return static_cast<int>(a) >= static_cast<int>(b) ? a : b;
	}

	// ---- the sizes

	enum Size : int
	{
		Custom = 0,
		Minimal = 1,
		Compact = 2,
		Full = 3,
	};

	constexpr int DEFAULT_SIZE = Compact;

	/// The switches a size sets, in the order of SWITCH_KEYS.
	struct Switches
	{
		bool fps = false, gpu = false, cpu = false, speed = false, frame_times = false, resolution = false, hardware = false, vps = false,
			 gs_stats = false, gpu_stats = false, version = false;

		constexpr bool operator==(const Switches&) const = default;
		constexpr bool Any() const
		{
			return fps || gpu || cpu || speed || frame_times || resolution || hardware || vps || gs_stats || gpu_stats || version;
		}
	};

	/// The keys (section EmuCore/GS) the sizes set, and their fields.
	struct SwitchKey
	{
		const char* key;
		bool Switches::*field;
	};
	inline constexpr const char* SWITCH_SECTION = "EmuCore/GS";
	inline constexpr SwitchKey SWITCH_KEYS[] = {
		{"OsdShowFPS", &Switches::fps},
		{"OsdShowGPU", &Switches::gpu},
		{"OsdShowCPU", &Switches::cpu},
		{"OsdShowSpeed", &Switches::speed},
		{"OsdShowFrameTimes", &Switches::frame_times},
		{"OsdShowResolution", &Switches::resolution},
		{"OsdShowHardwareInfo", &Switches::hardware},
		{"OsdShowVPS", &Switches::vps},
		{"OsdShowGSStats", &Switches::gs_stats},
		{"OsdShowGPUStats", &Switches::gpu_stats},
		{"OsdShowVersion", &Switches::version},
	};
	inline constexpr size_t SWITCH_COUNT = sizeof(SWITCH_KEYS) / sizeof(SWITCH_KEYS[0]);

	/// Whether `key` (in EmuCore/GS) is one the sizes set.
	inline bool IsSwitchKey(const char* key)
	{
		for (const SwitchKey& k : SWITCH_KEYS)
		{
			const char *a = k.key, *b = key;
			while (*a && *a == *b)
				a++, b++;
			if (*a == 0 && *b == 0)
				return true;
		}
		return false;
	}

	constexpr Switches Preset(int size)
	{
		Switches s;
		if (size < Minimal || size > Full)
			return s;
		s.fps = true;
		if (size >= Compact)
			s.gpu = s.cpu = true;
		if (size >= Full)
			s.speed = s.frame_times = s.resolution = s.hardware = true;
		return s;
	}

	constexpr bool IsPreset(int size)
	{
		return size >= Minimal && size <= Full;
	}

	/// What the overlay is drawn as: a preset whose switches are as it set them, else Custom.
	constexpr Size EffectiveSize(int setting, const Switches& now)
	{
		return (IsPreset(setting) && Preset(setting) == now) ? static_cast<Size>(setting) : Custom;
	}

	/// The size the settings keep after the user changed one of the switches (now: all of them after
	/// the change; fps_switched_on: the change was OsdShowFPS turned on). Turning the FPS counter on
	/// with nothing else shown brings the chosen size's figures back (the default size, Compact, for
	/// a new user); with everything off the size is kept for next time; otherwise a preset that is no
	/// longer as it set the switches becomes Custom. `apply` is set when the preset's switches are to
	/// be written.
	constexpr int SizeAfterSwitch(int setting, const Switches& now, bool fps_switched_on, bool& apply)
	{
		apply = false;
		if (!IsPreset(setting))
			return Custom;
		if (!now.Any())
			return setting;
		Switches fps_only;
		fps_only.fps = true;
		if (fps_switched_on && now == fps_only)
		{
			apply = !(Preset(setting) == now);
			return setting;
		}
		return Preset(setting) == now ? setting : Custom;
	}

	/// The size for a settings file that has none (written once): the preset its switches match,
	/// Custom when they match none, the default when nothing is shown.
	constexpr int InitialSize(const Switches& now)
	{
		if (!now.Any())
			return DEFAULT_SIZE;
		for (int s = Minimal; s <= Full; s++)
		{
			if (Preset(s) == now)
				return s;
		}
		return Custom;
	}

	// ---- frame generation: "30 → 60" (the game's rate, then the screen's) when it runs

	/// Whether the FPS figure is shown as two rates: frame generation is giving the screen pictures
	/// (shown_per_second > 0.5) at a rate other than the game's.
	constexpr bool ShowsTwoRates(float game_fps, float shown_per_second)
	{
		return shown_per_second > 0.5f && (shown_per_second - game_fps > 1.5f || game_fps - shown_per_second > 1.5f);
	}
} // namespace PS5OsdLogic
