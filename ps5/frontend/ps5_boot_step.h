// Crux2 - what a game's start is doing, for a start that takes long: the loading screen names
// the step after a few seconds and says what to look at, and the boot log keeps how long each
// step took. The steps are VMManager::Initialize's, marked where it begins each one.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The words are header-only, for ps5/tests/bootstep.

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace PS5BootStep
{
	enum class Step
	{
		None,
		GameList, // the game list's scan is stopped first
		Bios,
		Disc, // the disc image (or the drive) is opened
		Graphics,
		Sound,
		Controllers,
		Cards,
		Network,
		Usb,
	};

	/// A step's name, as "opening the disc image".
	inline const char* Doing(Step s)
	{
		switch (s)
		{
			case Step::GameList: return "stopping the game list's scan";
			case Step::Bios: return "loading the BIOS";
			case Step::Disc: return "opening the disc image";
			case Step::Graphics: return "starting the renderer";
			case Step::Sound: return "starting the sound";
			case Step::Controllers: return "starting the controllers";
			case Step::Cards: return "opening the memory cards";
			case Step::Network: return "starting the network adapter";
			case Step::Usb: return "starting the USB devices";
			case Step::None: break;
		}
		return "";
	}

	/// What to look at when a step takes long; `network`: the game is on the network share.
	inline const char* Advice(Step s, bool network)
	{
		switch (s)
		{
			case Step::GameList:
				return network ? "The game list was reading the network share; it stops at the next file." : "The game list stops at the next file.";
			case Step::Disc:
				return network ? "Over the network a large image is slow to open. If this does not move, the share may have stopped answering: "
								 "check the computer that shares it."
							   : "If this does not move, the drive that holds it may not be answering.";
			case Step::Cards: return "A memory card on the network share or a slow drive takes longer to open.";
			case Step::Network: return "The network adapter waits for the console's network.";
			default: return "If this does not move, close the title and start it again; the boot log says where it stopped.";
		}
	}

	/// How long a step goes before the loading screen names it.
	constexpr double SLOW_SECONDS = 8.0;

	/// The loading screen's line for a step that has gone on `seconds`: empty while it is not slow.
	inline std::string SlowLine(Step s, double seconds, bool network)
	{
		if (s == Step::None || seconds < SLOW_SECONDS)
			return std::string();
		char line[384];
		std::snprintf(line, sizeof(line), "Still %s after %d seconds. %s", Doing(s), static_cast<int>(seconds), Advice(s, network));
		return line;
	}

	// Runtime (ps5_boot_step.cpp).
	/// Marks the start of a step (None: the start is over); the step before is logged when it took
	/// half a second or more.
	void Set(Step s);
	/// The step under way and for how long it has gone.
	Step Current(double* seconds);
} // namespace PS5BootStep
