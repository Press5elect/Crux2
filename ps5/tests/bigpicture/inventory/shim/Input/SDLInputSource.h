// Crux2 - the settings inventory: SDL's input source as Pad.cpp names it, without SDL (the PC that
// makes the inventory need not have SDL 3). Pad.cpp calls ResetRGBForAllPlayers only when the
// controllers' defaults are set, which the pages never do while they are made; it is a stub.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
#pragma once

#include "Input/InputSource.h"

class SettingsInterface;

class SDLInputSource
{
public:
	static void ResetRGBForAllPlayers(SettingsInterface& si);
};
