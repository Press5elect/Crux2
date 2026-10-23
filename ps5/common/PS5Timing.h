// Crux2 - how late the console's sleeps wake, measured once at start, and the margin the frame
// limiter keeps for it.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The frame limiter sleeps for most of what is left of a frame and waits out the rest
// (Threading::SleepUntil, PS5Misc.cpp). How much "the rest" must be depends on how late the
// kernel wakes a sleeping thread: PSWin measured short timed waits on this console rounded up
// to a tick of about 10 ms, which a fixed 1.5 ms margin does not cover, and a frame that wakes
// 8 ms late is a hitch. So at start a background thread asks for short sleeps with each of the
// two calls the console has (nanosleep and sceKernelUsleep), notes how late each woke, takes
// the call that woke closer to time, and sets the margin to its 90th percentile of lateness
// plus 0.3 ms, between 0.5 and 12 ms.
//
// The measuring is here, header-only, so ps5/tests/timing runs it on the PC with real sleeps.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace PS5Timing
{
	inline constexpr uint64_t MARGIN_MIN_NS = 500000;
	inline constexpr uint64_t MARGIN_MAX_NS = 12000000;
	inline constexpr uint64_t MARGIN_EXTRA_NS = 300000;
	inline constexpr uint64_t MARGIN_DEFAULT_NS = 1500000;
	inline constexpr unsigned SAMPLES_PER_REQUEST = 12;
	inline constexpr uint64_t REQUESTS_NS[] = {1000000, 3000000};

	// The p-th percentile (0..100) of the values, nearest rank; 0 for none.
	inline uint64_t Percentile(std::vector<uint64_t> values, unsigned p)
	{
		if (values.empty())
			return 0;
		std::sort(values.begin(), values.end());
		size_t rank = (values.size() * p + 99) / 100;
		if (rank == 0)
			rank = 1;
		return values[std::min(rank, values.size()) - 1];
	}

	inline uint64_t MarginFor(const std::vector<uint64_t>& lateness)
	{
		if (lateness.empty())
			return MARGIN_DEFAULT_NS;
		const uint64_t m = Percentile(lateness, 90) + MARGIN_EXTRA_NS;
		return std::clamp(m, MARGIN_MIN_NS, MARGIN_MAX_NS);
	}

	struct Measured
	{
		std::vector<uint64_t> lateness; // how late each sleep woke, ns
		uint64_t p50 = 0, p90 = 0, worst = 0;
		bool ok = false; // every sleep returned
	};

	// sleep(ns) sleeps and returns false if the call failed; now() is a nanosecond clock.
	template <typename SleepFn, typename NowFn>
	Measured Measure(SleepFn sleep, NowFn now)
	{
		Measured m;
		m.ok = true;
		for (const uint64_t request : REQUESTS_NS)
		{
			for (unsigned i = 0; i < SAMPLES_PER_REQUEST; i++)
			{
				const uint64_t from = now();
				if (!sleep(request))
				{
					m.ok = false;
					return m;
				}
				const uint64_t took = now() - from;
				m.lateness.push_back(took > request ? took - request : 0);
			}
		}
		m.p50 = Percentile(m.lateness, 50);
		m.p90 = Percentile(m.lateness, 90);
		m.worst = Percentile(m.lateness, 100);
		return m;
	}

	// Which of two measured calls to sleep with: the one whose 90th percentile is lower, the
	// first on a tie (within 0.1 ms) or when the second failed.
	inline bool PreferSecond(const Measured& first, const Measured& second)
	{
		if (!second.ok)
			return false;
		if (!first.ok)
			return true;
		return second.p90 + 100000 < first.p90;
	}

	// The frame limiter's last stretch (Threading::SleepUntil): while more than this is left it
	// gives its turn away (sched_yield) rather than spin, and spins only the end. PSWin's build 17
	// spun the last 11 ms of every wait and took 2.3 more cores in one game for no frame more;
	// its build 18 keeps only the short end. The other threads (the GS and VU threads, the
	// driver's) get the core while the limiter waits.
	inline constexpr uint64_t SPIN_LAST_NS = 250000;

	// What the frame limiter's waits cost: asleep, awake, and how often a sleep woke past the
	// end it was meant for. Summary() is one line for the boot log ("" with no waits).
	struct LimiterWaits
	{
		uint64_t waits = 0;
		uint64_t asleep_ns = 0;
		uint64_t awake_ns = 0;
		uint64_t late = 0; // waits whose sleep woke after the end
		uint64_t latest_ns = 0; // the most a sleep overshot

		void Add(uint64_t asleep, uint64_t awake, uint64_t overshot)
		{
			waits++;
			asleep_ns += asleep;
			awake_ns += awake;
			if (overshot > 0)
			{
				late++;
				latest_ns = overshot > latest_ns ? overshot : latest_ns;
			}
		}
	};

	// The frame limiter's wait until `end` (Threading::SleepUntil): sleep for what is left less
	// the margin, then wait out the rest awake, yielding while more than SPIN_LAST_NS is left and
	// spinning the end. Returns what it cost, added to `waits`. Header-only, with the clock and
	// the three ways of waiting given, so ps5/tests/pacing drives it with a simulated console.
	template <typename Now, typename Sleep, typename Yield, typename Spin>
	inline void WaitUntil(uint64_t end, uint64_t margin, Now now, Sleep sleep, Yield yield, Spin spin, LimiterWaits& waits)
	{
		const uint64_t start = now();
		if (end <= start)
			return;
		if (end - start > margin)
			sleep(end - start - margin);
		const uint64_t woke = now();
		for (;;)
		{
			const uint64_t t = now();
			if (t >= end)
				break;
			if (end - t > SPIN_LAST_NS)
				yield();
			else
				spin();
		}
		const uint64_t awake_until = woke > end ? woke : end;
		waits.Add(woke - start, awake_until - woke, woke > end ? woke - end : 0);
	}

	// Runtime, PS5Misc.cpp: the margin in use (MARGIN_DEFAULT_NS until measured), whether
	// sceKernelUsleep was taken, and the probe, started once on a thread of its own.
	uint64_t SpinMarginNs();
	bool SleepsWithUsleep();
	void StartProbe();
	// Where the probe's one line goes (the boot log); Console otherwise. Set before StartProbe.
	void SetReport(void (*report)(const char*));
	// The frame limiter's waits since the last call, as one line ("" with none), and cleared.
	void TakeLimiterLine(char* line, size_t size);
} // namespace PS5Timing
