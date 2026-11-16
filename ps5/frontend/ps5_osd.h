// Crux2 - the performance overlay (the on-screen display's figures).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The figures PCSX2's on-screen display switches on (Settings > On-Screen Display > Show), in one of
// three sizes ([PS5] OsdSize, ps5_osd_logic.h): Minimal, a pill with the FPS; Compact (the default),
// one bar with the FPS and the GPU's and threads' loads; Full, a card with the frame times and their
// graph, the loads as bars and a footer; and Custom, the card with what the switches give. Each size
// is a preset of the switches. Drawn by ps5_osd_draw.h.
// [PS5] OsdStyle: 0 this overlay (the default), 1 PCSX2's own lines.

#pragma once

namespace PS5Osd
{
	/// Draws the overlay where the figures' place says and moves position_y under it. False when the
	/// style is PCSX2's own lines (the caller draws those).
	bool DrawPanel(float& position_y, float scale, float margin);
} // namespace PS5Osd
