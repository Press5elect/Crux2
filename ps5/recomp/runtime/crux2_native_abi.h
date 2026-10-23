// Crux2 - the boundary between Crux2 and a game's native module: main-processor (EE) code of one
// game, converted ahead of time to C++ by PS2Recomp in its Crux2 mode and built for the console.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Plain C, so that the module needs nothing of Crux2 but this file. Crux2 gives the module a table
// of what it may call (memory through PCSX2's own map of the PS2's addresses, a log line); the
// module gives Crux2 three functions. The state crosses as a copy of the registers: Crux2 fills
// it from PCSX2's, the module runs, Crux2 copies it back. Nothing else is shared.
//
// A module is made on the player's own PC from the player's own copy of the game's executable
// (ps5/recomp/tools/recomp-game.sh), and is the game's code: it is never shipped with Crux2.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 3: the SA register is PCSX2's own value (Crux2 v1.3.5; the module runs PCSX2's code for the MMI,
//    FPU and SA instructions); 2: SA as a bit count, the watch callback.
#define CRUX2_NATIVE_ABI 3

/// What Crux2 offers the module. The memory functions are PCSX2's own (vtlb_memRead/Write), for
/// the addresses the map does not point straight at memory for (hardware registers, the
/// scratchpad when it is not mapped, unmapped addresses).
typedef struct Crux2NativeHost
{
	uint32_t abi;
	uint32_t size; ///< sizeof(Crux2NativeHost)
	/// PCSX2's map of the PS2's 4 GiB, one entry per 4 KiB page: entry + address is the host
	/// address of the byte when that sum is not negative; otherwise the page has a handler.
	const uintptr_t* vmap;
	uint8_t (*read8)(uint32_t address);
	uint16_t (*read16)(uint32_t address);
	uint32_t (*read32)(uint32_t address);
	uint64_t (*read64)(uint32_t address);
	void (*read128)(uint32_t address, void* out16);
	void (*write8)(uint32_t address, uint8_t value);
	void (*write16)(uint32_t address, uint16_t value);
	void (*write32)(uint32_t address, uint32_t value);
	void (*write64)(uint32_t address, uint64_t value);
	void (*write128)(uint32_t address, const void* in16);
	void (*log)(const char* line);
	/// Before a function's native code first runs: asks Crux2 to watch [start, start + bytes) for
	/// writes (PCSX2's write protection of pages with code in them), so that a write there reaches
	/// the module as crux2_native_invalidate. Zero when it cannot be watched (a page PCSX2 checks by
	/// hand because code and data share it): the function is then left to PCSX2's recompiler.
	int (*watch)(uint32_t start, uint32_t bytes);
} Crux2NativeHost;

/// The main processor's state, as it crosses. The layout is the module's and Crux2's own (not
/// PCSX2's), so that neither depends on the other's.
typedef struct Crux2NativeState
{
	uint64_t gpr[32][2]; ///< the 128-bit registers, low half first
	uint64_t hi[2], lo[2]; ///< HI/LO and HI1/LO1 (the second halves)
	uint32_t sa; ///< PCSX2's SA register as it is (cpuRegs.sa: a byte count after MTSAB)
	uint32_t pc; ///< in: where to start (an entry the module has); out: where to go on
	uint32_t fpr[32]; ///< the FPU's registers, as bits
	uint32_t fcr31;
	uint32_t facc; ///< the FPU's accumulator, as bits
	int64_t budget; ///< in: instructions to run at most before giving control back
	int64_t count; ///< out: instructions run
	uint32_t exit_reason; ///< out: CRUX2_NATIVE_EXIT_*
	uint32_t traps; ///< out: trap conditions met (the emulator raises nothing for them here)
} Crux2NativeState;

enum
{
	CRUX2_NATIVE_EXIT_NOT_NATIVE = 0, ///< the next address has no native code
	CRUX2_NATIVE_EXIT_BUDGET = 1, ///< the instruction budget ran out (events are due)
	CRUX2_NATIVE_EXIT_HOST = 2, ///< the next instruction is one the emulator runs itself
};

/// One function of the game's executable that the module has native code for: its bytes as the
/// module was made from them. Before a function's native code first runs, the module hashes what
/// memory holds there and compares; a function that differs (patched by a cheat or a fix in
/// PCSX2's game database, or code the game loaded over it) is left to PCSX2's recompiler.
typedef struct Crux2NativeFunction
{
	uint32_t start, end; ///< [start, end) in the PS2's addresses
	uint64_t hash; ///< FNV-1a 64 of those bytes (crux2_native_hash)
} Crux2NativeFunction;

/// What the module is.
typedef struct Crux2NativeInfo
{
	uint32_t abi;
	uint32_t size; ///< sizeof(Crux2NativeInfo)
	uint32_t entries; ///< addresses it has native code for (function starts and places to resume)
	uint32_t functions; ///< functions
	uint32_t text_start, text_end; ///< the range the functions are in
	const char* executable; ///< the executable's name ("SLUS_213.76")
	const char* made_by; ///< "PS2Recomp <revision>, Crux2 mode"
} Crux2NativeInfo;

/// The module's functions, found by name in its dynamic symbols.
typedef const Crux2NativeInfo* (*Crux2NativeInitFn)(const Crux2NativeHost* host);
/// Whether pc has native code that may run now (its function's bytes checked, see above).
typedef int (*Crux2NativeHasFn)(uint32_t pc);
typedef uint32_t (*Crux2NativeRunFn)(Crux2NativeState* state);
/// Memory in [start, start + bytes) was written to by something other than a game's data store
/// (PCSX2 cleared its compiled code there): the functions in it are checked again before use.
typedef void (*Crux2NativeInvalidateFn)(uint32_t start, uint32_t bytes);
/// Counts for the log: functions checked and found the same, and found different.
typedef void (*Crux2NativeStatsFn)(uint32_t* checked_same, uint32_t* checked_different, uint32_t* invalidations);

#define CRUX2_NATIVE_INIT_SYMBOL "crux2_native_init"
#define CRUX2_NATIVE_HAS_SYMBOL "crux2_native_has"
#define CRUX2_NATIVE_RUN_SYMBOL "crux2_native_run"
#define CRUX2_NATIVE_INVALIDATE_SYMBOL "crux2_native_invalidate"
#define CRUX2_NATIVE_STATS_SYMBOL "crux2_native_stats"

/// FNV-1a 64, the hash of the code bytes (the same in the module's build and in Crux2).
static inline uint64_t crux2_native_hash(const void* data, uint64_t size)
{
	const uint8_t* p = (const uint8_t*)data;
	uint64_t h = 0xcbf29ce484222325ull;
	for (uint64_t i = 0; i < size; i++)
		h = (h ^ p[i]) * 0x100000001b3ull;
	return h;
}

#ifdef __cplusplus
}
#endif
