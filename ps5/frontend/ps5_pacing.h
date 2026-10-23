// Crux2 - how a game's frames are paced on the console's screen.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// A game shows a frame at each of its vsyncs, 59.94 a second for most. The screen refreshes at
// 59.94 or 119.88 Hz. For each frame to stay on the screen for the same time:
//
// - the emulator waits for the screen rather than for a clock where the two rates agree
//   (PCSX2's "use vsync for timing"): the swapchain has one picture waiting at most, so a
//   present waits for the refresh, and the game cannot drift against the screen;
// - at 119.88 Hz, VideoOut is asked to show each flip for two refreshes (the driver's
//   wsi_videoout_set_flip_rate), unless two pictures are given for each of the game's frames:
//   one made between frames (frame generation, "double") or one black (black frame insertion);
// - where the rates do not agree (a 50 Hz game, fast forward), the clock paces, as before.
//
// PlanFor is the decision, header-only for ps5/tests/pacing. The runtime (ps5_pacing.cpp) applies
// it from VMManager::UpdateTargetSpeed, tells the GS thread how many pictures a vsync is given,
// keeps a watch that the screen does hold the game back (a game that runs fast while paced by
// the screen goes back to the clock), and counts the pictures shown for the performance overlay.

#pragma once

#include <cmath>
#include <cstdint>
#include <string>

namespace PS5Pacing
{
	struct Plan
	{
		unsigned refreshes_per_frame = 1; // the screen's refreshes for each of the game's vsyncs
		float ratio = 1.0f; // the screen's rate over the game's, after the refreshes per frame
		bool can_sync = false; // the rates agree within 5%
		int flip_rate = 0; // VideoOut: each flip shown for at least flip_rate + 1 refreshes
		unsigned presents = 1; // pictures given for each of the game's vsyncs
		bool black_second = false; // the second of two is black (black frame insertion)
		bool by_screen = false; // the emulator waits for the screen rather than for a clock
	};

	// host: the screen's refresh rate; guest: the game's vsyncs a second. framegen_double:
	// frame generation is set to make a picture between each two at a screen of twice the
	// game's rate; black_frames: black frame insertion is on; by_screen: "Frame pacing" is "by
	// the screen" (and the screen-paced watch has not given up on it).
	inline Plan PlanFor(float host, float guest, bool framegen_double, bool black_frames, bool by_screen)
	{
		Plan p;
		if (!(host > 1.0f) || !(guest > 1.0f))
			return p;
		const float whole = std::round(host / guest);
		const unsigned rpf = whole < 1.0f ? 1u : static_cast<unsigned>(whole);
		const float ratio = host / (guest * static_cast<float>(rpf));
		if (!(ratio >= 0.95f && ratio <= 1.05f) || rpf > 3)
		{
			p.ratio = host / guest;
			return p;
		}
		p.refreshes_per_frame = rpf;
		p.ratio = ratio;
		p.can_sync = true;
		if (rpf == 2 && framegen_double)
			p.presents = 2;
		else if (rpf == 2 && black_frames)
		{
			p.presents = 2;
			p.black_second = true;
		}
		else
			p.flip_rate = static_cast<int>(rpf) - 1;
		p.by_screen = by_screen;
		return p;
	}

	// The watch over pacing by the screen: fed the time of each of the game's vsyncs while the
	// screen paces, it gives up when two windows of two seconds in a row ran more than 4% fast,
	// which happens only if presents do not wait for the screen.
	struct Watch
	{
		static constexpr uint64_t WINDOW_NS = 2000000000ULL;
		uint64_t window_start = 0;
		unsigned vsyncs = 0;
		unsigned fast_windows = 0;
		bool gave_up = false;

		void Reset()
		{
			window_start = 0;
			vsyncs = 0;
			fast_windows = 0;
		}

		// Returns true the one time it gives up.
		bool Vsync(uint64_t now_ns, float guest)
		{
			if (gave_up)
				return false;
			if (window_start == 0)
			{
				window_start = now_ns;
				vsyncs = 0;
				return false;
			}
			vsyncs++;
			const uint64_t elapsed = now_ns - window_start;
			if (elapsed < WINDOW_NS)
				return false;
			const double rate = static_cast<double>(vsyncs) * 1e9 / static_cast<double>(elapsed);
			fast_windows = (rate > guest * 1.04) ? fast_windows + 1 : 0;
			window_start = now_ns;
			vsyncs = 0;
			if (fast_windows >= 2)
			{
				gave_up = true;
				return true;
			}
			return false;
		}
	};

	// The watch over a game that cannot keep up with the screen. At 119.88 Hz each of a 59.94 Hz
	// game's frames is held for two refreshes; a frame that takes a little more than 16.7 ms then
	// waits for a third (25 ms), and the emulator, waiting for the screen, loses the difference:
	// frames of 17-20 ms come out at 16.7 and 25 ms in turn, 48 a second, 80% (Black at 6x
	// resolution, 2026-10-21). At either rate, with two frames allowed to wait for the screen, the
	// image for the frame after a late one is freed only when the late one shows: 45-55 frames a
	// second where the GPU's 15 ms a frame had time for 60 (Black at 59.94 Hz, 2026-10-21). While
	// the game is behind, the clock paces, a flip shows at the next refresh and a third frame may
	// wait, so the game runs as fast as its frames are made; it all comes back once frames are made
	// with time to spare.
	//
	// Fed the time of each of the game's vsyncs and how long the frame limiter waited in that
	// frame (nothing while the screen paces, as it does not wait then).
	struct Behind
	{
		static constexpr uint64_t WINDOW_NS = 1000000000ULL;
		static constexpr double SLOW = 0.97; // a window below 97% of the game's rate is slow
		static constexpr unsigned SLOW_WINDOWS = 2; // two slow seconds in a row: behind
		static constexpr double SPARE = 0.2; // the limiter waited a fifth of each frame: time to spare
		static constexpr unsigned SPARE_WINDOWS = 3; // three such seconds in a row: caught up
		uint64_t window_start = 0;
		unsigned vsyncs = 0;
		uint64_t waited_ns = 0;
		unsigned slow_windows = 0;
		unsigned spare_windows = 0;
		bool behind = false;
		double last_rate = 0; // the vsyncs a second of the window that decided, for the log

		void Reset()
		{
			window_start = 0;
			vsyncs = 0;
			waited_ns = 0;
			slow_windows = 0;
			spare_windows = 0;
		}
		// Returns true when `behind` changed.
		bool Vsync(uint64_t now_ns, float guest, uint64_t waited)
		{
			if (window_start == 0 || now_ns < window_start)
			{
				window_start = now_ns;
				vsyncs = 0;
				waited_ns = 0;
				return false;
			}
			vsyncs++;
			waited_ns += waited;
			const uint64_t elapsed = now_ns - window_start;
			if (elapsed < WINDOW_NS)
				return false;
			const double rate = static_cast<double>(vsyncs) * 1e9 / static_cast<double>(elapsed);
			const double frame_ns = 1e9 / static_cast<double>(guest);
			const double wait_per_frame = static_cast<double>(waited_ns) / static_cast<double>(vsyncs);
			window_start = now_ns;
			vsyncs = 0;
			waited_ns = 0;
			last_rate = rate;
			if (!behind)
			{
				slow_windows = (rate < guest * SLOW) ? slow_windows + 1 : 0;
				if (slow_windows < SLOW_WINDOWS)
					return false;
				behind = true;
				slow_windows = 0;
				spare_windows = 0;
				return true;
			}
			spare_windows = (rate >= guest * 0.99 && wait_per_frame >= frame_ns * SPARE) ? spare_windows + 1 : 0;
			if (spare_windows < SPARE_WINDOWS)
				return false;
			behind = false;
			slow_windows = 0;
			spare_windows = 0;
			return true;
		}
	};

	// Runtime (ps5_pacing.cpp).
	// Applies a plan: the flip rate to VideoOut and the presents to the GS thread; logs changes.
	void Apply(const Plan& plan, float host, float guest);
	// What the GS thread gives each vsync (1 or 2) and whether the second is black.
	unsigned PresentsPerVsync();
	bool SecondIsBlack();
	bool ByScreen();
	// Called by the frame limiter at each of the game's vsyncs while the screen paces; true when
	// pacing must go back to the clock (the target speed is worked out again).
	// Starts the watch's count again (a state loaded, a pause ended); what it decided stays.
	void RestartWatch();
	bool NoteVsyncByScreen(uint64_t now_ns, float guest);
	// Whether the screen-paced watch gave up (for this run of the title).
	bool ScreenGaveUp();
	// The watch over falling behind a held flip (Behind): the frame limiter adds what it waits,
	// and at each of the game's vsyncs, while a flip is held for more than one refresh or the game
	// is behind, asks whether that changed (then the target speed is worked out again).
	void NoteLimiterWait(uint64_t ns);
	bool NoteVsyncBehind(uint64_t now_ns, float guest);
	bool IsBehind();
	// The VM stopped: flips shown for one refresh again, counters cleared.
	void Stop();
	// The pictures shown: distinct ones (a game's frame or a made one) and repeats.
	void NoteShown(bool distinct);
	// Distinct pictures shown a second, over the last second; 0 when none were counted.
	float ShownPerSecond();

	// How evenly pictures reached the screen: the time between each two presents, kept as a
	// histogram (0.1 ms steps up to 250 ms). Summary() says it as one line ("" with too few);
	// TakeWindow() the same for what came since it was last asked (the profiler's windows).
	struct FrameTimes
	{
		static constexpr unsigned BUCKETS = 2500;
		uint32_t counts[BUCKETS] = {};
		uint64_t frames = 0;
		double total_ms = 0, longest_ms = 0;
		unsigned over_25ms = 0; // presents that came more than 25 ms after the one before

		void Add(double ms)
		{
			const unsigned b = ms <= 0 ? 0u : (ms >= BUCKETS / 10.0 ? BUCKETS - 1 : static_cast<unsigned>(ms * 10.0));
			counts[b]++;
			frames++;
			total_ms += ms;
			longest_ms = ms > longest_ms ? ms : longest_ms;
			over_25ms += (ms > 25.0) ? 1u : 0u;
		}
		// The interval below which all but `share` of the presents came (the 1% and 0.1% lows).
		double Percentile(double share) const
		{
			const uint64_t beyond = static_cast<uint64_t>(static_cast<double>(frames) * share);
			uint64_t seen = 0;
			for (unsigned b = BUCKETS; b-- > 0;)
			{
				seen += counts[b];
				if (seen > beyond)
					return (b + 0.5) / 10.0;
			}
			return 0.0;
		}
	};
	std::string Summary(const FrameTimes& t);
	std::string FrameTimesLine();
	std::string TakeWindow();
} // namespace PS5Pacing
