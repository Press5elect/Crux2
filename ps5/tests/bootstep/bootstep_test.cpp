// Crux2 - host test of the loading screen's line for a slow start (frontend/ps5_boot_step.h).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

#include "ps5_boot_step.h"

#include <cstdio>

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

int main()
{
	using namespace PS5BootStep;
	// Nothing while a step is quick, or when there is none.
	CHECK(SlowLine(Step::Disc, 2.0, true).empty());
	CHECK(SlowLine(Step::None, 60.0, true).empty());
	// A slow disc image on the share: the step, the seconds, and the share named.
	const std::string share = SlowLine(Step::Disc, 14.6, true);
	CHECK(share.rfind("Still opening the disc image after 14 seconds. ", 0) == 0);
	CHECK(share.find("share") != std::string::npos);
	// The same on the console's own storage does not talk about a share.
	CHECK(SlowLine(Step::Disc, 9.0, false).find("share") == std::string::npos);
	// Every step has a name and something to look at.
	for (Step s : {Step::GameList, Step::Bios, Step::Disc, Step::Graphics, Step::Sound, Step::Controllers, Step::Cards, Step::Network, Step::Usb})
	{
		CHECK(Doing(s)[0] != '\0');
		CHECK(Advice(s, false)[0] != '\0' && Advice(s, true)[0] != '\0');
		CHECK(SlowLine(s, SLOW_SECONDS, false).size() > 20);
	}
	std::printf("boot steps: %d checks, %d failed\n", s_checks, s_failed);
	return s_failed == 0 ? 0 : 1;
}
