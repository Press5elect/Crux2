// Crux2 - frame pacing on the console's screen: the runtime of ps5_pacing.h.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

#include "ps5_pacing.h"
#include "ps5_platform.h"
#include "ps5_clock.h"
#include "../common/PS5Timing.h"

#include "common/Timer.h"

#include <atomic>
#include <cstdio>
#include <mutex>

// RADV's VideoOut WSI (PS5_Mesa, wsi_common_videoout.c): each flip shown for at least rate + 1
// refreshes.
extern "C" int wsi_videoout_set_flip_rate(int rate);
// The most images presented and not yet shown before an acquire waits (wsi_videoout_set_queue_depth).
extern "C" int wsi_videoout_set_queue_depth(int depth);

namespace PS5Pacing
{
	namespace
	{
		std::atomic<unsigned> s_presents{1};
		std::atomic<bool> s_black_second{false};
		std::atomic<bool> s_by_screen{false};
		std::atomic<bool> s_gave_up{false};

		std::mutex s_lock; // the rest
		int s_flip_rate = 0;
		int s_queue_depth = 2; // as the swapchain is made (VKSwapChain.cpp)
		int s_said_flip_rate = -1;
		unsigned s_said_presents = 0;
		bool s_said_black = false;
		bool s_said_by_screen = false;
		bool s_said_any = false;
		Watch s_watch;
		Behind s_behind_watch;
		std::atomic<bool> s_behind{false};
		std::atomic<uint64_t> s_limiter_waited{0};
		unsigned s_times_behind = 0;

		// Shown pictures, counted on the GS thread; read on the CPU thread for the overlay.
		std::atomic<uint64_t> s_shown_window_start{0};
		std::atomic<uint32_t> s_shown_in_window{0};
		std::atomic<float> s_shown_per_second{0.0f};

		// The intervals between presents: the whole of the game played, and the profiler's window.
		std::mutex s_times_lock;
		FrameTimes s_times, s_window;
		uint64_t s_last_present = 0;
	} // namespace

	void Apply(const Plan& plan, float host, float guest)
	{
		std::lock_guard<std::mutex> lock(s_lock);
		if (plan.flip_rate != s_flip_rate)
		{
			const int r = wsi_videoout_set_flip_rate(plan.flip_rate);
			if (r != 0)
				ps5px_log("pacing: VideoOut did not take a flip rate of %d (%d)", plan.flip_rate, r);
			s_flip_rate = plan.flip_rate;
		}
		// Two images waiting at most while the game keeps up (the controls followed closely), three
		// while it is behind (a frame waiting for its vblank does not hold the next one back).
		// Behind: four of the five images may wait (VKSwapChain.cpp), so the GS thread does not wait
		// in the acquire for frames the graphics chip or the screen has not taken yet.
		const int depth = s_behind.load(std::memory_order_relaxed) ? 4 : 2;
		if (depth != s_queue_depth)
		{
			if (wsi_videoout_set_queue_depth(depth) != 0)
				ps5px_log("pacing: VideoOut did not take a queue depth of %d", depth);
			s_queue_depth = depth;
		}
		s_presents.store(plan.presents, std::memory_order_relaxed);
		s_black_second.store(plan.black_second, std::memory_order_relaxed);
		s_by_screen.store(plan.by_screen, std::memory_order_relaxed);
		if (!plan.by_screen)
			s_watch.Reset();

		// (A game in and out of heavy scenes changes plans often: the first few changes are said.)
		if (s_times_behind <= 5 && (!s_said_any || s_said_flip_rate != plan.flip_rate || s_said_presents != plan.presents ||
			s_said_black != plan.black_second || s_said_by_screen != plan.by_screen))
		{
			s_said_any = true;
			s_said_flip_rate = plan.flip_rate;
			s_said_presents = plan.presents;
			s_said_black = plan.black_second;
			s_said_by_screen = plan.by_screen;
			const char* how = !plan.can_sync ? "the screen's rate does not fit the game's" :
							  plan.black_second              ? "each frame shown, then black" :
							  plan.presents == 2             ? "a picture made between each two frames" :
							  plan.flip_rate > 0             ? "each frame held for more than one refresh" :
															   "one refresh for each frame";
			ps5px_log("pacing: game %.3f Hz on a %.3f Hz screen: %s; paced by the %s", guest, host, how,
				plan.by_screen ? "screen" : "clock");
		}
	}

	unsigned PresentsPerVsync()
	{
		return s_presents.load(std::memory_order_relaxed);
	}

	bool SecondIsBlack()
	{
		return s_black_second.load(std::memory_order_relaxed);
	}

	bool ByScreen()
	{
		return s_by_screen.load(std::memory_order_relaxed);
	}

	bool NoteVsyncByScreen(uint64_t now_ns, float guest)
	{
		std::lock_guard<std::mutex> lock(s_lock);
		if (!s_watch.Vsync(now_ns, guest))
			return false;
		s_gave_up.store(true, std::memory_order_relaxed);
		ps5px_log("pacing: the game ran faster than the screen while paced by it; paced by the clock from now on");
		return true;
	}

	void RestartWatch()
	{
		std::lock_guard<std::mutex> lock(s_lock);
		const bool gave_up = s_watch.gave_up;
		s_watch.Reset();
		s_watch.gave_up = gave_up;
		s_behind_watch.Reset();
		s_limiter_waited.store(0, std::memory_order_relaxed);
	}

	bool ScreenGaveUp()
	{
		return s_gave_up.load(std::memory_order_relaxed);
	}

	void NoteLimiterWait(uint64_t ns)
	{
		s_limiter_waited.fetch_add(ns, std::memory_order_relaxed);
	}

	bool NoteVsyncBehind(uint64_t now_ns, float guest)
	{
		const uint64_t waited = s_limiter_waited.exchange(0, std::memory_order_relaxed);
		std::lock_guard<std::mutex> lock(s_lock);
		if (!s_behind_watch.Vsync(now_ns, guest, waited))
			return false;
		const bool behind = s_behind_watch.behind;
		s_behind.store(behind, std::memory_order_relaxed);
		if (behind)
		{
			s_times_behind++;
			// The first few times in the log; a game in and out of heavy scenes would fill it.
			if (s_times_behind <= 5)
				ps5px_log("pacing: the game fell behind the screen (%.1f of %.2f frames a second); each frame now shows at the next "
						  "refresh, paced by the clock, with a frame more allowed to wait for the screen",
					s_behind_watch.last_rate, guest);
		}
		else if (s_times_behind <= 5)
		{
			ps5px_log("pacing: the game has time to spare again; frames held for the screen as before");
		}
		return true;
	}

	bool IsBehind()
	{
		return s_behind.load(std::memory_order_relaxed);
	}

	std::string Summary(const FrameTimes& t)
	{
		if (t.frames < 60)
			return std::string();
		char line[256];
		const double avg = t.total_ms / static_cast<double>(t.frames);
		const double p1 = t.Percentile(0.01), p01 = t.Percentile(0.001);
		std::snprintf(line, sizeof(line),
			"%llu presents, %.2f ms apart on average (%.1f a second); 1%% low %.1f a second (%.2f ms), 0.1%% low %.1f (%.2f ms); longest %.1f ms; "
			"%u later than 25 ms",
			static_cast<unsigned long long>(t.frames), avg, 1000.0 / avg, p1 > 0 ? 1000.0 / p1 : 0.0, p1, p01 > 0 ? 1000.0 / p01 : 0.0, p01,
			t.longest_ms, t.over_25ms);
		return line;
	}

	std::string FrameTimesLine()
	{
		const std::lock_guard<std::mutex> lock(s_times_lock);
		return Summary(s_times);
	}

	std::string TakeWindow()
	{
		const std::lock_guard<std::mutex> lock(s_times_lock);
		std::string line = Summary(s_window);
		s_window = FrameTimes();
		return line;
	}

	void Stop()
	{
		{
			// What the game played had: in the log once, as it is closed.
			const std::string line = FrameTimesLine();
			if (!line.empty())
				ps5px_log("pacing: %s", line.c_str());
			char clock[256];
			ps5px_clock_line(clock, sizeof(clock));
			if (clock[0])
				ps5px_log("%s", clock);
			char limiter[320];
			PS5Timing::TakeLimiterLine(limiter, sizeof(limiter));
			if (limiter[0])
				ps5px_log("pacing: %s", limiter);
			const std::lock_guard<std::mutex> lock(s_times_lock);
			s_times = FrameTimes();
			s_window = FrameTimes();
			s_last_present = 0;
		}
		std::lock_guard<std::mutex> lock(s_lock);
		if (s_flip_rate != 0)
		{
			wsi_videoout_set_flip_rate(0);
			s_flip_rate = 0;
		}
		if (s_queue_depth != 2)
		{
			wsi_videoout_set_queue_depth(2);
			s_queue_depth = 2;
		}
		s_presents.store(1, std::memory_order_relaxed);
		s_black_second.store(false, std::memory_order_relaxed);
		s_by_screen.store(false, std::memory_order_relaxed);
		// The next game starts paced by the screen again: giving up on it was this game's.
		s_watch = Watch();
		s_gave_up.store(false, std::memory_order_relaxed);
		if (s_times_behind != 0)
			ps5px_log("pacing: the game fell behind the screen %u time(s) in all", s_times_behind);
		s_behind_watch = Behind();
		s_behind.store(false, std::memory_order_relaxed);
		s_limiter_waited.store(0, std::memory_order_relaxed);
		s_times_behind = 0;
		s_said_any = false;
		s_shown_window_start.store(0, std::memory_order_relaxed);
		s_shown_in_window.store(0, std::memory_order_relaxed);
		s_shown_per_second.store(0.0f, std::memory_order_relaxed);
	}

	void NoteShown(bool distinct)
	{
		const uint64_t now = Common::Timer::GetCurrentValue();
		{
			const std::lock_guard<std::mutex> lock(s_times_lock);
			if (s_last_present != 0)
			{
				const double ms = Common::Timer::ConvertValueToMilliseconds(now - s_last_present);
				s_times.Add(ms);
				s_window.Add(ms);
			}
			s_last_present = now;
		}
		const uint64_t start = s_shown_window_start.load(std::memory_order_relaxed);
		if (start == 0)
		{
			s_shown_window_start.store(now, std::memory_order_relaxed);
			s_shown_in_window.store(0, std::memory_order_relaxed);
			return;
		}
		if (distinct)
			s_shown_in_window.fetch_add(1, std::memory_order_relaxed);
		const double seconds = Common::Timer::ConvertValueToSeconds(now - start);
		if (seconds >= 1.0)
		{
			s_shown_per_second.store(static_cast<float>(s_shown_in_window.load(std::memory_order_relaxed) / seconds),
				std::memory_order_relaxed);
			s_shown_window_start.store(now, std::memory_order_relaxed);
			s_shown_in_window.store(0, std::memory_order_relaxed);
		}
	}

	float ShownPerSecond()
	{
		return s_shown_per_second.load(std::memory_order_relaxed);
	}
} // namespace PS5Pacing
