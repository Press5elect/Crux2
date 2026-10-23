// Crux2 - the runtime PS2Recomp's generated code is built against, in Crux2 mode: the game's
// main-processor code running on Crux2's (PCSX2's) state instead of PS2Recomp's own runtime.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The names are PS2Recomp's (ps2xRuntime/include/ps2_runtime.h, GPL-3.0, ran-j and contributors),
// because the generated code calls them: R5900Context with its fields, PS2Runtime with the calls
// the translators emit, getRegU32 and friends. What is behind them is Crux2's:
//
//  - Memory goes through PCSX2's own map of the PS2's addresses (Crux2NativeHost::vmap): a page
//    that is plain memory is read and written in place, the rest through PCSX2's handlers. So a
//    write to a hardware register reaches PCSX2's hardware, and a write over code that PCSX2 has
//    compiled is caught by PCSX2's page protection as any other write is.
//  - Control goes back to Crux2 whenever the generated code would leave what the module has:
//    a branch to an address with no native code, an instruction the emulator runs itself
//    (CRUX2_EXIT, emitted by the converter's Crux2 mode), or an instruction budget spent (the
//    converter's back-edge checkpoints, eeCheckpointDue). Crux2 then lets PCSX2's recompiler
//    run from that address, and its event scheduler run on time.
//  - Calls to functions the module has are made as C calls, so a game's call chain stays native;
//    a call deeper than kMaxDepth, or one into an address without native code, gives control back.

#pragma once

#include <cstdint>
#include <cstring>
#include <immintrin.h>
#include <smmintrin.h>

#include "crux2_native_abi.h"

// What PS2Recomp's macros header expects of its runtime header.
inline constexpr uint32_t PS2_RAM_SIZE = 32u * 1024u * 1024u;
inline constexpr uint32_t PS2_RAM_MASK = PS2_RAM_SIZE - 1u;

enum PS2Exception
{
	EXCEPTION_TLB_REFILL = 0x02,
	EXCEPTION_ADDRESS_ERROR_LOAD = 0x04,
	EXCEPTION_ADDRESS_ERROR_STORE = 0x05,
	EXCEPTION_SYSCALL = 0x08,
	EXCEPTION_BREAKPOINT = 0x09,
	EXCEPTION_RESERVED_INSTRUCTION = 0x0A,
	EXCEPTION_INTEGER_OVERFLOW = 0x0C,
	EXCEPTION_TRAP = 0x0D,
};

// The main processor's state as the generated code sees it: PS2Recomp's fields, so every
// translator's output compiles. Only the main processor's part is filled (crux2_module.cpp); the
// COP0 and VU0 fields are never reached in Crux2 mode, whose code hands those instructions back.
struct alignas(16) R5900Context
{
	__m128i r[32];
	uint32_t pc;
	uint64_t insn_count;
	// HI and LO as the 128-bit registers they are (hi,hi1 and lo,lo1 side by side, 16-byte
	// aligned): crux2_ee_interp.h works on them whole, as PCSX2's interpreter does.
	alignas(16) uint64_t hi;
	uint64_t hi1;
	alignas(16) uint64_t lo;
	uint64_t lo1;
	uint32_t sa; ///< PCSX2's SA register as it is (a byte count after MTSAB); crux2_ee_interp.h uses it

	__m128 vu0_vf[32];
	uint16_t vi[16];
	float vu0_q, vu0_p, vu0_i;
	__m128 vu0_r;
	__m128 vu0_acc;
	uint16_t vu0_status;
	uint32_t vu0_mac_flags, vu0_clip_flags, vu0_clip_flags2;
	uint32_t vu0_cmsar0, vu0_cmsar1, vu0_cmsar2, vu0_cmsar3;
	uint32_t vu0_vpu_stat, vu0_vpu_stat2, vu0_vpu_stat3, vu0_vpu_stat4;
	uint32_t vu0_tpc, vu0_tpc2;
	uint32_t vu0_fbrst, vu0_fbrst2, vu0_fbrst3, vu0_fbrst4;
	uint32_t vu0_itop, vu0_top, vu0_info, vu0_xitop, vu0_pc;
	float vu0_cf[4];

	uint32_t cop0_index, cop0_random, cop0_entrylo0, cop0_entrylo1, cop0_context, cop0_pagemask, cop0_wired,
		cop0_badvaddr, cop0_count, cop0_entryhi, cop0_compare, cop0_status, cop0_cause, cop0_epc, cop0_prid,
		cop0_config, cop0_badpaddr, cop0_debug, cop0_perf, cop0_taglo, cop0_taghi, cop0_errorepc;

	uint32_t llbit, lladdr;
	bool in_delay_slot;
	uint32_t branch_pc;
	uint32_t cop2_ccr[32];

	float f[32];
	float f_acc;
	uint32_t fcr31;
};

inline uint32_t getRegU32(const R5900Context* ctx, int reg)
{
	return (reg <= 0 || reg > 31) ? 0u : static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[reg]));
}

inline void setReturnU32(R5900Context* ctx, uint32_t value)
{
	ctx->r[2] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(value)));
}

inline void setReturnS32(R5900Context* ctx, int32_t value)
{
	ctx->r[2] = _mm_set_epi64x(0, static_cast<int64_t>(value));
}

inline void setReturnU64(R5900Context* ctx, uint64_t value)
{
	ctx->r[2] = _mm_set_epi64x(0, static_cast<int64_t>(value));
	ctx->r[3] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<uint32_t>(value >> 32)));
}

// ---- memory, through PCSX2's map (Crux2NativeHost::vmap)

namespace crux2
{
	extern const Crux2NativeHost* g_host;

	/// Set when the code has read or written a hardware register (the EE's or the GS's registers,
	/// the IOP's memory, the BIOS: physical 0x10000000-0x1FFFFFFF). The run then gives control
	/// back at its next branch back or call, so PCSX2 sees what that access started in time: a
	/// DMA started, a timer read, an interrupt acknowledged. PCSX2's recompiler does the same
	/// (it tests for events at the end of the block). Without it, Black's memory card screen
	/// waited a whole event budget (up to a frame) after each of its many SIF transfers.
	inline bool g_yield = false;

	inline void NoteHandlerAccess(uint32_t a)
	{
		// (0x3xxxxxxx is RAM's uncached-accelerated mirror and 0x7xxxxxxx the scratchpad, whatever
		// their low bits say; Crux2's interpreter check sends those through handlers too.)
		const uint32_t segment = a >> 28;
		if ((a & 0x1FFFFFFFu) >= 0x10000000u && segment != 3 && segment != 7)
			g_yield = true;
	}

	/// The host address of a PS2 address that is plain memory, or nullptr for a handler's page.
	inline uint8_t* Direct(uint32_t address)
	{
		const intptr_t p = static_cast<intptr_t>(g_host->vmap[address >> 12] + address);
		return (p < 0) ? nullptr : reinterpret_cast<uint8_t*>(p);
	}

	inline uint8_t Read8(uint32_t a)
	{
		if (uint8_t* p = Direct(a))
			return *p;
		NoteHandlerAccess(a);
		return g_host->read8(a);
	}
	inline uint16_t Read16(uint32_t a)
	{
		if (uint8_t* p = Direct(a))
		{
			uint16_t v;
			std::memcpy(&v, p, sizeof(v));
			return v;
		}
		NoteHandlerAccess(a);
		return g_host->read16(a);
	}
	inline uint32_t Read32(uint32_t a)
	{
		if (uint8_t* p = Direct(a))
		{
			uint32_t v;
			std::memcpy(&v, p, sizeof(v));
			return v;
		}
		NoteHandlerAccess(a);
		return g_host->read32(a);
	}
	inline uint64_t Read64(uint32_t a)
	{
		if (uint8_t* p = Direct(a))
		{
			uint64_t v;
			std::memcpy(&v, p, sizeof(v));
			return v;
		}
		NoteHandlerAccess(a);
		return g_host->read64(a);
	}
	// LQ and SQ ignore the address's low four bits (the processor aligns them; PCSX2 does too).
	inline __m128i Read128(uint32_t a)
	{
		a &= ~15u;
		if (uint8_t* p = Direct(a))
			return _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
		alignas(16) uint8_t v[16];
		NoteHandlerAccess(a);
		g_host->read128(a, v);
		return _mm_load_si128(reinterpret_cast<const __m128i*>(v));
	}
	inline void Write8(uint32_t a, uint8_t v)
	{
		if (uint8_t* p = Direct(a))
			*p = v;
		else
		{
			NoteHandlerAccess(a);
			g_host->write8(a, v);
		}
	}
	inline void Write16(uint32_t a, uint16_t v)
	{
		if (uint8_t* p = Direct(a))
			std::memcpy(p, &v, sizeof(v));
		else
		{
			NoteHandlerAccess(a);
			g_host->write16(a, v);
		}
	}
	inline void Write32(uint32_t a, uint32_t v)
	{
		if (uint8_t* p = Direct(a))
			std::memcpy(p, &v, sizeof(v));
		else
		{
			NoteHandlerAccess(a);
			g_host->write32(a, v);
		}
	}
	inline void Write64(uint32_t a, uint64_t v)
	{
		if (uint8_t* p = Direct(a))
			std::memcpy(p, &v, sizeof(v));
		else
		{
			NoteHandlerAccess(a);
			g_host->write64(a, v);
		}
	}
	inline void Write128(uint32_t a, __m128i v)
	{
		a &= ~15u;
		if (uint8_t* p = Direct(a))
		{
			_mm_storeu_si128(reinterpret_cast<__m128i*>(p), v);
			return;
		}
		alignas(16) uint8_t b[16];
		_mm_store_si128(reinterpret_cast<__m128i*>(b), v);
		NoteHandlerAccess(a);
		g_host->write128(a, b);
	}
} // namespace crux2

// ---- the runtime the generated code calls

class PS2Runtime
{
public:
	using RecompiledFunction = void (*)(uint8_t*, R5900Context*, PS2Runtime*);

	enum class GuestBranchKind
	{
		DirectJump,
		DirectCall,
		IndirectJump,
		IndirectCall,
		Return,
	};

	/// A call deeper than this gives control back (the rest runs in PCSX2's recompiler).
	static constexpr uint32_t kMaxDepth = 96;

	int64_t count = 0; ///< instructions run (CRUX2_COUNT)
	int64_t budget = 0;
	bool exit_requested = false; ///< CRUX2_EXIT: the next instruction is the emulator's
	uint32_t depth = 0;
	uint32_t traps = 0;

	static RecompiledFunction Lookup(uint32_t pc);

	bool eeCheckpointDue() const { return exit_requested || count >= budget || crux2::g_yield; }

	/// A branch out of the function being run. True: carry on after it (a call whose callee came
	/// back to the return address); false: return, with ctx->pc where to go on.
	bool dispatchGuestBranch(uint8_t* rdram, R5900Context* ctx, uint32_t target, uint32_t /*sourcePc*/, uint32_t returnPc,
		GuestBranchKind kind, const char* /*name*/)
	{
		ctx->pc = target;
		if (kind != GuestBranchKind::DirectCall && kind != GuestBranchKind::IndirectCall)
			return false; // a jump or a return: the run loop goes on from target
		if (exit_requested || count >= budget || crux2::g_yield || depth >= kMaxDepth)
			return false;
		const RecompiledFunction fn = Lookup(target);
		if (!fn)
			return false;
		depth++;
		fn(rdram, ctx, this);
		depth--;
		return !exit_requested && ctx->pc == returnPc;
	}

	// Memory-mapped registers at addresses the converter knew (always through PCSX2's handlers).
	uint8_t Load8(uint8_t*, R5900Context*, uint32_t a) { return crux2::Read8(a); }
	uint16_t Load16(uint8_t*, R5900Context*, uint32_t a) { return crux2::Read16(a); }
	uint32_t Load32(uint8_t*, R5900Context*, uint32_t a) { return crux2::Read32(a); }
	uint64_t Load64(uint8_t*, R5900Context*, uint32_t a) { return crux2::Read64(a); }
	__m128i Load128(uint8_t*, R5900Context*, uint32_t a) { return crux2::Read128(a); }
	void Store8(uint8_t*, R5900Context*, uint32_t a, uint8_t v) { crux2::Write8(a, v); }
	void Store16(uint8_t*, R5900Context*, uint32_t a, uint16_t v) { crux2::Write16(a, v); }
	void Store32(uint8_t*, R5900Context*, uint32_t a, uint32_t v) { crux2::Write32(a, v); }
	void Store64(uint8_t*, R5900Context*, uint32_t a, uint64_t v) { crux2::Write64(a, v); }
	void Store128(uint8_t*, R5900Context*, uint32_t a, __m128i v) { crux2::Write128(a, v); }
	static bool isSpecialAddress(uint32_t) { return false; }

	// A trap condition met (TEQ after a division by zero, mostly): PCSX2's recompiler raises
	// nothing for these either; counted for the log.
	void handleTrap(uint8_t*, R5900Context*) { traps++; }
	void SignalException(R5900Context*, PS2Exception) { traps++; }

	// Never reached in Crux2 mode (the converter hands these instructions back), but compiled.
	void handleSyscall(uint8_t*, R5900Context*, uint32_t) { exit_requested = true; }
	void handleBreak(uint8_t*, R5900Context*) { exit_requested = true; }
	void handleTLBP(uint8_t*, R5900Context*) { exit_requested = true; }
	void handleTLBR(uint8_t*, R5900Context*) { exit_requested = true; }
	void handleTLBWI(uint8_t*, R5900Context*) { exit_requested = true; }
	void handleTLBWR(uint8_t*, R5900Context*) { exit_requested = true; }
	void clearLLBit(R5900Context* ctx) { ctx->llbit = 0; }
	void executeVU0Microprogram(uint8_t*, R5900Context*, uint32_t) { exit_requested = true; }
	void vu0StartMicroProgram(uint8_t*, R5900Context*, uint32_t) { exit_requested = true; }
	void kickGifDmaChainFromMMIO(uint8_t*, R5900Context*, uint32_t) { exit_requested = true; }
};

// The converter's Crux2-mode markers.
#define CRUX2_COUNT(n) (runtime->count += (n))
#define CRUX2_EXIT() (runtime->exit_requested = true)
