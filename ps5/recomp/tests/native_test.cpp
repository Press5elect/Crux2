// Crux2 - a native module on the PC, against a stand-in for PCSX2: flat PS2 memory behind a map
// of the same shape as PCSX2's (vtlb), the module loaded by Crux2's own loader, functions of a
// test program run natively and their results checked.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
//   native_test <module> <the test program's ELF> <fib> <strlen> <memcpy> <strcpy> <main>
//
// The test program (ps5/recomp/tests/program/main.c) is made with the open-source PS2 SDK: no
// game is involved.

#include "crux2_native_abi.h"
#include "ps5_native_loader.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

static int g_failed = 0, g_checks = 0;
#define EXPECT(c, ...)                         \
	do                                         \
	{                                          \
		g_checks++;                            \
		if (!(c))                              \
		{                                      \
			g_failed++;                        \
			std::printf("FAILED: " __VA_ARGS__); \
			std::printf(" (line %d)\n", __LINE__); \
		}                                      \
	} while (0)

namespace
{
	constexpr uint32_t kRam = 32u * 1024u * 1024u;
	alignas(4096) uint8_t* g_ram = nullptr;
	std::vector<uintptr_t> g_vmap;
	uint32_t g_handler_reads = 0, g_handler_writes = 0;

	uint8_t* Host(uint32_t a) { return g_ram + (a & (kRam - 1)); }

	uint8_t R8(uint32_t a) { g_handler_reads++; return 0; }
	uint16_t R16(uint32_t a) { g_handler_reads++; return 0; }
	uint32_t R32(uint32_t a) { g_handler_reads++; return 0; }
	uint64_t R64(uint32_t a) { g_handler_reads++; return 0; }
	void R128(uint32_t a, void* out) { g_handler_reads++; std::memset(out, 0, 16); }
	void W8(uint32_t, uint8_t) { g_handler_writes++; }
	void W16(uint32_t, uint16_t) { g_handler_writes++; }
	void W32(uint32_t, uint32_t) { g_handler_writes++; }
	void W64(uint32_t, uint64_t) { g_handler_writes++; }
	void W128(uint32_t, const void*) { g_handler_writes++; }
	void Log(const char* line) { std::printf("module: %s\n", line); }

	// Crux2's write watch: here only counted, and refused for one range to see it respected.
	uint32_t g_watched = 0, g_refuse_start = 0xFFFFFFFFu;
	int Watch(uint32_t start, uint32_t bytes)
	{
		g_watched++;
		return (start <= g_refuse_start && g_refuse_start < start + bytes) ? 0 : 1;
	}

	void MapRam(uint32_t base)
	{
		for (uint32_t page = 0; page < kRam / 4096; page++)
			g_vmap[(base >> 12) + page] = reinterpret_cast<uintptr_t>(g_ram) - base;
	}

	bool LoadElf(const char* path, std::vector<uint8_t>* text_copy)
	{
		std::FILE* f = std::fopen(path, "rb");
		if (!f)
			return false;
		std::vector<uint8_t> d;
		uint8_t b[65536];
		size_t n;
		while ((n = std::fread(b, 1, sizeof(b), f)) > 0)
			d.insert(d.end(), b, b + n);
		std::fclose(f);
		uint32_t phoff;
		uint16_t phentsize, phnum;
		std::memcpy(&phoff, &d[28], 4);
		std::memcpy(&phentsize, &d[42], 2);
		std::memcpy(&phnum, &d[44], 2);
		for (uint16_t i = 0; i < phnum; i++)
		{
			uint32_t h[8];
			std::memcpy(h, &d[phoff + i * phentsize], sizeof(h));
			if (h[0] != 1)
				continue;
			std::memcpy(Host(h[2]), &d[h[1]], h[4]);
		}
		(void)text_copy;
		return true;
	}

	struct Api
	{
		Crux2NativeInitFn init;
		Crux2NativeHasFn has;
		Crux2NativeRunFn run;
		Crux2NativeInvalidateFn invalidate;
		Crux2NativeStatsFn stats;
	};

	constexpr uint32_t kReturnTo = 0x00F00000u; // an address with no native code: "back in PCSX2"
	constexpr uint32_t kStack = 0x01F00000u;

	// Calls a function natively, as PCSX2 would reach it: from its start with $ra set, run again
	// from wherever it gives control back (it must be an address with native code: there is no
	// interpreter here) until it returns. `budget` small makes it give control back often.
	bool Call(const Api& api, Crux2NativeState& st, uint32_t function, int64_t budget, uint32_t* runs, uint32_t* stuck_pc)
	{
		st.gpr[31][0] = kReturnTo;
		st.gpr[29][0] = kStack;
		st.pc = function;
		*runs = 0;
		for (;;)
		{
			if (st.pc == kReturnTo)
				return true;
			if (!api.has(st.pc))
			{
				*stuck_pc = st.pc;
				return false;
			}
			st.budget = budget;
			api.run(&st);
			(*runs)++;
			if (*runs > 10000000)
			{
				*stuck_pc = st.pc;
				return false;
			}
		}
	}

	uint32_t Fib(uint32_t n) { return n < 2 ? n : Fib(n - 1) + Fib(n - 2); }
} // namespace

int main(int argc, char** argv)
{
	if (argc < 8)
	{
		std::fprintf(stderr, "usage: native_test <module> <elf> <fib> <strlen> <memcpy> <strcpy> <main>\n");
		return 2;
	}
	const uint32_t fib = std::strtoul(argv[3], nullptr, 16), strlen_fn = std::strtoul(argv[4], nullptr, 16),
				   memcpy_fn = std::strtoul(argv[5], nullptr, 16), strcpy_fn = std::strtoul(argv[6], nullptr, 16),
				   main_fn = std::strtoul(argv[7], nullptr, 16);

	std::setvbuf(stdout, nullptr, _IOLBF, 0);
	g_ram = static_cast<uint8_t*>(std::aligned_alloc(4096, kRam));
	std::memset(g_ram, 0, kRam);
	g_vmap.assign(1u << 20, uintptr_t{1} << 63); // every page a handler's, then RAM and its mirrors
	MapRam(0x00000000u);
	MapRam(0x20000000u);
	MapRam(0x30000000u);
	MapRam(0x80000000u);
	EXPECT(LoadElf(argv[2], nullptr), "the test program loads");

	std::string error;
	std::unique_ptr<PS5NativeLoader::Module> module = PS5NativeLoader::LoadFile(argv[1], &error);
	EXPECT(module != nullptr, "the module loads: %s", error.c_str());
	if (!module)
		return 1;
	Api api{reinterpret_cast<Crux2NativeInitFn>(module->Symbol(CRUX2_NATIVE_INIT_SYMBOL)),
		reinterpret_cast<Crux2NativeHasFn>(module->Symbol(CRUX2_NATIVE_HAS_SYMBOL)),
		reinterpret_cast<Crux2NativeRunFn>(module->Symbol(CRUX2_NATIVE_RUN_SYMBOL)),
		reinterpret_cast<Crux2NativeInvalidateFn>(module->Symbol(CRUX2_NATIVE_INVALIDATE_SYMBOL)),
		reinterpret_cast<Crux2NativeStatsFn>(module->Symbol(CRUX2_NATIVE_STATS_SYMBOL))};
	EXPECT(api.init && api.has && api.run && api.invalidate && api.stats, "the module's five functions are found");
	EXPECT(module->Symbol("memcpy") == nullptr, "nothing but the five is exported");

	Crux2NativeHost host{};
	host.abi = CRUX2_NATIVE_ABI;
	host.size = sizeof(host);
	host.vmap = g_vmap.data();
	host.read8 = R8;
	host.read16 = R16;
	host.read32 = R32;
	host.read64 = R64;
	host.read128 = R128;
	host.write8 = W8;
	host.write16 = W16;
	host.write32 = W32;
	host.write64 = W64;
	host.write128 = W128;
	host.log = Log;
	host.watch = Watch;
	const Crux2NativeInfo* info = api.init(&host);
	EXPECT(info && info->functions > 100 && info->entries > info->functions, "the module describes itself");
	if (!info)
		return 1;
	std::printf("module: %s, %u functions, %u entries, 0x%x-0x%x, %s; %zu KiB loaded\n", info->executable, info->functions, info->entries,
		info->text_start, info->text_end, info->made_by, module->Size() / 1024);
	EXPECT(api.has(fib) && api.has(strlen_fn) && api.has(memcpy_fn) && api.has(main_fn), "the test's functions have native code");
	EXPECT(!api.has(kReturnTo) && !api.has(fib + 2), "no native code where there is none");

	Crux2NativeState st{};
	uint32_t runs = 0, stuck = 0;

	// A recursive function, all in one run.
	st.gpr[4][0] = 20;
	EXPECT(Call(api, st, fib, 1ll << 40, &runs, &stuck) && runs == 1, "fib(20) runs natively in one go (runs %u, stopped at 0x%x)", runs, stuck);
	EXPECT(static_cast<uint32_t>(st.gpr[2][0]) == 6765, "fib(20) = 6765 (got %u)", static_cast<uint32_t>(st.gpr[2][0]));
	EXPECT(st.exit_reason == CRUX2_NATIVE_EXIT_NOT_NATIVE && st.count > 1000, "it ends by returning to PCSX2, having counted %lld instructions",
		static_cast<long long>(st.count));

	// The same, giving control back every 50 instructions (as when PCSX2's events are due), and
	// carried on from where it stopped each time.
	std::memset(&st, 0, sizeof(st));
	st.gpr[4][0] = 22;
	const bool done = Call(api, st, fib, 50, &runs, &stuck);
	EXPECT(done, "fib(22) in slices of 50 instructions carries on from every place it stops (stuck at 0x%x after %u runs)", stuck, runs);
	if (done)
		EXPECT(static_cast<uint32_t>(st.gpr[2][0]) == Fib(22) && runs > 100, "fib(22) = %u in %u slices (got %u)", Fib(22), runs,
			static_cast<uint32_t>(st.gpr[2][0]));

	// Library code: strlen, memcpy and strcpy of newlib, on memory.
	const char* text = "recompiled for the PS5";
	std::memcpy(Host(0x00E00000u), text, std::strlen(text) + 1);
	std::memset(&st, 0, sizeof(st));
	st.gpr[4][0] = 0x00E00000u;
	EXPECT(Call(api, st, strlen_fn, 1ll << 40, &runs, &stuck) && st.gpr[2][0] == std::strlen(text), "strlen (got %llu)",
		static_cast<unsigned long long>(st.gpr[2][0]));
	std::vector<uint8_t> pattern(4099);
	for (size_t i = 0; i < pattern.size(); i++)
		pattern[i] = static_cast<uint8_t>(i * 7 + 3);
	std::memcpy(Host(0x00E10000u), pattern.data(), pattern.size());
	std::memset(&st, 0, sizeof(st));
	st.gpr[4][0] = 0x00E20001u;
	st.gpr[5][0] = 0x00E10000u;
	st.gpr[6][0] = pattern.size();
	EXPECT(Call(api, st, memcpy_fn, 1ll << 40, &runs, &stuck) && std::memcmp(Host(0x00E20001u), pattern.data(), pattern.size()) == 0 &&
			   st.gpr[2][0] == 0x00E20001u,
		"memcpy of 4099 bytes to an odd address");
	std::memset(&st, 0, sizeof(st));
	st.gpr[4][0] = 0x00E30000u;
	st.gpr[5][0] = 0x00E00000u;
	EXPECT(Call(api, st, strcpy_fn, 1ll << 40, &runs, &stuck) && std::strcmp(reinterpret_cast<char*>(Host(0x00E30000u)), text) == 0,
		"strcpy");

	// The executable's main: it computes, then calls into the C library, which reaches a system
	// call (the emulator's to run): control must come back there, before it.
	std::memset(&st, 0, sizeof(st));
	st.gpr[31][0] = kReturnTo;
	st.gpr[29][0] = kStack;
	st.pc = main_fn;
	st.budget = 1ll << 40;
	const uint32_t reason = api.run(&st);
	uint32_t at;
	std::memcpy(&at, Host(st.pc), 4);
	std::printf("main: %lld instructions natively, then %s at 0x%08x (instruction 0x%08x)\n", static_cast<long long>(st.count),
		reason == CRUX2_NATIVE_EXIT_HOST ? "an instruction for the emulator" : reason == CRUX2_NATIVE_EXIT_BUDGET ? "the budget" : "no native code",
		st.pc, at);
	EXPECT(st.count > 100000, "main runs its loops natively (%lld instructions)", static_cast<long long>(st.count));
	EXPECT(reason == CRUX2_NATIVE_EXIT_HOST || reason == CRUX2_NATIVE_EXIT_NOT_NATIVE, "main stops where the emulator takes over");
	if (reason == CRUX2_NATIVE_EXIT_HOST)
		EXPECT((at & 0xFC00003Fu) == 0x0000000Cu || (at >> 26) == 0x10 || (at >> 26) == 0x12 || (at >> 26) == 0x2F,
			"it stopped before a system call, COP0, COP2 or CACHE instruction (0x%08x)", at);

	// A function whose bytes change is not run natively any more; put back, it is again.
	uint32_t same = 0, different = 0, invalidations = 0;
	api.stats(&same, &different, &invalidations);
	EXPECT(different == 0 && same > 0, "every function checked was the same (%u same, %u different)", same, different);
	uint32_t word;
	std::memcpy(&word, Host(fib + 8), 4);
	const uint32_t changed = word ^ 1u;
	std::memcpy(Host(fib + 8), &changed, 4);
	api.invalidate(fib + 8, 4);
	EXPECT(!api.has(fib), "a changed function has no native code");
	std::memcpy(Host(fib + 8), &word, 4);
	api.invalidate(fib + 8, 4);
	EXPECT(api.has(fib), "put back, it has again");
	api.stats(&same, &different, &invalidations);
	EXPECT(different == 1 && invalidations == 2, "the stats say so (%u different, %u invalidations)", different, invalidations);

	// A function Crux2 cannot watch for writes is left to the emulator.
	EXPECT(g_watched >= same, "every function checked was watched first (%u watched)", g_watched);
	g_refuse_start = fib;
	api.invalidate(fib, 4);
	EXPECT(!api.has(fib), "a function that cannot be watched has no native code");
	g_refuse_start = 0xFFFFFFFFu;
	api.invalidate(fib, 4);
	EXPECT(api.has(fib), "watchable again, it has");

	EXPECT(g_handler_reads == 0 && g_handler_writes == 0, "nothing reached the hardware's handlers (%u reads, %u writes)", g_handler_reads,
		g_handler_writes);

	std::printf("native: %d checks, %d failed\n", g_checks, g_failed);
	module.reset();
	std::free(g_ram);
	return g_failed ? 1 : 0;
}
