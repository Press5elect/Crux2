// Crux2 - native modules: a game's main-processor (EE) code converted ahead of time by PS2Recomp
// in its Crux2 mode (ps5/recomp), run in place of PCSX2's recompiled blocks where it exists.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// A module is made on the player's own PC from the player's own copy of the game's executable
// (ps5/recomp/tools/recomp-game.sh) and copied to <data folder>/native/<executable>.crux2native
// (SLUS_213.76.crux2native, say). Crux2 never ships one.
//
// How it runs: when PCSX2's recompiler (iR5900.cpp) is about to compile a block at an address the
// module has native code for, it compiles a call instead: the registers go to the module, its code
// runs until it reaches an address it has no code for, an instruction only the emulator can run
// (a system call, COP0, the vector unit's COP2, a cache operation), or PCSX2's next event is due;
// the registers come back, the cycles are added, and PCSX2's dispatcher carries on from there.
// Every function's bytes are checked against what the module was made from before its first run,
// and its pages are watched for writes (PCSX2's own protection of code pages), so a patched or
// replaced function goes back to PCSX2's recompiler.
//
// Everything here runs on the emulation (EE) thread.

#pragma once

#include <cstdint>
#include <string>

namespace PS5Native
{
	/// The running executable changed (Host::OnGameChanged): the module for it is loaded, or none.
	void GameChanged();
	/// The virtual machine closed: the module goes (PCSX2's recompiler is reset before the next).
	void VMDestroyed();

	/// Whether a module is loaded and switched on.
	bool Active();
	/// Whether native code may run from pc now (iR5900.cpp's recRecompile).
	bool Has(uint32_t pc);

	/// The compiled call's target: runs the module from pc on PCSX2's registers, sets cpuRegs.pc
	/// to where to go on and adds the cycles. Gives back false when it ran nothing (the block is
	/// then compiled again the usual way, and not native, see the source).
	bool Run(uint32_t pc);
	/// pc gave control straight back (its first instruction is the emulator's): it is compiled the
	/// usual way from now on, until PCSX2's recompiler resets.
	void Refuse(uint32_t pc);

	/// PCSX2 cleared its compiled code in [start, start + words * 4) (recClear): the functions there
	/// are checked again before their next run.
	void Clear(uint32_t start, uint32_t words);
	/// PCSX2's recompiler was reset (a reset, a state loaded): every function is checked again.
	void Reset();

	/// A line for the log and the performance overlay: functions run natively, instructions, entries.
	std::string Summary();
} // namespace PS5Native
