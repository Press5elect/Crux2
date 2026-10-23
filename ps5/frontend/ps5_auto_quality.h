// Crux2 - automatic quality: the internal resolution steps down while the graphics side cannot
// keep the game at full speed, and back up when it can again ([EmuCore/GS] PS5AutoQuality).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The resolution you chose stays what is saved. While it is on, a scene that holds the game under
// full speed with the graphics chip the busy one gets 1x less, one step at a time; the processor
// being the limit, the GS thread's included (where a lower resolution would not help), changes
// nothing, and a step that did not make the game faster is taken back.
// Ten seconds of full speed with room to spare give a step back. Each step is said on the screen
// and in the boot log. Settings applied again (any change in the menus) start from your
// resolution.

#pragma once

#include <cstdint>

namespace PS5AutoQuality
{
	/// The GS thread, once a vsync, after the picture was shown and the figures were updated
	/// (GSRenderer::VSync). Changes GSConfig's internal resolution only (GSUpdateConfig).
	void VSync();

	/// The renderer was made again: forget what was learned (the steps counted stay).
	void Reset();

	/// A game starts: forget everything, the steps counted too.
	void ResetSession();

	/// Steps down and up since the game started, for the session's summary in the boot log.
	uint32_t StepsDown();
	uint32_t StepsUp();
	/// The lowest internal resolution it went to (0 when it never stepped down).
	float Lowest();

	/// The game's session so far, at normal speed (sampled every half second whether automatic
	/// quality is on or not), for the summary written when it closes (ps5_host.cpp).
	struct Figures
	{
		double seconds_at_normal_speed = 0;
		float average_speed = 0;  // percent of the speed asked for
		double seconds_slow = 0;  // under 95% of it
		double longest_slow = 0;  // the longest stretch under 95%
		uint32_t steps_down = 0, steps_up = 0;
		float lowest = 0;
	};
	Figures SessionFigures();
} // namespace PS5AutoQuality
