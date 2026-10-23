// Crux2 - the on-screen display's figures as one panel.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The figures PCSX2's on-screen display switches on (Settings > On-Screen Display > Show), laid out
// the way PC overlays show them: a section for the graphics chip, one for the processor, one for
// the frames, each a column of names and a column of numbers with their units small beside them,
// and the frame times as a graph at the foot. Drawn in the title's lettering (Saira ExtraBold).
// [PS5] OsdStyle: 0 this panel (the default), 1 PCSX2's own lines.

#pragma once

namespace PS5Osd
{
	/// Draws the panel where the figures' place says and moves position_y under it. False when the
	/// style is PCSX2's own lines (the caller draws those).
	bool DrawPanel(float& position_y, float scale, float margin);
} // namespace PS5Osd
