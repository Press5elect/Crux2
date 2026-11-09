// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
// Crux2 - the x86 recompilers' emitted SQRT kernel (X86SoftFloatEmitter::EmitEESqrtKernel, the EE
// FPU's SQRT.S and the square root of RSQRT.S) run on the PC against PS2Float::Sqrt, bit for bit,
// flags included.
//
//   softfloat_kernel_test
//
// The kernel is emitted with PCSX2's own x86 emitter into executable memory and called through its
// internal ABI (eax in; eax = result, edx = flags out). Both forms it can take are emitted: the one
// that loads a chop-mode MXCSR around its SQRTSS (the EE's rounding mode is not chop) and the one
// that relies on the caller's MXCSR (it is). Every 2^23 mantissa is run at a spread of exponents
// covering both parities, the top binade (the extended input that always runs the recurrence),
// exponent 0 (zeros and denormals), both signs, and on both forms.
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include <sys/mman.h>
#include <xmmintrin.h>
#include "common/Pcsx2Defs.h"
#include "PS2Float.h"
#include "x86/SoftFloatEmitter.h"

void Log::Writev(LOGLEVEL, ConsoleColors, const char* fmt, va_list ap) { std::vfprintf(stderr, fmt, ap); std::fputc('\n', stderr); }
void pxOnAssertFail(const char* file, int line, const char* func, const char* msg)
{
	std::fprintf(stderr, "assertion failed: %s:%d %s: %s\n", file, line, func, msg ? msg : "");
	std::abort();
}

static std::atomic<u64> g_fail{0};
static std::atomic<u64> g_checked{0};
static unsigned g_threads = 4;

#define REPORT(...) do { if (g_fail.fetch_add(1) < 20) { std::fprintf(stderr, "FAIL: " __VA_ARGS__); std::fputc('\n', stderr); } } while (0)

template <class F>
static void parallel(F fn)
{
	std::vector<std::thread> ts;
	for (unsigned t = 0; t < g_threads; t++)
		ts.emplace_back([&, t] { fn(t); });
	for (auto& t : ts)
		t.join();
}

// The kernel's internal ABI: eax = raw radicand; returns eax = raw root, edx = flags (8 = invalid).
// It clobbers rax, rcx, rdx and r8-r11 and saves rbp, rsi, rdi and xmm0 itself. The call is made
// below the red zone, since the kernel builds its frame on the caller's stack.
static __fi u32 callKernel(const void* fn, u32 raw, u32* flags)
{
	u32 result, fl;
	asm volatile(
		"sub $128, %%rsp\n\t"
		"call *%[fn]\n\t"
		"add $128, %%rsp"
		: "=a"(result), "=d"(fl)
		: "a"(raw), [fn] "r"(fn)
		: "rcx", "r8", "r9", "r10", "r11", "memory", "cc");
	*flags = fl;
	return result;
}

alignas(16) static const u32 s_abs_mask[4] = {0x7fffffff, 0xffffffff, 0xffffffff, 0xffffffff};
static u32 s_mxcsr_chop, s_mxcsr_nearest;

int main(int argc, char** argv)
{
	setvbuf(stdout, nullptr, _IOLBF, 0);
	if (const char* t = std::getenv("SOFTFLOAT_THREADS")) g_threads = std::atoi(t);

	// The emitter encodes ptr32[&variable] as a 32-bit displacement, which PCSX2 guarantees by keeping
	// its code buffer within 2 GiB of its data; a plain mmap lands far from this program's statics
	// and the truncated address would fault. Map the code near them.
	void* code = MAP_FAILED;
	for (uptr hint = (reinterpret_cast<uptr>(&s_mxcsr_chop) & ~uptr(0xFFFF)) + (64 << 20);
	     code == MAP_FAILED && hint < reinterpret_cast<uptr>(&s_mxcsr_chop) + (1u << 30); hint += 64 << 20)
	{
		void* const m = mmap(reinterpret_cast<void*>(hint), 1 << 16, PROT_READ | PROT_WRITE | PROT_EXEC,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (m == MAP_FAILED)
			continue;
		const sptr distance = reinterpret_cast<sptr>(m) - reinterpret_cast<sptr>(&s_mxcsr_chop);
		if (distance > -(sptr(1) << 30) && distance < (sptr(1) << 30))
			code = m;
		else
			munmap(m, 1 << 16);
	}
	if (code == MAP_FAILED) { std::fprintf(stderr, "no executable memory within 1 GiB of the data\n"); return 2; }
	s_mxcsr_nearest = (_mm_getcsr() & ~0x6000u);
	s_mxcsr_chop = s_mxcsr_nearest | 0x6000u;
	x86Emitter::xSetPtr(code);
	const void* const kernel_switching = X86SoftFloatEmitter::EmitEESqrtKernel(s_abs_mask, &s_mxcsr_chop, &s_mxcsr_nearest);
	const void* const kernel_chop = X86SoftFloatEmitter::EmitEESqrtKernel(s_abs_mask, nullptr, nullptr);
	std::printf("emitted %d bytes of kernel code\n", (int)(x86Emitter::xGetPtr() - (u8*)code));

	// exponents: 0 (zero/denormal), both parities low, around 1.0, both parities high, 254, 255 (extended)
	const u32 exps[] = {0, 1, 2, 3, 4, 63, 64, 125, 126, 127, 128, 129, 200, 201, 252, 253, 254, 255};
	for (int form = 0; form < 2; form++)
	{
		const void* const kernel = form ? kernel_chop : kernel_switching;
		std::printf("%s form: all 2^23 mantissas x %zu exponents x 2 signs...\n",
			form ? "caller-chop" : "MXCSR-switching", sizeof(exps) / sizeof(exps[0]));
		parallel([&](unsigned t) {
			const unsigned saved = _mm_getcsr();
			_mm_setcsr(form ? s_mxcsr_chop : s_mxcsr_nearest);
			u64 n = 0;
			for (u32 e : exps)
				for (u32 sign = 0; sign < 2; sign++)
					for (u32 man = t; man < 0x800000; man += g_threads)
					{
						const u32 raw = (sign << 31) | (e << 23) | man;
						u32 flags;
						const u32 got = callKernel(kernel, raw, &flags);
						const PS2Float want = PS2Float(raw).Sqrt();
						const u32 want_flags = want.HasInvalid() ? 8u : 0u;
						if (got != want.raw || flags != want_flags)
							REPORT("sqrt kernel (%s) %08x: want %08x/%x got %08x/%x", form ? "chop" : "switching",
								raw, want.raw, want_flags, got, flags);
						n++;
					}
			// the kernel must leave MXCSR's control bits as it found them (its SQRTSS sets the sticky
			// inexact status bit, as the real one does)
			if ((_mm_getcsr() & ~0x3Fu) != ((form ? s_mxcsr_chop : s_mxcsr_nearest) & ~0x3Fu))
				REPORT("sqrt kernel (%s) changed MXCSR to %08x", form ? "chop" : "switching", _mm_getcsr());
			_mm_setcsr(saved);
			g_checked += n;
		});
	}
	std::printf("%lu comparisons, %lu failures\n", (unsigned long)g_checked.load(), (unsigned long)g_fail.load());
	return g_fail ? 1 : 0;
}
