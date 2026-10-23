// Crux2 - frame generation: which picture goes on the screen at each of its refreshes
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The game hands over a picture at each of its vsyncs, 60 times a second (50 for PAL), and many
// games hand over the same picture two or three times in a row: they draw 30 or 20 a second.
// Frame generation fills those repeats with pictures made between the last two new ones, and, on
// a screen that refreshes twice for each vsync (120 Hz), the refreshes between as well.
//
// To make a picture between two, the second has to be there: so everything is shown later than
// without, by the time from a new picture to the next refresh after the last made one, and the
// number of refreshes a picture will stay is guessed from how long the one before it stayed.
//
// Header-only and without anything of PCSX2's, so that ps5/tests/framegen runs it on a PC.
#pragma once

#include <algorithm>
#include <cstdint>

namespace PS5FrameGen
{
struct Pacer
{
	/// A game that shows a picture for longer than this many vsyncs is not drawing at a rate (a
	/// loading screen, a menu that stands still): nothing is made between such pictures.
	static constexpr uint32_t LONGEST = 4;

	uint32_t period = 1; ///< vsyncs the picture before the newest stayed: how long the newest is taken to stay
	uint32_t since = 0;  ///< vsyncs since the newest picture came (0 in the vsync that brought it)
	uint32_t gap = 0;    ///< the same, counted on for the next period

	void Reset()
	{
		period = 1;
		since = 0;
		gap = 0;
	}

	/// A vsync of the game; `fresh`: it brought a new picture.
	void Vsync(bool fresh)
	{
		if (fresh)
		{
			period = (gap >= 1 && gap <= LONGEST) ? gap : 1;
			gap = 0;
			since = 0;
		}
		else
		{
			since = std::min<uint32_t>(since + 1, 1000);
		}
		gap = std::min<uint32_t>(gap + 1, 1000);
	}

	/// Whether anything is made at all at this rate: `presents` pictures are shown for a vsync.
	bool Makes(uint32_t presents) const { return period * presents > 1; }

	/// The `shown`-th of the `presents` pictures shown for this vsync: the time, between the
	/// picture before the newest (0) and the newest (1), of the picture to show. 1: the newest itself.
	float Time(uint32_t shown, uint32_t presents) const
	{
		const uint32_t slots = period * presents;
		const uint32_t slot = since * presents + shown + 1;
		return (slot >= slots) ? 1.0f : static_cast<float>(slot) / static_cast<float>(slots);
	}
};
} // namespace PS5FrameGen
