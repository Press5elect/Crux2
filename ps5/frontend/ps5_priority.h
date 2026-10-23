// Crux2 - which of the title's threads the console runs first ([PS5] ThreadPriorities).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Every thread of a title starts at the same priority (700 of 256 to 767, lower runs first), and
// the console's scheduler gives a processor to the higher priority strictly: a thread that never
// waits keeps its processor from every thread of its priority or below. (Measured by PS5_MihawkOS's
// scheduler probe, Copyright (C) 2026 Mihawk, GPL-3.0-or-later: with every processor busy, a
// periodic thread at the title's priority made no progress at all.) The emulator's EE, VU and GS
// threads spin while they wait for each other, so a sound thread, the controllers or the interface
// could wait behind them, and a background job (a download, an unpacking, card sync, the game
// list) could take a processor from them.
//
// A thread starts at the priority of the thread that started it, so the emulation thread sets the
// title's own (700) again: the GS, VU and software renderer threads it starts are then at 700 too.
//
// So: the sound threads and the microphones run first, then the controllers, then the interface;
// the emulator's threads stay where every thread starts; background jobs run after them all. Each
// thread sets its own, when it starts. scePthreadSetprio is found while the title runs (not
// imported: a title that imports what the console does not give it does not start).

#pragma once

namespace PS5Priority
{
enum class Role
{
	Sound,      // 690: the sound output streams, the interface's sounds, the microphones
	Input,      // 692: the USB controllers' thread
	Interface,  // 695: the main thread (the interface, the host's loop)
	Emulation,  // 700: the emulation thread, and so the threads it starts (GS, VU, software renderer)
	Background, // 730: downloads, unpacking, copies, card sync, the game list, the web page
};

/// The calling thread runs at its role's priority, unless [PS5] ThreadPriorities is off.
/// Said once per role in the boot log ("priority: ...").
#ifdef __PROSPERO__
void SetCurrentThread(Role role);
#else
// The PC-side tests build the files that call this: nothing to set there.
inline void SetCurrentThread(Role) {}
#endif
} // namespace PS5Priority
