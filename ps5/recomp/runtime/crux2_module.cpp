// Crux2 - the frame of a game's native module: what Crux2 calls, around the code PS2Recomp
// generated for the game in its Crux2 mode (ps2_runtime.h has how that code reaches Crux2).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The module has no imports: it is built freestanding (-nostdlib), so the console's loader in
// Crux2 only has to place it and apply its relocations (crux2_module_libc.c has what the compiler
// may call). Its tables come from the build (build-module.py writes crux2_module_info.cpp from the
// game's executable and PS2Recomp's output): each function's range and the hash of its bytes, and
// for each address the converter can enter at, which function that is.

#include "ps2_runtime.h"

extern const uint32_t g_ps2RecompiledFunctionTableBase;
extern const uint32_t g_ps2RecompiledFunctionTableEnd;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount;
extern PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[];

// crux2_module_info.cpp
extern const uint32_t g_crux2FunctionCount;
extern const Crux2NativeFunction g_crux2Functions[]; // by start, ascending
extern const uint32_t g_crux2SlotFunction[]; // per table slot: function index + 1, or 0
extern uint8_t g_crux2FunctionState[]; // 0 not checked, 1 the same, 2 different
extern const char g_crux2Executable[];
extern const char g_crux2MadeBy[];

namespace crux2
{
	const Crux2NativeHost* g_host = nullptr;
}

namespace
{
	Crux2NativeInfo s_info;
	uint32_t s_same = 0, s_different = 0, s_invalidations = 0;

	enum : uint8_t
	{
		NotChecked = 0,
		Same = 1,
		Different = 2,
	};

	// The function's bytes in memory now, against what the module was made from.
	bool Check(const Crux2NativeFunction& f)
	{
		// Watched first, then read: a write after this point reaches crux2_native_invalidate.
		if (crux2::g_host->watch && !crux2::g_host->watch(f.start, f.end - f.start))
			return false;
		uint64_t h = 0xcbf29ce484222325ull;
		for (uint32_t a = f.start; a < f.end; a++)
			h = (h ^ crux2::Read8(a)) * 0x100000001b3ull;
		return h == f.hash;
	}

	void Load(R5900Context& ctx, const Crux2NativeState& st)
	{
		for (int i = 0; i < 32; i++)
			ctx.r[i] = _mm_set_epi64x(static_cast<int64_t>(st.gpr[i][1]), static_cast<int64_t>(st.gpr[i][0]));
		ctx.r[0] = _mm_setzero_si128();
		ctx.hi = st.hi[0];
		ctx.hi1 = st.hi[1];
		ctx.lo = st.lo[0];
		ctx.lo1 = st.lo[1];
		ctx.sa = st.sa;
		ctx.pc = st.pc;
		std::memcpy(ctx.f, st.fpr, sizeof(ctx.f));
		ctx.fcr31 = st.fcr31;
		std::memcpy(&ctx.f_acc, &st.facc, sizeof(ctx.f_acc));
	}

	void Store(Crux2NativeState& st, const R5900Context& ctx)
	{
		for (int i = 0; i < 32; i++)
			_mm_storeu_si128(reinterpret_cast<__m128i*>(st.gpr[i]), ctx.r[i]);
		st.gpr[0][0] = st.gpr[0][1] = 0;
		st.hi[0] = ctx.hi;
		st.hi[1] = ctx.hi1;
		st.lo[0] = ctx.lo;
		st.lo[1] = ctx.lo1;
		st.sa = ctx.sa;
		st.pc = ctx.pc;
		std::memcpy(st.fpr, ctx.f, sizeof(st.fpr));
		st.fcr31 = ctx.fcr31;
		std::memcpy(&st.facc, &ctx.f_acc, sizeof(st.facc));
	}
} // namespace

PS2Runtime::RecompiledFunction PS2Runtime::Lookup(uint32_t pc)
{
	if ((pc & 3u) != 0 || pc < g_ps2RecompiledFunctionTableBase)
		return nullptr;
	const uint32_t slot = (pc - g_ps2RecompiledFunctionTableBase) >> 2;
	if (slot >= g_ps2RecompiledFunctionTableSlotCount)
		return nullptr;
	const uint32_t index = g_crux2SlotFunction[slot];
	if (index == 0)
		return nullptr;
	uint8_t& state = g_crux2FunctionState[index - 1];
	if (state == NotChecked) [[unlikely]]
	{
		const bool same = Check(g_crux2Functions[index - 1]);
		state = same ? Same : Different;
		(same ? s_same : s_different)++;
	}
	return (state == Same) ? g_ps2RecompiledFunctionTable[slot] : nullptr;
}

extern "C" __attribute__((visibility("default"))) const Crux2NativeInfo* crux2_native_init(const Crux2NativeHost* host)
{
	if (!host || host->abi != CRUX2_NATIVE_ABI || host->size < sizeof(Crux2NativeHost))
		return nullptr;
	crux2::g_host = host;
	uint32_t entries = 0;
	for (uint32_t i = 0; i < g_ps2RecompiledFunctionTableSlotCount; i++)
		entries += g_crux2SlotFunction[i] ? 1u : 0u;
	s_info.abi = CRUX2_NATIVE_ABI;
	s_info.size = sizeof(s_info);
	s_info.entries = entries;
	s_info.functions = g_crux2FunctionCount;
	s_info.text_start = g_crux2FunctionCount ? g_crux2Functions[0].start : 0;
	s_info.text_end = g_crux2FunctionCount ? g_crux2Functions[g_crux2FunctionCount - 1].end : 0;
	s_info.executable = g_crux2Executable;
	s_info.made_by = g_crux2MadeBy;
	return &s_info;
}

extern "C" __attribute__((visibility("default"))) int crux2_native_has(uint32_t pc)
{
	return PS2Runtime::Lookup(pc) != nullptr;
}

extern "C" __attribute__((visibility("default"))) void crux2_native_invalidate(uint32_t start, uint32_t bytes)
{
	const uint32_t end = start + bytes;
	// (Functions are by start: the first that may overlap is found by halving.)
	uint32_t lo = 0, hi = g_crux2FunctionCount;
	while (lo < hi)
	{
		const uint32_t mid = (lo + hi) / 2;
		if (g_crux2Functions[mid].end <= start)
			lo = mid + 1;
		else
			hi = mid;
	}
	for (uint32_t i = lo; i < g_crux2FunctionCount && g_crux2Functions[i].start < end; i++)
	{
		if (g_crux2Functions[i].end > start && g_crux2FunctionState[i] != NotChecked)
		{
			g_crux2FunctionState[i] = NotChecked;
			s_invalidations++;
		}
	}
}

extern "C" __attribute__((visibility("default"))) void crux2_native_stats(uint32_t* same, uint32_t* different, uint32_t* invalidations)
{
	*same = s_same;
	*different = s_different;
	*invalidations = s_invalidations;
}

extern "C" __attribute__((visibility("default"))) uint32_t crux2_native_run(Crux2NativeState* st)
{
	alignas(16) R5900Context ctx;
	std::memset(&ctx, 0, sizeof(ctx));
	Load(ctx, *st);
	PS2Runtime runtime;
	runtime.budget = st->budget > 0 ? st->budget : 1;
	crux2::g_yield = false;

	uint32_t reason = CRUX2_NATIVE_EXIT_NOT_NATIVE;
	for (;;)
	{
		if (runtime.exit_requested)
		{
			reason = CRUX2_NATIVE_EXIT_HOST;
			break;
		}
		if (runtime.count >= runtime.budget || crux2::g_yield)
		{
			reason = CRUX2_NATIVE_EXIT_BUDGET;
			break;
		}
		const PS2Runtime::RecompiledFunction fn = PS2Runtime::Lookup(ctx.pc);
		if (!fn)
			break;
		fn(nullptr, &ctx, &runtime);
	}

	Store(*st, ctx);
	st->count = runtime.count;
	st->exit_reason = reason;
	st->traps = runtime.traps;
	return reason;
}
