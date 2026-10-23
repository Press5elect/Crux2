// Crux2 - frame generation's pacing (ps5_framegen_pacer.h), on the PC.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Games as they hand over pictures (a new one every vsync, every second, every third, changing
// between those, standing still for a while), on a screen that refreshes once and twice for a
// vsync: what is put on the screen at each refresh has to be the newest picture at the end of
// its stay, pictures at evenly spaced times before that, and never a time that goes backwards.
#include "ps5_framegen_pacer.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static int s_checks = 0, s_failed = 0;
#define CHECK(what) \
	do \
	{ \
		s_checks++; \
		if (!(what)) \
		{ \
			s_failed++; \
			std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #what); \
		} \
	} while (0)

// What the screen shows over a run: for each refresh, the number of the newest picture then and
// the time between the one before it and it (1: the picture itself).
struct Shown
{
	int picture;
	float time;
};

// `pattern`: one letter a vsync, N a new picture, - the same picture again.
static std::vector<Shown> Run(const std::string& pattern, uint32_t presents)
{
	PS5FrameGen::Pacer pacer;
	std::vector<Shown> out;
	int picture = -1;
	for (const char c : pattern)
	{
		const bool fresh = (c == 'N');
		picture += fresh ? 1 : 0;
		pacer.Vsync(fresh);
		for (uint32_t shown = 0; shown < presents; shown++)
			out.push_back({picture, (picture >= 1 && pacer.Makes(presents)) ? pacer.Time(shown, presents) : 1.0f});
	}
	return out;
}

// The place in the game's own time of what is shown: picture 3 at time 0.5 is 2.5.
static float Place(const Shown& s)
{
	return static_cast<float>(s.picture) - 1.0f + s.time;
}

static std::string Repeat(const std::string& part, int times)
{
	std::string out;
	for (int i = 0; i < times; i++)
		out += part;
	return out;
}

int main()
{
	// 60 pictures a second on a 60 Hz screen: every picture is the game's own.
	for (const Shown& s : Run(Repeat("N", 20), 1))
		CHECK(s.time == 1.0f);

	// 30 a second on 60 Hz: after the first two pictures, a made one at the half and the picture itself, in turn.
	{
		const std::vector<Shown> shown = Run(Repeat("N-", 12), 1);
		for (size_t i = 4; i + 1 < shown.size(); i += 2)
		{
			CHECK(shown[i].time == 0.5f && shown[i + 1].time == 1.0f);
			CHECK(shown[i].picture == shown[i + 1].picture);
		}
		// What is on the screen moves on by half a picture every refresh.
		for (size_t i = 5; i < shown.size(); i++)
			CHECK(std::fabs(Place(shown[i]) - Place(shown[i - 1]) - 0.5f) < 1e-5f);
	}

	// 20 a second on 60 Hz: thirds.
	{
		const std::vector<Shown> shown = Run(Repeat("N--", 10), 1);
		for (size_t i = 6; i + 2 < shown.size(); i += 3)
		{
			CHECK(std::fabs(shown[i].time - 1.0f / 3.0f) < 1e-5f && std::fabs(shown[i + 1].time - 2.0f / 3.0f) < 1e-5f &&
				  shown[i + 2].time == 1.0f);
		}
	}

	// 60 a second on a 120 Hz screen: a made picture and the game's own for every vsync.
	{
		const std::vector<Shown> shown = Run(Repeat("N", 20), 2);
		for (size_t i = 4; i + 1 < shown.size(); i += 2)
			CHECK(shown[i].time == 0.5f && shown[i + 1].time == 1.0f);
	}

	// 30 a second on 120 Hz: quarters.
	{
		const std::vector<Shown> shown = Run(Repeat("N-", 12), 2);
		for (size_t i = 8; i + 3 < shown.size(); i += 4)
		{
			CHECK(shown[i].time == 0.25f && shown[i + 1].time == 0.5f && shown[i + 2].time == 0.75f && shown[i + 3].time == 1.0f);
		}
	}

	// Whatever the game does, what is on the screen never goes back in the game's time, the
	// picture itself is reached whenever a picture stays as long as the one before it did, and
	// a time is always above 0 and at most 1.
	for (const uint32_t presents : {1u, 2u})
	{
		for (const std::string& pattern : {Repeat("N-", 6) + Repeat("N", 6) + Repeat("N-", 6), Repeat("N--", 4) + Repeat("N-", 4) + Repeat("N--", 4),
				 Repeat("N", 5) + "N---------" + Repeat("N-", 5), std::string("N-N--N-N---N-NN-N--N-NNN--N-N-"), Repeat("N----", 6),
				 Repeat("N-", 3) + std::string(40, '-') + Repeat("N-", 3)})
		{
			const std::vector<Shown> shown = Run(pattern, presents);
			for (size_t i = 1; i < shown.size(); i++)
			{
				CHECK(shown[i].time > 0.0f && shown[i].time <= 1.0f);
				CHECK(Place(shown[i]) >= Place(shown[i - 1]) - 1e-5f || shown[i].picture < 1);
			}
			// The last refresh of a picture that stays at least as long as the one before: itself.
			for (size_t i = presents; i < shown.size(); i++)
			{
				const bool last_of_its = (i + 1 == shown.size()) || (shown[i + 1].picture != shown[i].picture);
				if (!last_of_its || shown[i].picture < 2)
					continue;
				size_t stay = 0, stay_before = 0;
				for (const Shown& s : shown)
				{
					stay += (s.picture == shown[i].picture);
					stay_before += (s.picture == shown[i].picture - 1);
				}
				if (stay >= stay_before && stay_before <= PS5FrameGen::Pacer::LONGEST * presents)
					CHECK(shown[i].time == 1.0f);
			}
		}
	}

	// A picture that stood for longer than a game draws at (a loading screen): nothing is made
	// towards the picture that follows it, which is shown at once.
	{
		const std::vector<Shown> shown = Run("N-N-N" + std::string(30, '-') + "N-N-N-", 1);
		size_t after = 0;
		for (size_t i = 0; i < shown.size(); i++)
		{
			if (shown[i].picture == 3 && after == 0)
				after = i;
		}
		CHECK(after != 0 && shown[after].time == 1.0f);
	}

	std::printf("frame generation's pacing: %d checks, %d failed\n", s_checks, s_failed);
	return s_failed ? 1 : 0;
}
