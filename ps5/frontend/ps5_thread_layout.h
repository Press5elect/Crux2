// Crux2 - where the emulator's threads run ([PS5] ThreadLayout).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The layout: the EE,
// VU and GS threads each on a core of their own, the second hardware thread of the EE's and the
// VU's cores left empty, and every other thread of the title (the driver's workers, sound, the
// interface, shader compiles) kept to the CPUs left over, so that nothing shares the EE's or the
// VU's core. PCSX2's own thread pinning pins the three threads only: the others could still land
// on their cores.

#pragma once

#include <cstdint>

namespace PS5ThreadLayout
{
	/// A title's logical processors (0 to 12; 2n and 2n+1 are one core's two threads).
	constexpr uint64_t kTitleMask = 0x1FFFu;

	/// Whether [PS5] ThreadLayout asks for the layout (read on the CPU thread at each game's start).
	bool Wanted();

	/// Every thread of the title to these processors (FreeBSD's cpuset for the process). Threads
	/// pinned afterwards keep their own masks; a thread a pinned thread starts inherits its mask.
	bool RestrictProcess(uint64_t mask);

	/// The calling thread to these processors (logged as `what`), whatever the process's set.
	bool PinCallingThread(uint64_t mask, const char* what);

	/// paraLLEl-GS's thread under the layout: core 3 (processors 6 and 7), its own while the
	/// layout keeps EE, VU and GS on cores 0 to 2; its driver thread, started from it, shares it.
	constexpr uint64_t kParallelGsMask = (uint64_t{1} << 6) | (uint64_t{1} << 7);
} // namespace PS5ThreadLayout
