// Crux2 - host test of frame generation's Steady 30 (frontend/ps5_framegen_pacer.h): a game's
// pictures taken two vsyncs apart at the most often, so a game going between 30 and 60 a second
// is shown at an even 30 that frame generation fills to 60 (or 120); off, nothing changes.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
// From Porpoise 14fdbae (GPL-3.0-or-later): the behaviour tested here.

#include "ps5_framegen_pacer.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static int s_checks = 0, s_failed = 0;
#define CHECK(cond)                                                                \
	do                                                                             \
	{                                                                              \
		s_checks++;                                                                \
		if (!(cond))                                                               \
		{                                                                          \
			s_failed++;                                                            \
			std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		}                                                                          \
	} while (0)

struct Vsync
{
	bool taken;      // a new picture handed to frame generation at this vsync
	int game;        // the game's newest picture then (what is handed over when taken)
	uint32_t period; // the rate the pacer is at
	float time[2];   // Time() of the pictures shown for the vsync (1: the newest taken itself)
};

// `pattern`: one letter a vsync, N a new picture from the game, - the same again.
static std::vector<Vsync> Run(const std::string& pattern, bool steady30, uint32_t presents)
{
	PS5FrameGen::Pacer pacer;
	pacer.steady30 = steady30;
	std::vector<Vsync> out;
	int game = -1;
	for (const char c : pattern)
	{
		const bool fresh = (c == 'N');
		game += fresh ? 1 : 0;
		Vsync v{};
		v.taken = pacer.Vsync(fresh);
		v.game = game;
		v.period = pacer.period;
		for (uint32_t shown = 0; shown < presents; shown++)
			v.time[shown] = pacer.Makes(presents) ? pacer.Time(shown, presents) : 1.0f;
		out.push_back(v);
	}
	return out;
}

static std::string Repeat(const std::string& part, int times)
{
	std::string out;
	for (int i = 0; i < times; i++)
		out += part;
	return out;
}

// The vsyncs between the pictures taken, from the `from`-th vsync on.
static std::vector<int> Gaps(const std::vector<Vsync>& run, size_t from)
{
	std::vector<int> gaps;
	int last = -1;
	for (size_t i = from; i < run.size(); i++)
	{
		if (!run[i].taken)
			continue;
		if (last >= 0)
			gaps.push_back(static_cast<int>(i) - last);
		last = static_cast<int>(i);
	}
	return gaps;
}

int main()
{
	// Off: every new picture is taken when it comes, whatever the pattern.
	for (const std::string& pattern : {Repeat("N", 20), Repeat("N-", 10), std::string("NN-NNN-N-NN--NNN-N")})
	{
		const std::vector<Vsync> run = Run(pattern, false, 1);
		for (size_t i = 0; i < run.size(); i++)
			CHECK(run[i].taken == (pattern[i] == 'N'));
	}

	// A game swinging between 60 and 30 a second (and in between): taken every second vsync, the
	// newest picture each time, the rate settles on 2, and on a 60 Hz screen a made picture at
	// the half and the game's own take turns.
	{
		const std::string pattern = Repeat("N", 10) + Repeat("N-", 6) + std::string("NN-NNN-N-NN-N") + Repeat("N", 9) + Repeat("N-", 6);
		const std::vector<Vsync> run = Run(pattern, true, 1);
		CHECK(run[0].taken);
		for (const int gap : Gaps(run, 0))
			CHECK(gap == 2);
		for (size_t i = 6; i < run.size(); i++)
			CHECK(run[i].period == 2);
		for (size_t i = 6; i < run.size(); i++)
			CHECK(run[i].time[0] == (run[i].taken ? 0.5f : 1.0f));
		// What is handed over is always the newest the game has (the caller pushes its current one).
		int handed = -1;
		for (const Vsync& v : run)
		{
			if (!v.taken)
				continue;
			CHECK(v.game > handed);
			handed = v.game;
		}
		// The same game without Steady 30: the rate goes between 1 and 2.
		const std::vector<Vsync> off = Run(pattern, false, 1);
		bool one = false, two = false;
		for (size_t i = 4; i < off.size(); i++)
		{
			one = one || off[i].period == 1;
			two = two || off[i].period == 2;
		}
		CHECK(one && two);
	}

	// On a 120 Hz screen: quarters, three made between each two taken.
	{
		const std::vector<Vsync> run = Run(Repeat("N", 30), true, 2);
		for (size_t i = 8; i + 1 < run.size(); i += 2)
		{
			const bool first = run[i].taken;
			CHECK(run[i].time[0] == (first ? 0.25f : 0.75f));
			CHECK(run[i].time[1] == (first ? 0.5f : 1.0f));
		}
	}

	// A picture held back is taken at the next vsync even when no newer one came.
	{
		const std::vector<Vsync> run = Run("N-NN--", true, 1);
		CHECK(run[0].taken && !run[1].taken && run[2].taken && !run[3].taken && run[4].taken && !run[5].taken);
	}

	// A game at 20 a second, or below, is taken as it comes.
	{
		const std::string pattern = Repeat("N--", 10);
		const std::vector<Vsync> run = Run(pattern, true, 1);
		for (size_t i = 0; i < run.size(); i++)
			CHECK(run[i].taken == (pattern[i] == 'N'));
		CHECK(run.back().period == 3);
	}

	// After a standstill (a loading screen), the next picture is taken at once.
	{
		const std::vector<Vsync> run = Run("N-N-" + std::string(20, '-') + "N-N-", true, 1);
		CHECK(run[24].taken && run[24].time[0] == 1.0f);
	}

	// Reset forgets a held picture; the first after it is taken at once.
	{
		PS5FrameGen::Pacer pacer;
		pacer.steady30 = true;
		CHECK(pacer.Vsync(true));
		CHECK(!pacer.Vsync(true));
		pacer.Reset();
		CHECK(!pacer.held);
		CHECK(pacer.Vsync(true));
	}

	// Whatever the game does, a time is above 0 and at most 1, and the pictures taken are never
	// closer than two vsyncs.
	for (const uint32_t presents : {1u, 2u})
	{
		for (const std::string& pattern : {std::string("N-N--N-N---N-NN-N--N-NNN--N-N-"), Repeat("NN-", 10), Repeat("N", 7) + std::string(9, '-') + Repeat("NN--", 5)})
		{
			const std::vector<Vsync> run = Run(pattern, true, presents);
			for (const Vsync& v : run)
				for (uint32_t s = 0; s < presents; s++)
					CHECK(v.time[s] > 0.0f && v.time[s] <= 1.0f);
			for (const int gap : Gaps(run, 0))
				CHECK(gap >= 2);
		}
	}

	std::printf("frame generation's Steady 30: %d checks, %d failed\n", s_checks, s_failed);
	return s_failed ? 1 : 0;
}
