// Crux2 - the minimum hold of a quick tap (PS5InputLogic::TapHold, ps5_input_source_logic.h), on the PC.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The source passes every reading of a poll in order, and the game reads the pad's state once in
// its frame: a press that went down and up in one poll's readings must still be down when the game
// reads, and stay so for at least 50 ms.

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

int main()
{
	using PS5InputLogic::TapHold;
	constexpr uint32_t CROSS = 1u << 0, SQUARE = 1u << 2, UP = 1u << 11, R3 = 1u << 8;
	constexpr uint64_t ms = 1000;

	// A tap whose down and up are both in one poll's readings: down for the game after the poll,
	// held 50 ms, then let go by the polls that bring no reading.
	{
		TapHold t;
		CHECK(t.Update(CROSS, 1000 * ms) == CROSS);
		CHECK(t.Update(0, 1000 * ms) == CROSS); // the same poll's next reading: let go too soon
		CHECK(t.Given() == CROSS);
		CHECK(t.Tick(1016 * ms) == CROSS);
		CHECK(t.Tick(1033 * ms) == CROSS);
		CHECK(t.Tick(1049 * ms) == CROSS);
		CHECK(t.Tick(1050 * ms) == 0); // 50 ms after it went down
		CHECK(t.Tick(1066 * ms) == 0);
	}
	// Held longer than the hold: let go at once.
	{
		TapHold t;
		CHECK(t.Update(SQUARE, 2000 * ms) == SQUARE);
		CHECK(t.Update(SQUARE, 2500 * ms) == SQUARE);
		CHECK(t.Update(0, 2516 * ms) == 0);
	}
	// A release during the hold is given when the hold ends, from a later reading too.
	{
		TapHold t;
		CHECK(t.Update(UP, 3000 * ms) == UP);
		CHECK(t.Update(0, 3020 * ms) == UP);
		CHECK(t.Update(0, 3040 * ms) == UP);
		CHECK(t.Update(0, 3060 * ms) == 0);
	}
	// Buttons are held each for themselves: another button's press does not hold this one longer,
	// and one let go late while another taps.
	{
		TapHold t;
		CHECK(t.Update(CROSS, 4000 * ms) == CROSS);
		CHECK(t.Update(CROSS | SQUARE, 4040 * ms) == (CROSS | SQUARE));
		CHECK(t.Update(SQUARE, 4045 * ms) == (CROSS | SQUARE)); // Cross held until 4050
		CHECK(t.Update(0, 4050 * ms) == SQUARE); // Cross's hold over; Square's until 4090
		CHECK(t.Tick(4089 * ms) == SQUARE);
		CHECK(t.Tick(4090 * ms) == 0);
	}
	// A second tap during the first one's hold starts the hold again from its own press.
	{
		TapHold t;
		CHECK(t.Update(CROSS, 5000 * ms) == CROSS);
		CHECK(t.Update(0, 5005 * ms) == CROSS);
		CHECK(t.Update(CROSS, 5030 * ms) == CROSS);
		CHECK(t.Update(0, 5031 * ms) == CROSS);
		CHECK(t.Tick(5060 * ms) == CROSS); // 30 ms after the second press
		CHECK(t.Tick(5080 * ms) == 0);
	}
	// With no hold (no game running) the buttons go as they are.
	{
		TapHold t;
		CHECK(t.Update(R3, 6000 * ms, 0) == R3);
		CHECK(t.Update(0, 6000 * ms, 0) == 0);
		// a hold that was running ends the moment the game stops
		CHECK(t.Update(CROSS, 6100 * ms) == CROSS);
		CHECK(t.Update(0, 6101 * ms) == CROSS);
		CHECK(t.Tick(6102 * ms, 0) == 0);
	}
	// Reset: nothing held, and a press after it is a new press.
	{
		TapHold t;
		CHECK(t.Update(CROSS, 7000 * ms) == CROSS);
		t.Reset();
		CHECK(t.Given() == 0);
		CHECK(t.Tick(7001 * ms) == 0);
		CHECK(t.Update(CROSS, 7002 * ms) == CROSS);
		CHECK(t.Update(0, 7003 * ms) == CROSS);
		CHECK(t.Tick(7052 * ms) == 0);
	}
	// Bit 31 and a clock far from zero.
	{
		TapHold t;
		const uint64_t base = 0xFFFF'FFFF'0000ull;
		CHECK(t.Update(1u << 31, base) == (1u << 31));
		CHECK(t.Update(0, base + 10 * ms) == (1u << 31));
		CHECK(t.Tick(base + 50 * ms) == 0);
	}

	std::printf("tap hold: %d checks, %d failed\n", s_checks, s_failures);
	return s_failures == 0 ? 0 : 1;
}
