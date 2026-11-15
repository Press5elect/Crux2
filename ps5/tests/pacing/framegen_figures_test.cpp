// Crux2 - host test of frame generation's figures (frontend/ps5_pacing.h, FrameGenFigures): the
// pictures a second the screen is given and how many of them are made, each half second, and the
// boot log's line every 10 s.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

#include "ps5_pacing.h"

#include <cmath>
#include <cstdio>

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

// A game at `game_rate` on a screen at `screen_rate`, frame generation filling the rest, for `seconds`.
// Returns the log lines that came due.
static int Run(PS5Pacing::FrameGenFigures& f, uint64_t& now, double screen_rate, int per_game, double seconds,
	PS5Pacing::FrameGenFigures::Line* last)
{
	const uint64_t step = static_cast<uint64_t>(1e9 / screen_rate);
	const uint64_t presents = static_cast<uint64_t>(seconds * screen_rate);
	int lines = 0;
	for (uint64_t i = 0; i < presents; i++)
	{
		const bool own = (i % static_cast<uint64_t>(per_game)) == 0;
		if (own)
			f.Game();
		PS5Pacing::FrameGenFigures::Line line;
		if (f.Shown(now, !own, line))
		{
			lines++;
			if (last)
				*last = line;
		}
		now += step;
	}
	return lines;
}

int main()
{
	using F = PS5Pacing::FrameGenFigures;
	// A game at 30 on a 60 Hz screen: 60 given, 30 made, after the first half second.
	{
		F f;
		uint64_t now = 1000000000ULL;
		F::Line line;
		CHECK(Run(f, now, 60.0, 2, 0.4, &line) == 0);
		CHECK(f.shown_rate == 0.0f); // not half a second yet
		Run(f, now, 60.0, 2, 0.3, &line);
		CHECK(std::fabs(f.shown_rate - 60.0f) < 1.5f);
		CHECK(std::fabs(f.made_rate - 30.0f) < 1.5f);
		// A line every 10 s, with the window's own counts.
		const int lines = Run(f, now, 60.0, 2, 20.0, &line);
		CHECK(lines == 2);
		CHECK(line.seconds >= 10.0 && line.seconds < 10.1);
		CHECK(line.shown >= 599 && line.shown <= 601);
		CHECK(line.made >= 299 && line.made <= 301);
		CHECK(line.own >= 299 && line.own <= 301);
	}
	// A game at 60 doubled on a 120 Hz screen: 120 given, 60 made.
	{
		F f;
		uint64_t now = 5;
		Run(f, now, 120.0, 2, 2.0, nullptr);
		CHECK(std::fabs(f.shown_rate - 120.0f) < 2.5f);
		CHECK(std::fabs(f.made_rate - 60.0f) < 2.5f);
	}
	// A game at 20 on a 60 Hz screen: two of each three made.
	{
		F f;
		uint64_t now = 5;
		Run(f, now, 60.0, 3, 2.0, nullptr);
		CHECK(std::fabs(f.made_rate - 40.0f) < 1.5f);
	}
	// Switched off: nothing counted, and the windows start again when it comes back.
	{
		F f;
		uint64_t now = 5;
		Run(f, now, 60.0, 2, 9.0, nullptr);
		f.Off();
		CHECK(f.shown_rate == 0.0f && f.made_rate == 0.0f && f.shown == 0 && f.rate_start == 0);
		now += 60ULL * 1000000000ULL; // a minute without it
		F::Line line;
		CHECK(Run(f, now, 60.0, 2, 9.0, &line) == 0); // no line for the minute it was off
		CHECK(Run(f, now, 60.0, 2, 2.0, &line) == 1);
		CHECK(line.seconds < 10.1);
	}
	std::printf("frame generation figures: %d checks, %d failed\n", s_checks, s_failed);
	return s_failed == 0 ? 0 : 1;
}
