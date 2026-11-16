// Crux2 - which half of the touch pad a click is on (PS5InputLogic::TouchClick), on the PC.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The left half is Select, the right half Start. A thumb resting on one half must not decide a click
// made with another finger on the other half, and a click reported before its finger takes the finger
// seen a moment before.

#include "ps5_input_source_logic.h"

#include <cstdio>

static int s_failures = 0;
static int s_checks = 0;

#define CHECK(condition) \
	do \
	{ \
		s_checks++; \
		if (!(condition)) \
		{ \
			s_failures++; \
			std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); \
		} \
	} while (0)

using PS5InputLogic::TouchClick;

static void Feed(TouchClick& t, unsigned count, uint16_t x0, uint8_t id0, uint16_t x1, uint8_t id1, uint64_t now)
{
	const uint16_t xs[2] = {x0, x1};
	const uint8_t ids[2] = {id0, id1};
	t.Touches(count, xs, ids, now);
}

int main()
{
	// One finger: its half.
	{
		TouchClick t;
		Feed(t, 1, 300, 7, 0, 0, 1000);
		CHECK(t.LeftHalf(1000));
		t.Reset();
		Feed(t, 1, 1500, 7, 0, 0, 1000);
		CHECK(!t.LeftHalf(1000));
	}
	// A thumb resting on the right half (the first finger reported), a second finger clicks the left.
	{
		TouchClick t;
		Feed(t, 1, 1600, 3, 0, 0, 1000);
		Feed(t, 1, 1600, 3, 0, 0, 20000);
		Feed(t, 2, 1600, 3, 400, 4, 40000);
		CHECK(t.LeftHalf(40000));
	}
	// The same the other way round.
	{
		TouchClick t;
		Feed(t, 1, 300, 3, 0, 0, 1000);
		Feed(t, 2, 300, 3, 1700, 4, 30000);
		CHECK(!t.LeftHalf(30000));
	}
	// The report may list the newer finger first: the order of touching decides, not the slot.
	{
		TouchClick t;
		Feed(t, 1, 1600, 3, 0, 0, 1000);
		Feed(t, 2, 500, 9, 1600, 3, 30000);
		CHECK(t.LeftHalf(30000));
	}
	// The click's reading has no finger yet: the one seen a moment before.
	{
		TouchClick t;
		Feed(t, 1, 200, 5, 0, 0, 1000);
		Feed(t, 0, 0, 0, 0, 0, 9000);
		CHECK(t.LeftHalf(9000));
		CHECK(t.LeftHalf(1000 + TouchClick::RECENT_US));
		// Too long ago: Start, as with no finger at all.
		CHECK(!t.LeftHalf(1000 + TouchClick::RECENT_US + 1));
	}
	// No finger ever: Start.
	{
		TouchClick t;
		Feed(t, 0, 0, 0, 0, 0, 1000);
		CHECK(!t.LeftHalf(1000));
	}
	// A finger that lifts and touches again is a new touch (a new id), and counts as the newest.
	{
		TouchClick t;
		Feed(t, 2, 1600, 3, 300, 4, 1000);  // 3 touched in the same reading, then 4
		Feed(t, 1, 1600, 3, 0, 0, 2000);     // 4 lifts
		Feed(t, 2, 1600, 3, 350, 6, 3000);   // a finger touches the left half again
		CHECK(t.LeftHalf(3000));
		Feed(t, 1, 350, 6, 0, 0, 4000);      // 3 lifts
		Feed(t, 2, 350, 6, 1800, 8, 5000);   // and a new one on the right
		CHECK(!t.LeftHalf(5000));
	}
	// A middle at exactly 960 is the right half (as before).
	{
		TouchClick t;
		Feed(t, 1, TouchClick::MIDDLE, 1, 0, 0, 10);
		CHECK(!t.LeftHalf(10));
		Feed(t, 1, TouchClick::MIDDLE - 1, 2, 0, 0, 20);
		CHECK(t.LeftHalf(20));
	}
	std::printf("touch pad clicks: %d checks, %d failed\n", s_checks, s_failures);
	return s_failures ? 1 : 0;
}
