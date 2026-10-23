// Crux2 - what a game is: a few sentences of it, who made it, when, what kind of game it is.
// Looked up on Wikipedia and Wikidata the first time a game is shown where that is said (the Disc
// tab), on a thread of its own, and kept in <data>/about/<serial>.txt. (ps5_about.cpp)
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

#pragma once

#include "ps5_about_logic.h"

#include <string>

namespace PS5About
{
	/// What is known of the game. With nothing kept of it yet, and [PS5] FetchAbout on (the
	/// default), a lookup starts and `looking` (may be null) says so; a later call has its answer.
	/// Cheap: from the thread that draws, every frame.
	Info Get(const std::string& serial, const std::string& title, bool* looking);

	/// Forgets what is kept of every game (the setting's "Look again").
	void Forget();
} // namespace PS5About
