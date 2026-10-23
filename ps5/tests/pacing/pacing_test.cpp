// Crux2 - host test of frame pacing's decisions (frontend/ps5_pacing.h) and of the start-up
// measurement of how late sleeps wake (common/PS5Timing.h), with this PC's real sleeps.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

#include "ps5_pacing.h"
#include "PS5Timing.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>

static int s_checks = 0, s_failed = 0;
#define CHECK(cond)                                                                    \
	do                                                                                 \
	{                                                                                  \
		s_checks++;                                                                    \
		if (!(cond))                                                                   \
		{                                                                              \
			s_failed++;                                                                \
			std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		}                                                                              \
	} while (0)

// The parts of the pacing header that are runtime (ps5_pacing.cpp) are not linked here.
namespace PS5Pacing
{
	std::string Summary(const FrameTimes& t);
}

static void Plans()
{
	using PS5Pacing::PlanFor;
	// A 60 Hz screen and an NTSC game: one refresh a frame, by the screen.
	{
		const auto p = PlanFor(59.94f, 59.94f, false, false, true);
		CHECK(p.can_sync && p.refreshes_per_frame == 1 && p.flip_rate == 0 && p.presents == 1 && p.by_screen);
		CHECK(std::fabs(p.ratio - 1.0f) < 1e-4f);
	}
	// 120 Hz: each frame held for two refreshes by VideoOut.
	{
		const auto p = PlanFor(119.88f, 59.94f, false, false, true);
		CHECK(p.can_sync && p.refreshes_per_frame == 2 && p.flip_rate == 1 && p.presents == 1 && !p.black_second);
	}
	// 120 Hz with frame generation's doubling: two pictures a vsync, no flip rate.
	{
		const auto p = PlanFor(119.88f, 59.94f, true, false, true);
		CHECK(p.presents == 2 && p.flip_rate == 0 && !p.black_second);
	}
	// 120 Hz with black frames: two, the second black; doubling wins over it.
	{
		const auto p = PlanFor(119.88f, 59.94f, false, true, true);
		CHECK(p.presents == 2 && p.black_second && p.flip_rate == 0);
		const auto q = PlanFor(119.88f, 59.94f, true, true, true);
		CHECK(q.presents == 2 && !q.black_second);
	}
	// Black frames at 60 Hz would flicker at 30: not done.
	{
		const auto p = PlanFor(59.94f, 59.94f, false, true, true);
		CHECK(p.presents == 1 && !p.black_second);
	}
	// A PAL game on a 60 or 120 Hz screen: the clock paces, nothing held.
	for (float host : {59.94f, 119.88f})
	{
		const auto p = PlanFor(host, 50.0f, false, true, true);
		CHECK(!p.can_sync && !p.by_screen && p.presents == 1 && p.flip_rate == 0);
	}
	// Pacing by the clock asked for: in step, but not by the screen.
	{
		const auto p = PlanFor(119.88f, 59.94f, false, false, false);
		CHECK(p.can_sync && !p.by_screen && p.flip_rate == 1);
	}
	// A game at 40 on 120: three refreshes each (flip rate 2).
	{
		const auto p = PlanFor(119.88f, 39.96f, false, false, true);
		CHECK(p.can_sync && p.refreshes_per_frame == 3 && p.flip_rate == 2);
	}
	// A game at 30 on 120 (four refreshes): beyond what VideoOut holds, the clock paces.
	{
		const auto p = PlanFor(119.88f, 29.97f, false, false, true);
		CHECK(!p.can_sync && !p.by_screen);
	}
	// No rates known: nothing.
	{
		const auto p = PlanFor(0.0f, 59.94f, false, false, true);
		CHECK(!p.can_sync && p.presents == 1 && p.flip_rate == 0);
		const auto q = PlanFor(NAN, 59.94f, false, false, true);
		CHECK(!q.can_sync);
	}
	// A screen 3% off still syncs; 6% off does not.
	CHECK(PlanFor(61.7f, 59.94f, false, false, true).can_sync);
	CHECK(!PlanFor(63.6f, 59.94f, false, false, true).can_sync);
}

static void Watch()
{
	const float guest = 59.94f;
	// Vsyncs at the game's own rate for a minute: never gives up.
	{
		PS5Pacing::Watch w;
		bool gave = false;
		const double step = 1e9 / guest;
		for (int i = 0; i < 3600; i++)
			gave |= w.Vsync(static_cast<uint64_t>(1e9 + i * step), guest);
		CHECK(!gave && !w.gave_up);
	}
	// Twice as fast (presents that do not wait): gives up after two windows, once.
	{
		PS5Pacing::Watch w;
		int gave = 0;
		const double step = 1e9 / (guest * 2);
		for (int i = 0; i < 2000; i++)
			gave += w.Vsync(static_cast<uint64_t>(1e9 + i * step), guest) ? 1 : 0;
		CHECK(gave == 1 && w.gave_up);
	}
	// A short burst (a queue catching up after a stall) between normal stretches: not enough.
	{
		PS5Pacing::Watch w;
		bool gave = false;
		double t = 1e9;
		for (int i = 0; i < 600; i++, t += 1e9 / guest)
			gave |= w.Vsync(static_cast<uint64_t>(t), guest);
		for (int i = 0; i < 40; i++, t += 1e9 / (guest * 1.5))
			gave |= w.Vsync(static_cast<uint64_t>(t), guest);
		for (int i = 0; i < 600; i++, t += 1e9 / guest)
			gave |= w.Vsync(static_cast<uint64_t>(t), guest);
		CHECK(!gave);
	}
	// Slower than the game (a heavy scene): never gives up.
	{
		PS5Pacing::Watch w;
		bool gave = false;
		for (int i = 0; i < 2000; i++)
			gave |= w.Vsync(static_cast<uint64_t>(1e9 + i * (1e9 / 40.0)), guest);
		CHECK(!gave);
	}
}

// The watch over a game behind a held flip: frames of 17-20 ms held for two refreshes of 119.88 Hz
// come at 16.7 and 25 ms in turn (48 a second); two such seconds and the game is behind; three
// seconds with a fifth of each frame to spare and it is caught up.
static void Behind()
{
	const float guest = 59.94f;
	const uint64_t frame = static_cast<uint64_t>(1e9 / guest);
	{
		PS5Pacing::Behind b;
		uint64_t t = 1000;
		unsigned changes = 0;
		bool behind_at = false;
		for (int i = 0; i < 48 * 3; i++)
		{
			t += (i % 2) ? 25000000ULL : 16683000ULL;
			if (b.Vsync(t, guest, 0))
			{
				changes++;
				behind_at = b.behind;
			}
		}
		CHECK(changes == 1 && behind_at && b.behind);
		CHECK(b.last_rate > 46.0 && b.last_rate < 50.0);
		// Paced by the clock, still slow (no wait): stays behind.
		for (int i = 0; i < 55 * 5; i++)
		{
			t += 18000000ULL;
			CHECK(!b.Vsync(t, guest, 0));
		}
		CHECK(b.behind);
		// Full speed with a little to spare (1 ms a frame): not enough, stays behind.
		for (int i = 0; i < 60 * 5; i++)
		{
			t += frame;
			CHECK(!b.Vsync(t, guest, 1000000ULL));
		}
		CHECK(b.behind);
		// Full speed with 5 ms a frame to spare for three seconds and more: caught up.
		changes = 0;
		for (int i = 0; i < 60 * 4 + 5; i++)
		{
			t += frame;
			if (b.Vsync(t, guest, 5000000ULL))
				changes++;
		}
		CHECK(changes == 1 && !b.behind);
	}
	{
		// Full speed: never behind. One slow second alone (a loading stutter): not behind.
		PS5Pacing::Behind b;
		uint64_t t = 1000;
		for (int i = 0; i < 60 * 10; i++)
		{
			t += frame;
			CHECK(!b.Vsync(t, guest, 0));
		}
		for (int i = 0; i < 40; i++)
		{
			t += 25000000ULL;
			CHECK(!b.Vsync(t, guest, 0));
		}
		for (int i = 0; i < 60 * 3; i++)
		{
			t += frame;
			CHECK(!b.Vsync(t, guest, 0));
		}
		CHECK(!b.behind);
		// 98% is not slow (the screen's rate rounds a little).
		for (int i = 0; i < 59 * 5; i++)
		{
			t += static_cast<uint64_t>(frame / 0.98);
			CHECK(!b.Vsync(t, guest, 0));
		}
		CHECK(!b.behind);
		// A restart (a pause) begins the count again, and a clock that went back does no harm.
		b.Reset();
		CHECK(!b.Vsync(500, guest, 0));
		CHECK(!b.Vsync(400, guest, 0));
	}
}

static void FrameTimes()
{
	PS5Pacing::FrameTimes t;
	for (int i = 0; i < 990; i++)
		t.Add(16.68);
	for (int i = 0; i < 10; i++)
		t.Add(33.4);
	CHECK(t.frames == 1000);
	CHECK(std::fabs(t.Percentile(0.01) - 16.65) < 0.2);   // exactly 1% at 33.4: the 1% low is the next below
	CHECK(std::fabs(t.Percentile(0.005) - 33.45) < 0.2);
	CHECK(std::fabs(t.longest_ms - 33.4) < 1e-9 && t.over_25ms == 10);
	t.Add(1000.0); // past the histogram: kept in the last bucket
	CHECK(t.counts[PS5Pacing::FrameTimes::BUCKETS - 1] == 1 && t.longest_ms == 1000.0);
	PS5Pacing::FrameTimes few;
	few.Add(16.0);
	CHECK(few.Percentile(0.01) > 15.9);
}

static void Timing()
{
	using namespace PS5Timing;
	CHECK(Percentile({}, 50) == 0);
	CHECK(Percentile({5, 1, 3}, 50) == 3);
	CHECK(Percentile({1, 2, 3, 4, 5, 6, 7, 8, 9, 10}, 90) == 9);
	CHECK(Percentile({1, 2, 3, 4, 5, 6, 7, 8, 9, 10}, 100) == 10);
	CHECK(MarginFor({}) == MARGIN_DEFAULT_NS);
	CHECK(MarginFor({0, 0, 0}) == MARGIN_MIN_NS);
	CHECK(MarginFor({50000000}) == MARGIN_MAX_NS);
	// A kernel that rounds to a 10 ms tick: the margin covers it.
	CHECK(MarginFor({9000000, 7000000, 9500000, 8000000}) == 9800000);

	// The measurement with a sleep that wakes 2 ms late, and one that fails.
	uint64_t clock = 0;
	auto now = [&clock] { return clock; };
	const Measured late = Measure([&clock](uint64_t ns) { clock += ns + 2000000; return true; }, now);
	CHECK(late.ok && late.lateness.size() == SAMPLES_PER_REQUEST * 2 && late.p90 == 2000000 && late.worst == 2000000);
	const Measured broken = Measure([](uint64_t) { return false; }, now);
	CHECK(!broken.ok);
	const Measured tight = Measure([&clock](uint64_t ns) { clock += ns + 100000; return true; }, now);
	CHECK(PreferSecond(late, tight) && !PreferSecond(tight, late));
	CHECK(!PreferSecond(late, broken) && PreferSecond(broken, late));

	// And this PC's nanosleep, for real: wakes after what was asked, and the margin is sane.
	auto real_now = [] { return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
								 std::chrono::steady_clock::now().time_since_epoch()).count()); };
	const Measured real = Measure(
		[](uint64_t ns) {
			timespec ts{static_cast<time_t>(ns / 1000000000ULL), static_cast<long>(ns % 1000000000ULL)};
			return nanosleep(&ts, nullptr) == 0;
		},
		real_now);
	CHECK(real.ok);
	const uint64_t margin = MarginFor(real.lateness);
	CHECK(margin >= MARGIN_MIN_NS && margin <= MARGIN_MAX_NS);
	std::printf("this PC: nanosleep wakes %.3f ms late typically, %.3f ms at 90%%, %.3f at worst; margin %.2f ms\n", real.p50 / 1e6,
		real.p90 / 1e6, real.worst / 1e6, margin / 1e6);
}


// The frame limiter's wait on a simulated console whose sleeps wake at the next 10 ms tick, whose
// yield (a system call) takes 20 us and whose spin step 50 ns: it ends on time, spins no more than
// the last SPIN_LAST_NS, and gives its turn away for the rest of what it waits awake.
static void Limiter()
{
	using namespace PS5Timing;
	uint64_t clock = 3000000; // 3 ms past a tick
	uint64_t yields = 0, spins = 0, spun_ns = 0;
	auto now = [&clock] { return clock; };
	auto sleep_tick = [&clock](uint64_t ns) { clock = ((clock + ns + 9999999) / 10000000) * 10000000; };
	auto yield = [&clock, &yields] { clock += 20000; yields++; };
	auto spin = [&clock, &spins, &spun_ns] { clock += 50; spins++; spun_ns += 50; };
	LimiterWaits waits;
	const uint64_t frame = 16683333;
	uint64_t end = clock + frame, worst_late = 0;
	for (int i = 0; i < 600; i++) // ten seconds of 59.94 Hz frames
	{
		WaitUntil(end, 10300000, now, sleep_tick, yield, spin, waits); // a 10 ms tick measured: up to 10 ms late, plus 0.3
		worst_late = std::max(worst_late, clock - end);
		clock += 2000000; // the frame's work
		end += frame;
	}
	const double awake_ms = waits.awake_ns / 1e6 / waits.waits;
	std::printf("frame limiter on a 10 ms tick: %.2f ms awake a frame, of it %.3f ms spinning and %.2f ms given away in %.0f yields; "
				"%.3f ms late at worst\n",
		awake_ms, spun_ns / 1e6 / waits.waits, yields * 0.02 / waits.waits, static_cast<double>(yields) / waits.waits, worst_late / 1e6);
	CHECK(waits.waits == 600);
	CHECK(worst_late <= 20000); // the end, to one yield's length
	CHECK(spun_ns / waits.waits <= SPIN_LAST_NS + 50);
	CHECK(waits.late == 0);
	// A sleep that overshoots: counted.
	LimiterWaits over;
	uint64_t c2 = 0;
	WaitUntil(5000000, 500000, [&c2] { return c2; }, [&c2](uint64_t ns) { c2 += ns + 3000000; }, [&c2] { c2 += 1000; }, [&c2] { c2 += 50; }, over);
	CHECK(over.late == 1 && over.latest_ns == 2500000 && over.awake_ns == 0);
	// Past the end already: nothing.
	LimiterWaits none;
	WaitUntil(10, 0, [] { return uint64_t(20); }, [](uint64_t) {}, [] {}, [] {}, none);
	CHECK(none.waits == 0);
}

int main()
{
	Plans();
	Watch();
	Behind();
	FrameTimes();
	Timing();
	Limiter();
	std::printf("pacing: %d checks, %d failed\n", s_checks, s_failed);
	return s_failed == 0 ? 0 : 1;
}
