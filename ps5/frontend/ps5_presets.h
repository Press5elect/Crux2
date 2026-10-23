// Crux2 - what the title offers ready-made, where a setting would otherwise have to be typed.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

#pragma once

namespace PS5Presets
{
/// A name server a PlayStation 2 game can be pointed at: the people who keep games' online play
/// going after the official servers closed run one, and it answers a game's question "where is
/// my server?" with the address of theirs. The address is the one the service's own site gives;
/// the service is not this project's, and may change or end without the title knowing.
struct NameServer
{
	const char* name;    // "PS2 Online"
	const char* site;    // where its address is published
	const char* address; // "a.b.c.d"
};

/// The first of them is the one a new installation starts with ([DEV9/Eth] DNS1), and the one
/// an installation that never chose a name server is moved to once (ps5_host.cpp).
inline constexpr NameServer NAME_SERVERS[] = {
	{"PS2 Online", "ps2online.com", "45.7.228.197"},
	// PS Rewired's guide for PCSX2 (psrewired.com/guides/pcsx2) gives this as the primary name server.
	{"PS Rewired", "psrewired.com", "67.222.156.250"},
};
} // namespace PS5Presets
