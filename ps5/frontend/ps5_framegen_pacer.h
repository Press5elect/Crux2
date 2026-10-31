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

	/// The stays of the last pictures kept to tell the game's rate by (v1.0.18).
	static constexpr uint32_t HISTORY = 8;

	uint32_t period = 1; ///< vsyncs a picture is taken to stay: the game's rate as last settled
	uint32_t since = 0;  ///< vsyncs since the newest picture came (0 in the vsync that brought it)
	uint32_t gap = 0;    ///< the same, counted on for the next period
	uint32_t stays[HISTORY] = {}; ///< the last pictures' stays, newest at `next - 1`
	uint32_t kept = 0, next = 0;

	void Reset()
	{
		period = 1;
		since = 0;
		gap = 0;
		kept = next = 0;
	}

	/// A vsync of the game; `fresh`: it brought a new picture.
	///
	/// Up to v1.0.17 the period was the stay of the picture before alone, so a game at 30 a second
	/// that once showed a picture for one vsync or for three (which they do, loading or dropping a
	/// frame) had the next picture made in halves, then nothing, then in thirds: what moved on the
	/// screen sped up and slowed down every few seconds (seen in a console log as the period
	/// changing several times a second). The rate now changes only when the same stay comes twice
	/// in a row, or when it is most of the last eight: one picture early or late is shown as it
	/// comes and does not change how the ones after it are made.
	void Vsync(bool fresh)
	{
		if (fresh)
		{
			if (gap < 1 || gap > LONGEST)
			{
				// The first picture, or one after a standstill: no rate yet.
				kept = next = 0;
				period = 1;
			}
			else
			{
				stays[next] = gap;
				next = (next + 1) % HISTORY;
				kept = (kept < HISTORY) ? kept + 1 : HISTORY;
				period = Settled();
			}
			gap = 0;
			since = 0;
		}
		else
		{
			since = (since < 1000) ? since + 1 : 1000;
		}
		gap = (gap < 1000) ? gap + 1 : 1000;
	}

	/// The rate the last stays settle on: the newest when the one before it was the same, else the
	/// one most of them had, else the rate there was.
	uint32_t Settled() const
	{
		const uint32_t newest = stays[(next + HISTORY - 1) % HISTORY];
		if (kept >= 2 && stays[(next + HISTORY - 2) % HISTORY] == newest)
			return newest;
		if (kept == 1)
			return newest;
		uint32_t count[LONGEST + 1] = {};
		for (uint32_t i = 0; i < kept; i++)
			count[stays[i]]++;
		for (uint32_t value = 1; value <= LONGEST; value++)
		{
			if (count[value] * 2 > kept)
				return value;
		}
		return period;
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
