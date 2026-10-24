// Crux2 - automatic quality: the runtime of ps5_auto_quality.h.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Every half second (PCSX2's figures are updated as often) the speed, the graphics chip's use and
// the GS thread's use are looked at:
//   - three samples in a row (1.5 s) under 95% of the speed asked for, with the graphics chip over
//     90% busy: one step down (4x to 3x, 3x to 2x, 2x to 1x). The GS thread busy alone is not a
//     reason: its work is the game's draws, which a lower resolution does not make fewer (Black's
//     heaviest scenes, 2026-10-21: stepped from 6x to 1x on the GS thread at 97%, and still 85-88%
//     at 1x with the graphics chip at 27%);
//   - a step down that did not make the game faster (two samples after it settles, the speed not
//     3 points above what it was) is taken back, and no step down is made for the next 30 s;
//   - twenty samples in a row (10 s) at full speed, with the graphics chip under 65% and the GS
//     thread under 75%: one step back up, never above the resolution you chose;
//   - after a step, the next two samples are not counted: the texture cache is filled again at
//     the new size, which is slow for a moment and says nothing about the scene;
//   - after a step down, at least 15 s before a step up, so a scene on the edge does not swing.
// Only at normal speed (not fast forward or slow motion), with the hardware renderer.
// Needs proper testing on a console: the thresholds are a first choice.

#include "ps5_auto_quality.h"
#include "ps5_pgs.h"

#include "Config.h"
#include "GS/GS.h"
#include "Host.h"
#include "IconsFontAwesome.h"
#include "PerformanceMetrics.h"
#include "VMManager.h"

#include "common/SmallString.h"
#include "common/Timer.h"

#include "fmt/format.h"

#include <algorithm>
#include <cmath>

extern "C" void ps5px_log(const char* format, ...);

namespace PS5AutoQuality
{
	namespace
	{
		constexpr double SAMPLE_SECONDS = 0.5;
		constexpr int DOWN_SAMPLES = 3;
		constexpr int UP_SAMPLES = 20;
		constexpr int SETTLE_SAMPLES = 2;
		constexpr int HOLD_AFTER_DOWN_SAMPLES = 30;
		constexpr float SLOW = 0.95f;
		constexpr float FULL = 0.99f;
		constexpr float BUSY = 90.0f;
		constexpr float GPU_ROOM = 65.0f;
		constexpr float GS_ROOM = 75.0f;

		Common::Timer s_since_sample;
		Common::Timer s_since_setting;
		bool s_on = false;
		bool s_known = false;     // s_on was read once
		float s_chosen = 0.0f;    // the resolution of the settings (yours)
		float s_ours = 0.0f;      // the one set here, while it differs from yours
		int s_slow = 0, s_room = 0, s_settle = 0, s_hold = 0;
		uint32_t s_down = 0, s_up = 0;
		float s_lowest = 0.0f;
		// The step down being checked: the speed before it, the samples left before it is judged.
		float s_speed_before = 0.0f;
		int s_judge = 0;
		int s_no_down = 0; // samples left in which no step down is made
		constexpr int JUDGE_SAMPLES = 2;
		constexpr float HELPED = 3.0f; // points of speed a step down must win
		constexpr int NO_DOWN_SAMPLES = 60;

		// The session's figures (at normal speed only).
		uint32_t s_samples = 0, s_slow_samples = 0, s_streak = 0, s_longest = 0, s_slow_logs = 0;
		double s_speed_sum = 0.0;

		// paraLLEl-GS: the samples a pixel step instead of the resolution (16, 8, 4, 2, 1). What the
		// graphics card is doing is its own time a frame (its timestamps), against the frame's period;
		// the GS thread waiting for it says the same when the timestamps are not there.
		uint32_t s_pgs_chosen = 0, s_pgs_ours = 0, s_pgs_lowest = 0;
		int s_pgs_slow = 0, s_pgs_room = 0, s_pgs_settle = 0, s_pgs_hold = 0;
		constexpr float PGS_BUSY = 0.85f; // of the frame's period
		constexpr float PGS_ROOM = 0.55f;

		// Not below 4 samples while the picture at twice the size is on: it needs 4 or more, and below them the
		// picture falls to the PS2's own size, far softer than the few samples saved are worth.
		uint32_t PgsStepDown(uint32_t s)
		{
			const uint32_t floor = Host::GetBoolSettingValue("EmuCore/GS", "PS5ParallelGSHighRes", true) ? 4u : 1u;
			return s / 2 >= floor ? s / 2 : s;
		}
		uint32_t PgsStepUp(uint32_t s) { return s < 16 ? s * 2 : 16; }

		void PgsSet(uint32_t samples, bool down)
		{
			PS5PGS::SetSamples(samples);
			s_pgs_ours = samples;
			s_pgs_settle = SETTLE_SAMPLES;
			s_pgs_slow = s_pgs_room = 0;
			s_pgs_hold = down ? HOLD_AFTER_DOWN_SAMPLES : 0;
			if (down)
			{
				s_down++;
				s_pgs_lowest = s_pgs_lowest ? std::min(s_pgs_lowest, samples) : samples;
			}
			else
			{
				s_up++;
			}
			const std::string text = (samples >= s_pgs_chosen) ?
				fmt::format("Automatic quality: back to {} samples a pixel", samples) :
				fmt::format("Automatic quality: {} samples a pixel for now, to keep full speed (yours: {})", samples, s_pgs_chosen);
			Host::AddIconOSDMessage("PS5AutoQuality", down ? ICON_FA_GAUGE_SIMPLE_HIGH : ICON_FA_GAUGE_HIGH, text, Host::OSD_QUICK_DURATION);
			const PS5PGS::Figures f = PS5PGS::GetFigures();
			ps5px_log("auto quality: paraLLEl-GS at %u samples (%s; yours %u; speed %.0f%%, the graphics card %.1f ms a frame, the GS "
					  "thread waited %.1f ms a frame for it)",
				samples, down ? "down" : "up", s_pgs_chosen, PerformanceMetrics::GetSpeed(), f.gpu_ms, f.wait_ms);
		}

		// paraLLEl-GS's own stepping (the resolution is left alone).
		void PgsVSync(float target, float speed)
		{
			const PS5PGS::Figures f = PS5PGS::GetFigures();
			if (f.samples == 0)
				return;
			if (s_pgs_ours == 0 || (f.samples != s_pgs_ours && f.samples != s_pgs_chosen))
			{
				// The first look, or the settings applied again: what is in effect is yours.
				s_pgs_chosen = f.samples;
				s_pgs_ours = f.samples;
				s_pgs_slow = s_pgs_room = s_pgs_hold = 0;
			}
			if (s_pgs_settle > 0)
			{
				s_pgs_settle--;
				return;
			}
			if (s_pgs_hold > 0)
				s_pgs_hold--;
			const float period_ms = 1000.0f / std::max(1.0f, target * 0.6f); // 60 Hz at 100%: 16.7 ms
			// The graphics card's load: its own time a frame, or, without timestamps, the wait for it.
			const float load = (f.gpu_ms > 0.0f) ? f.gpu_ms / period_ms : (f.wait_ms > 0.5f ? 1.0f : 0.0f);
			const uint32_t now = s_pgs_ours;
			if (speed < target * SLOW && load > PGS_BUSY)
			{
				s_pgs_room = 0;
				if (++s_pgs_slow >= DOWN_SAMPLES && PgsStepDown(now) < now)
					PgsSet(PgsStepDown(now), true);
				return;
			}
			s_pgs_slow = 0;
			if (now < s_pgs_chosen && speed >= target * FULL && load < PGS_ROOM && s_pgs_hold == 0)
			{
				if (++s_pgs_room >= UP_SAMPLES)
					PgsSet(std::min(s_pgs_chosen, PgsStepUp(now)), false);
				return;
			}
			s_pgs_room = 0;
		}

		void Set(float scale, bool down)
		{
			Pcsx2Config::GSOptions config = GSConfig;
			config.UpscaleMultiplier = scale;
			GSUpdateConfig(config);
			s_ours = scale;
			s_settle = SETTLE_SAMPLES;
			s_slow = s_room = 0;
			s_hold = down ? HOLD_AFTER_DOWN_SAMPLES : 0;
			if (down)
			{
				s_down++;
				s_lowest = (s_lowest == 0.0f) ? scale : std::min(s_lowest, scale);
			}
			else
			{
				s_up++;
			}

			const std::string text = (scale >= s_chosen) ?
				fmt::format("Automatic quality: back to {:g}x", scale) :
				fmt::format("Automatic quality: {:g}x for now, to keep full speed (yours: {:g}x)", scale, s_chosen);
			Host::AddIconOSDMessage("PS5AutoQuality", down ? ICON_FA_GAUGE_SIMPLE_HIGH : ICON_FA_GAUGE_HIGH, text, Host::OSD_QUICK_DURATION);
			ps5px_log("auto quality: %gx (%s; yours %gx; speed %.0f%%, graphics chip %.0f%%, GS thread %.0f%%)", scale,
				down ? "down" : "up", s_chosen, PerformanceMetrics::GetSpeed(), PerformanceMetrics::GetGPUUsage(),
				PerformanceMetrics::GetGSThreadUsage());
		}
	} // namespace

	void VSync()
	{
		if (s_since_sample.GetTimeSeconds() < SAMPLE_SECONDS)
			return;
		s_since_sample.Reset();

		if (VMManager::GetLimiterMode() == LimiterModeType::Nominal && VMManager::GetTargetSpeed() > 0.0f)
		{
			const float ratio = PerformanceMetrics::GetSpeed() / (VMManager::GetTargetSpeed() * 100.0f);
			s_samples++;
			s_speed_sum += std::min(ratio, 1.5f);
			if (ratio < SLOW)
			{
				s_slow_samples++;
				s_longest = std::max(s_longest, ++s_streak);
				// Two seconds under full speed: once per stretch (at most 30 a session), what the emulator was doing,
				// for the boot log: the threads' and the graphics chip's load, and what a frame drew (the renderer's
				// counters: PRIM primitives, DRW draws, DRWC draw calls, BAR barriers, RP render passes, RB readbacks,
				// TC texture copies, TU uploads, XB pass splits, FD decompressions, DP DATE passes, RC copies of the
				// render target). This runs on the GS thread, where the counters live.
				if (s_streak == 4 && s_slow_logs < 30)
				{
					s_slow_logs++;
					SmallString stats;
					GSgetStats(stats);
					ps5px_log("slow: %.0f%% of full speed for 2 s; EE %.0f%%, GS %.0f%%, VU %.0f%%, GPU %.0f%%; a frame: %s",
						PerformanceMetrics::GetSpeed(), PerformanceMetrics::GetCPUThreadUsage(), PerformanceMetrics::GetGSThreadUsage(),
						PerformanceMetrics::GetVUThreadUsage(), PerformanceMetrics::GetGPUUsage(), stats.c_str());
					if (PS5PGS::Active())
						ps5px_log("slow: %s", PS5PGS::StatusLine().c_str());
				}
			}
			else
			{
				s_streak = 0;
			}
		}

		// The switch, every two seconds (a game's own settings can have it).
		if (!s_known || s_since_setting.GetTimeSeconds() >= 2.0)
		{
			s_since_setting.Reset();
			s_known = true;
			const bool on = Host::GetBoolSettingValue("EmuCore/GS", "PS5AutoQuality", true);
			if (!on && s_on && s_ours != 0.0f && s_ours != s_chosen && GSConfig.UpscaleMultiplier == s_ours)
			{
				// Switched off while stepped down: yours again at once.
				Set(s_chosen, false);
				s_ours = 0.0f;
			}
			s_on = on;
		}

		// The settings were applied again (or this is the first look): what is in effect now is
		// yours, and nothing is stepped.
		if (s_ours == 0.0f || GSConfig.UpscaleMultiplier != s_ours)
		{
			s_chosen = GSConfig.UpscaleMultiplier;
			s_ours = s_chosen;
			s_slow = s_room = s_hold = 0;
		}

		if (!s_on || !GSIsHardwareRenderer() || VMManager::GetLimiterMode() != LimiterModeType::Nominal)
		{
			s_slow = s_room = 0;
			return;
		}
		if (PS5PGS::Active())
		{
			const float pgs_target = VMManager::GetTargetSpeed() * 100.0f;
			if (pgs_target > 0.0f)
				PgsVSync(pgs_target, PerformanceMetrics::GetSpeed());
			return;
		}
		if (s_settle > 0)
		{
			s_settle--;
			return;
		}
		if (s_hold > 0)
			s_hold--;

		const float target = VMManager::GetTargetSpeed() * 100.0f;
		const float speed = PerformanceMetrics::GetSpeed();
		const float gpu = PerformanceMetrics::GetGPUUsage();
		const float gs = PerformanceMetrics::GetGSThreadUsage();
		if (target <= 0.0f)
			return;

		const float now = GSConfig.UpscaleMultiplier;
		if (s_no_down > 0)
			s_no_down--;

		// A step down that did not help is taken back.
		if (s_judge > 0 && --s_judge == 0 && speed < s_speed_before + HELPED && now < s_chosen)
		{
			const float back = std::min(s_chosen, std::floor(now) + 1.0f);
			ps5px_log("auto quality: %gx did not make the game faster (%.0f%%, was %.0f%%); back to %gx", now, speed, s_speed_before, back);
			Set(back, false);
			s_no_down = NO_DOWN_SAMPLES;
			return;
		}

		if (speed < target * SLOW && gpu > BUSY && s_no_down == 0)
		{
			s_room = 0;
			if (++s_slow >= DOWN_SAMPLES && now > 1.0f)
			{
				const float lower = std::max(1.0f, std::ceil(now) - 1.0f);
				s_speed_before = speed;
				Set(lower, true);
				s_judge = JUDGE_SAMPLES;
			}
			return;
		}
		s_slow = 0;

		if (now < s_chosen && speed >= target * FULL && gpu < GPU_ROOM && gs < GS_ROOM && s_hold == 0)
		{
			if (++s_room >= UP_SAMPLES)
				Set(std::min(s_chosen, std::floor(now) + 1.0f), false);
			return;
		}
		s_room = 0;
	}

	void Reset()
	{
		s_known = false;
		s_on = false;
		s_chosen = s_ours = 0.0f;
		s_slow = s_room = s_settle = s_hold = 0;
		s_pgs_chosen = s_pgs_ours = 0;
		s_pgs_slow = s_pgs_room = s_pgs_settle = s_pgs_hold = 0;
		s_judge = s_no_down = 0;
		s_speed_before = 0.0f;
	}

	void ResetSession()
	{
		Reset();
		s_down = s_up = 0;
		s_lowest = 0.0f;
		s_pgs_lowest = 0;
		s_samples = s_slow_samples = s_streak = s_longest = s_slow_logs = 0;
		s_speed_sum = 0.0;
	}

	Figures SessionFigures()
	{
		Figures f;
		f.seconds_at_normal_speed = s_samples * SAMPLE_SECONDS;
		f.average_speed = s_samples ? static_cast<float>(s_speed_sum / s_samples * 100.0) : 0.0f;
		f.seconds_slow = s_slow_samples * SAMPLE_SECONDS;
		f.longest_slow = s_longest * SAMPLE_SECONDS;
		f.steps_down = s_down;
		f.steps_up = s_up;
		f.lowest = s_lowest;
		return f;
	}

	uint32_t StepsDown() { return s_down; }
	uint32_t StepsUp() { return s_up; }
	float Lowest() { return s_lowest; }
} // namespace PS5AutoQuality
