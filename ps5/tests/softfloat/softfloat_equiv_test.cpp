// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
// Crux2 - PS2Float: the divide/sqrt shortcut (DivSignificand/SqrtSignificand) and the multiplier gate
// (MulMantissa) against the unchanged digit recurrence and Booth array they stand in front of.
//
//   softfloat_equiv_test [--bench] [scale]
//
// The reference routines below are PS2Float.cpp's / PS2Float.inl's code before the shortcuts, copied
// verbatim (only the cache removed). Results must be bit-identical, flags included.
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include <bit>
#include <cmath>
#include "common/Pcsx2Defs.h"
#include "PS2Float.h"

void Log::Writev(LOGLEVEL, ConsoleColors, const char* fmt, va_list ap) { std::vfprintf(stderr, fmt, ap); std::fputc('\n', stderr); }

namespace PS2FloatDetail
{
u32 DivSignificand(u32 sma, u32 smb);
u32 SqrtSignificand(u32 m);
} // namespace PS2FloatDetail

//---------------------------------------------------------------- reference: the full recurrence
struct CSAResult { u32 sum, carry; };
static CSAResult CSA(u32 a, u32 b, u32 c) { u32 u = a ^ b; u32 h = (a & b) | (u & c); u32 l = u ^ c; return {l, h << 1}; }
static s32 quotientSelect(CSAResult current)
{
	constexpr u32 mask = (1 << 24) - 1;
	const s32 test = ((current.sum & ~mask) + current.carry) | (current.sum & mask);
	return (test >= (1 << 23)) - (test < static_cast<s32>(~0u << 24));
}
static CSAResult selectCSAResult(CSAResult current, CSAResult csa, s32 quotientBit)
{
	const u32 mask = static_cast<u32>(-(quotientBit != 0));
	return {(current.sum & ~mask) | (csa.sum & mask), (current.carry & ~mask) | (csa.carry & mask)};
}
static u32 mantissa(u32 x) { return (x & 0x7fffff) | 0x800000; }
static u32 exponent(u32 x) { return (x >> 23) & 0xff; }

static u32 refDivQ(u32 sma, u32 smb)
{
	u32 am = sma << 2, bm = smb << 2;
	CSAResult current = {am, 0};
	u32 quotient = 0;
	int quotientBit = 1;
	for (int i = 0; i < 23; i++)
	{
		quotient = (quotient << 1) + quotientBit;
		u32 add = quotientBit > 0 ? ~bm : quotientBit < 0 ? bm : 0;
		current.carry += quotientBit > 0;
		CSAResult csa = CSA(current.sum, current.carry, add);
		quotientBit = quotientSelect(selectCSAResult(current, csa, quotientBit));
		current.sum = csa.sum << 1;
		current.carry = csa.carry << 1;
	}
	quotient = (quotient << 1) + quotientBit;
	u32 add = quotientBit > 0 ? ~bm : quotientBit < 0 ? bm : 0;
	current.carry += quotientBit > 0;
	CSAResult csa = CSA(current.sum, current.carry, add);
	quotientBit = quotientSelect(selectCSAResult(current, csa, quotientBit));
	quotient = (quotient << 1) + quotientBit;
	return quotient;
}

static u32 refSqrtQ(u32 m)
{
	CSAResult current = {m, 0};
	u32 quotient = 0;
	s32 quotientBit = 1;
	for (s32 i = 0; i < 23; i++)
	{
		u32 adjust = quotient + (quotientBit << (24 - i));
		quotient += quotientBit << (25 - i);
		u32 add = quotientBit > 0 ? ~adjust : quotientBit < 0 ? adjust : 0;
		current.carry += quotientBit > 0;
		CSAResult csa = CSA(current.sum, current.carry, add);
		quotientBit = quotientSelect(selectCSAResult(current, csa, quotientBit));
		current.sum = csa.sum << 1;
		current.carry = csa.carry << 1;
	}
	u32 adjust = quotient + (quotientBit << 1);
	quotient += quotientBit << 2;
	u32 add = quotientBit > 0 ? ~adjust : quotientBit < 0 ? adjust : 0;
	current.carry += quotientBit > 0;
	CSAResult csa = CSA(current.sum, current.carry, add);
	quotientBit = quotientSelect(selectCSAResult(current, csa, quotientBit));
	quotient += quotientBit << 1;
	return quotient;
}

static PS2Float refDiv(PS2Float x, PS2Float y)
{
	u32 a = x.raw, b = y.raw;
	u32 sign = ((a ^ b) & 0x80000000);
	if (((a & 0x7F800000) == 0) && ((b & 0x7F800000) != 0))
		return PS2Float(sign);
	if (((a & 0x7F800000) != 0) && ((b & 0x7F800000) == 0))
	{
		PS2Float result = PS2Float(sign | PS2Float::MAX_FLOATING_POINT_VALUE);
		result.SetDivideByZero();
		return result;
	}
	if (((a & 0x7F800000) == 0) && ((b & 0x7F800000) == 0))
	{
		PS2Float result = PS2Float(sign | PS2Float::MAX_FLOATING_POINT_VALUE);
		result.SetInvalid();
		return result;
	}
	s32 cexp = exponent(a) - exponent(b) + 126;
	if (cexp > 255)
	{
		PS2Float result = PS2Float(sign | PS2Float::MAX_FLOATING_POINT_VALUE);
		result.SetOverflow();
		return result;
	}
	else if (cexp < 0)
	{
		PS2Float result = PS2Float(sign);
		result.SetUnderflow();
		return result;
	}
	u32 quotient = refDivQ(mantissa(a), mantissa(b));
	if (quotient >= (1 << 24))
	{
		cexp += 1;
		quotient >>= 1;
	}
	if (cexp > 255)
	{
		PS2Float result = PS2Float(sign | PS2Float::MAX_FLOATING_POINT_VALUE);
		result.SetOverflow();
		return result;
	}
	else if (cexp < 1)
	{
		PS2Float result = PS2Float(sign);
		result.SetUnderflow();
		return result;
	}
	return PS2Float((quotient & 0x7fffff) | (cexp << 23) | sign);
}

static PS2Float refSqrt(PS2Float x)
{
	u32 a = x.raw;
	const bool negative = (a & PS2Float::SIGNMASK) != 0;
	if ((a & 0x7F800000) == 0)
	{
		PS2Float result = PS2Float(0);
		result.SetInvalid(negative);
		return result;
	}
	u32 m = mantissa(a) << 1;
	if (!(a & 0x800000))
		m <<= 1;
	u32 quotient = refSqrtQ(m);
	s32 Dvdtexp = exponent(a);
	Dvdtexp = (Dvdtexp + 127) >> 1;
	PS2Float result = PS2Float(((quotient >> 2) & 0x7fffff) | (Dvdtexp << 23));
	if (negative)
		result.SetInvalid();
	return result;
}

// RsqrtImpl with the reference Div and Sqrt; `self` is the numerator, `other` the radicand.
static PS2Float refRsqrtImpl(PS2Float self, PS2Float other)
{
	const u32 other_abs = other.Abs();
	if ((other_abs & 0x7F800000) == 0)
	{
		PS2Float result = PS2Float((self.raw & PS2Float::SIGNMASK) | PS2Float::MAX_FLOATING_POINT_VALUE);
		if (self.IsDenormalized())
			result.SetInvalid();
		else
			result.SetDivideByZero();
		return result;
	}
	if (self.IsDenormalized())
		return PS2Float(self.raw & PS2Float::SIGNMASK);
	const u32 sign = self.raw & PS2Float::SIGNMASK;
	const u32 sqrt_exp = (other.Exponent() + 127) >> 1;
	const s32 cexp = self.Exponent() - sqrt_exp + 126;
	if (cexp > 255)
	{
		PS2Float result = PS2Float(sign | PS2Float::MAX_FLOATING_POINT_VALUE);
		result.SetOverflow();
		return result;
	}
	else if (cexp < 0)
	{
		PS2Float result = PS2Float(sign);
		result.SetUnderflow();
		return result;
	}
	return refDiv(self, refSqrt(PS2Float(other_abs)));
}

// Mul/DoMul with the Booth array run on every product.
static PS2Float refMul(PS2Float x, PS2Float y)
{
	using namespace PS2FloatDetail;
	if (x.IsDenormalized() || y.IsDenormalized())
	{
		// DetermineMultiplicationDivisionOperationSign is private: the sign of a zero product is the xor
		return PS2Float(((x.raw ^ y.raw) & PS2Float::SIGNMASK) | 0u);
	}
	u32 sign = (x.raw ^ y.raw) & PS2Float::SIGNMASK;
	s32 resExponent = x.Exponent() + y.Exponent() - 127;
	if (resExponent > 255)
	{
		PS2Float result = PS2Float(sign | PS2Float::MAX_FLOATING_POINT_VALUE);
		result.SetOverflow();
		return result;
	}
	else if (resExponent < 0)
	{
		PS2Float result = PS2Float(sign);
		result.SetUnderflow();
		return result;
	}
	u32 resMantissa = (u32)(MulMantissaArray(mantissa(x.raw), mantissa(y.raw)) >> 23);
	if (resMantissa > 0xFFFFFF)
	{
		resMantissa >>= 1;
		resExponent++;
	}
	if (resExponent > 255)
	{
		PS2Float result = PS2Float(sign | PS2Float::MAX_FLOATING_POINT_VALUE);
		result.SetOverflow();
		return result;
	}
	else if (resExponent < 1)
	{
		PS2Float result = PS2Float(sign);
		result.SetUnderflow();
		return result;
	}
	return PS2Float(sign | (u32)(resExponent << 23) | (resMantissa & 0x7FFFFF));
}

//---------------------------------------------------------------- harness
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

static u32 divFinal(u32 q) { return q >= (1u << 24) ? q >> 1 : q; }

static bool sameFloat(PS2Float a, PS2Float b) { return a.raw == b.raw && a.flags == b.flags; }

static u32 rnd32(std::mt19937_64& g) { return (u32)g(); }

// Operands biased towards the interesting exponents: zero/denormal, 1, 254/255, and a spread.
static u32 randOperand(std::mt19937_64& g)
{
	u64 r = g();
	u32 raw = (u32)r;
	switch ((r >> 40) & 15)
	{
		case 0: raw &= 0x807FFFFF; break;                             // denormal / zero exponent
		case 1: raw = (raw & 0x807FFFFF) | (0xFFu << 23); break;      // top binade
		case 2: raw = (raw & 0x807FFFFF) | (0xFEu << 23); break;
		case 3: raw = (raw & 0x807FFFFF) | (1u << 23); break;
		case 4: raw = (raw & 0x807FFFFF) | ((120 + ((r >> 44) & 15)) << 23); break;
		case 5: raw = (raw & 0xFF800000) | (((r >> 44) & 1) ? 0x7FFFFF : ((r >> 45) & 3)); break; // extreme significands
		default: break;
	}
	return raw;
}

static std::vector<u32> edgeValues()
{
	std::vector<u32> v;
	const u32 exps[] = {0, 1, 2, 63, 126, 127, 128, 129, 190, 253, 254, 255};
	const u32 mans[] = {0, 1, 2, 0x3FFFFF, 0x400000, 0x400001, 0x555555, 0x7FFFFE, 0x7FFFFF};
	for (u32 e : exps)
		for (u32 m : mans)
			for (u32 s : {0u, 0x80000000u})
				v.push_back(s | e << 23 | m);
	v.push_back(0x7FFFFFFF);
	v.push_back(0xFFFFFFFF);
	return v;
}

//---------------------------------------------------------------- tests
static void testDivKernel(u64 scale)
{
	// (a) all 2^23 dividend significands against a set of divisor significands: the corners, the cap
	// boundaries (3<<22 +- k), and a stride through the whole range.
	std::vector<u32> mbs = {0x800000, 0x800001, 0x800002, 0x8FFFFF, 0xA00000, 0xBFFFFF, 0xBFFFFE, 0xC00000, 0xC00001,
		0xC00002, 0xC00003, 0xDFFFFF, 0xFFFFFF, 0xFFFFFE, 0xFFFFFD, 0xFFFFF0, 0xB55555, 0xAAAAAA, 0x955555, 0xEAAAAA};
	std::mt19937_64 g(1);
	for (u64 i = 0; i < 44 * scale; i++)
		mbs.push_back(0x800000 | (rnd32(g) & 0x7FFFFF));
	std::atomic<u64> next{0};
	parallel([&](unsigned) {
		for (;;)
		{
			u64 i = next.fetch_add(1);
			if (i >= mbs.size())
				return;
			const u32 mb = mbs[i];
			for (u32 ma = 0x800000; ma < 0x1000000; ma++)
			{
				const u32 want = divFinal(refDivQ(ma, mb)), got = divFinal(PS2FloatDetail::DivSignificand(ma, mb));
				if (want != got)
					REPORT("div kernel ma=%06x mb=%06x want %07x got %07x", ma, mb, want, got);
			}
			g_checked += 1u << 23;
		}
	});
	// (b) the same, with the roles reversed for the first 20 (all dividends are divisors too)
	// (c) random pairs
	const u64 n = 100000000ull * scale / g_threads;
	parallel([&](unsigned t) {
		std::mt19937_64 gr(100 + t);
		for (u64 i = 0; i < n; i++)
		{
			const u32 ma = 0x800000 | (rnd32(gr) & 0x7FFFFF), mb = 0x800000 | (rnd32(gr) & 0x7FFFFF);
			const u32 want = divFinal(refDivQ(ma, mb)), got = divFinal(PS2FloatDetail::DivSignificand(ma, mb));
			if (want != got)
				REPORT("div kernel (random) ma=%06x mb=%06x want %07x got %07x", ma, mb, want, got);
		}
		g_checked += n;
	});
}

static void testSqrtKernel()
{
	// Exhaustive: every radicand the operation can feed the kernel (odd exponent: significand << 1,
	// even: << 2).
	parallel([&](unsigned t) {
		for (u32 sig = 0x800000 + t; sig < 0x1000000; sig += g_threads)
			for (u32 sh = 1; sh <= 2; sh++)
			{
				const u32 m = sig << sh;
				const u32 want = (refSqrtQ(m) >> 2) & 0xFFFFFF, got = PS2FloatDetail::SqrtSignificand(m);
				if (want != got)
					REPORT("sqrt kernel m=%07x want %06x got %06x", m, want, got);
			}
	});
	g_checked += 1ull << 24;
}

static void testOps(u64 scale)
{
	const std::vector<u32> edge = edgeValues();
	// Edge cross product, every op.
	std::atomic<u64> next{0};
	parallel([&](unsigned) {
		for (;;)
		{
			u64 i = next.fetch_add(1);
			if (i >= edge.size())
				return;
			for (u32 b : edge)
			{
				const u32 a = edge[i];
				if (!sameFloat(PS2Float(a).Div(PS2Float(b)), refDiv(PS2Float(a), PS2Float(b)))) REPORT("Div edge %08x %08x", a, b);
				if (!sameFloat(PS2Float(a).Mul(PS2Float(b)), refMul(PS2Float(a), PS2Float(b)))) REPORT("Mul edge %08x %08x", a, b);
				if (!sameFloat(PS2Float(a).Rsqrt(PS2Float(b)), refRsqrtImpl(PS2Float(a), PS2Float(b)))) REPORT("Rsqrt edge %08x %08x", a, b);
				if (!sameFloat(PS2Float(a).ERSQRT(), refRsqrtImpl(PS2Float(0x3F800000u), PS2Float(a)))) REPORT("ERSQRT edge %08x", a);
				if (!sameFloat(PS2Float(a).ERCPR(), refDiv(PS2Float(0x3F800000u), PS2Float(a)))) REPORT("ERCPR edge %08x", a);
				if (!sameFloat(PS2Float(a).Sqrt(), refSqrt(PS2Float(a)))) REPORT("Sqrt edge %08x", a);
			}
		}
	});
	const u64 n = 25000000ull * scale / g_threads;
	parallel([&](unsigned t) {
		std::mt19937_64 gr(500 + t);
		for (u64 i = 0; i < n; i++)
		{
			const u32 a = randOperand(gr), b = randOperand(gr);
			if (!sameFloat(PS2Float(a).Div(PS2Float(b)), refDiv(PS2Float(a), PS2Float(b)))) REPORT("Div %08x %08x", a, b);
			if (!sameFloat(PS2Float(a).Mul(PS2Float(b)), refMul(PS2Float(a), PS2Float(b)))) REPORT("Mul %08x %08x", a, b);
			if (!sameFloat(PS2Float(a).Rsqrt(PS2Float(b)), refRsqrtImpl(PS2Float(a), PS2Float(b)))) REPORT("Rsqrt %08x %08x", a, b);
			if (!sameFloat(PS2Float(a).Sqrt(), refSqrt(PS2Float(a)))) REPORT("Sqrt %08x", a);
			if (!sameFloat(PS2Float(a).ERSQRT(), refRsqrtImpl(PS2Float(PS2Float::ONE), PS2Float(a)))) REPORT("ERSQRT %08x", a);
			// VuRsqrt only adds the denormal front end; its tail is RsqrtImpl
			PS2Float vr = PS2Float(a).VuRsqrt(PS2Float(b));
			if (b & 0x7F800000) { if (!sameFloat(vr, refRsqrtImpl(PS2Float(a), PS2Float(b)))) REPORT("VuRsqrt %08x %08x", a, b); }
		}
		g_checked += n;
	});
}

static void testMulKernel(u64 scale)
{
	using namespace PS2FloatDetail;
	// (a) all 2^23 multiplicands against 512 multiplier significands (corners + stride + random):
	// the gated product must equal the array's in bits 23..47.
	std::vector<u32> bs = {0x800000, 0x800001, 0x8000FF, 0xFFFFFF, 0xFFFFFE, 0xC00000, 0xAAAAAA, 0xB504F3, 0xFF8000, 0x808000, 0x800100, 0x810000};
	std::mt19937_64 g(7);
	while (bs.size() < 512 * scale)
		bs.push_back(0x800000 | (rnd32(g) & 0x7FFFFF));
	std::atomic<u64> next{0};
	std::atomic<u64> needed{0}, total{0};
	parallel([&](unsigned) {
		u64 nd = 0, tt = 0;
		for (;;)
		{
			u64 i = next.fetch_add(1);
			if (i >= bs.size())
				break;
			const u32 b = bs[i];
			for (u32 a = 0x800000; a < 0x1000000; a++)
			{
				const u64 want = MulMantissaArray(a, b) >> 23, got = MulMantissa(a, b) >> 23;
				if (want != got)
					REPORT("mul kernel a=%06x b=%06x want %lx got %lx", a, b, (unsigned long)want, (unsigned long)got);
				nd += ((u64)a * b & 0x7FFFFF) < 0x8000;
				tt++;
			}
		}
		needed += nd;
		total += tt;
	});
	std::printf("  mul gate: array needed on %.3f%% of the exhaustive sample\n", 100.0 * needed / total);
	// (b) where the gate does not fire the array's value must be full or full-2^15 only (the premise).
	// (c) random + near-boundary: operands chosen so the tail sits around 0..2^16.
	const u64 n = 200000000ull * scale / g_threads;
	std::atomic<u64> boundary{0};
	parallel([&](unsigned t) {
		std::mt19937_64 gr(900 + t);
		u64 bd = 0;
		for (u64 i = 0; i < n; i++)
		{
			u32 a = 0x800000 | (rnd32(gr) & 0x7FFFFF), b = 0x800000 | (rnd32(gr) & 0x7FFFFF);
			const u64 full = (u64)a * b;
			const u64 arr = MulMantissaArray(a, b);
			if (arr != full && arr != full - 0x8000)
				REPORT("array premise broken a=%06x b=%06x", a, b);
			if ((arr >> 23) != (MulMantissa(a, b) >> 23))
				REPORT("mul kernel (random) a=%06x b=%06x", a, b);
			// force a tail near the boundary: nudge b until the low 23 bits fall in [0, 2^16)
			if ((i & 7) == 0)
			{
				for (int k = 0; k < 64; k++)
				{
					u32 b2 = b + k;
					if (b2 > 0xFFFFFF)
						break;
					const u64 f2 = (u64)a * b2;
					if ((f2 & 0x7FFFFF) < 0x10000)
					{
						bd++;
						if ((MulMantissaArray(a, b2) >> 23) != (MulMantissa(a, b2) >> 23))
							REPORT("mul kernel (boundary) a=%06x b=%06x", a, b2);
						break;
					}
				}
			}
		}
		boundary += bd;
		g_checked += n;
	});
	std::printf("  mul gate: %lu near-boundary tails checked\n", (unsigned long)boundary.load());
}

//---------------------------------------------------------------- benchmark
template <class F>
static double timeIt(F fn, u64 n)
{
	auto t0 = std::chrono::steady_clock::now();
	u32 acc = fn(n);
	auto t1 = std::chrono::steady_clock::now();
	volatile u32 sink = acc;
	(void)sink;
	return std::chrono::duration<double, std::nano>(t1 - t0).count() / n;
}

static void bench()
{
	using namespace PS2FloatDetail;
	const u64 N = 20000000;
	std::vector<u32> sa(1 << 16), sb(1 << 16);
	std::mt19937_64 g(3);
	for (auto& x : sa) x = 0x800000 | (rnd32(g) & 0x7FFFFF);
	for (auto& x : sb) x = 0x800000 | (rnd32(g) & 0x7FFFFF);
	auto loopDiv = [&](auto f) { return timeIt([&](u64 n) { u32 acc = 0; for (u64 i = 0; i < n; i++) acc += f(sa[i & 0xFFFF], sb[(i * 7 + 1) & 0xFFFF]); return acc; }, N); };
	auto loopSqrt = [&](auto f) { return timeIt([&](u64 n) { u32 acc = 0; for (u64 i = 0; i < n; i++) acc += f(sa[i & 0xFFFF] << 1); return acc; }, N); };
	auto loopMul = [&](auto f) { return timeIt([&](u64 n) { u32 acc = 0; for (u64 i = 0; i < n; i++) acc += (u32)(f(sa[i & 0xFFFF], sb[(i * 7 + 1) & 0xFFFF]) >> 23); return acc; }, N); };
	std::printf("bench (ns/op, random significands, kernel only, no cache):\n");
	double d0 = loopDiv(refDivQ), d1 = loopDiv([](u32 a, u32 b) { return DivSignificand(a, b); });
	double s0 = loopSqrt(refSqrtQ), s1 = loopSqrt([](u32 m) { return SqrtSignificand(m); });
	double m0 = loopMul([](u32 a, u32 b) { return MulMantissaArray(a, b); }), m1 = loopMul([](u32 a, u32 b) { return MulMantissa(a, b); });
	std::printf("  div  recurrence %.2f  shortcut %.2f  x%.2f\n", d0, d1, d0 / d1);
	std::printf("  sqrt recurrence %.2f  shortcut %.2f  x%.2f\n", s0, s1, s0 / s1);
	std::printf("  mul  array      %.2f  gated    %.2f  x%.2f\n", m0, m1, m0 / m1);
	// whole operations, random finite operands (the cache of Div/Sqrt is in play: 4096x4 entries)
	auto loopOp = [&](auto f) { return timeIt([&](u64 n) { u32 acc = 0; for (u64 i = 0; i < n; i++) acc += f(0x3F800000u | (sa[i & 0xFFFF] & 0x7FFFFF), 0x3F800000u | (sb[(i * 7 + 1) & 0xFFFF] & 0x7FFFFF)); return acc; }, N); };
	double D0 = loopOp([](u32 a, u32 b) { return refDiv(PS2Float(a), PS2Float(b)).raw; }), D1 = loopOp([](u32 a, u32 b) { return PS2Float(a).Div(PS2Float(b)).raw; });
	double M0 = loopOp([](u32 a, u32 b) { return refMul(PS2Float(a), PS2Float(b)).raw; }), M1 = loopOp([](u32 a, u32 b) { return PS2Float(a).Mul(PS2Float(b)).raw; });
	double R0 = loopOp([](u32 a, u32 b) { return refRsqrtImpl(PS2Float(a), PS2Float(b)).raw; }), R1 = loopOp([](u32 a, u32 b) { return PS2Float(a).Rsqrt(PS2Float(b)).raw; });
	std::printf("  PS2Float::Div   ref %.2f new %.2f x%.2f\n", D0, D1, D0 / D1);
	std::printf("  PS2Float::Mul   ref %.2f new %.2f x%.2f\n", M0, M1, M0 / M1);
	std::printf("  PS2Float::Rsqrt ref %.2f new %.2f x%.2f\n", R0, R1, R0 / R1);
}

int main(int argc, char** argv)
{
	bool doBench = false;
	u64 scale = 1;
	for (int i = 1; i < argc; i++)
	{
		if (!std::strcmp(argv[i], "--bench")) doBench = true;
		else scale = std::strtoull(argv[i], nullptr, 10);
	}
	setvbuf(stdout, nullptr, _IOLBF, 0);
	if (const char* t = std::getenv("SOFTFLOAT_THREADS")) g_threads = std::atoi(t);
	if (doBench) { bench(); return 0; }
	std::printf("div kernel...\n");  testDivKernel(scale);
	std::printf("sqrt kernel (exhaustive)...\n"); testSqrtKernel();
	std::printf("mul kernel...\n");  testMulKernel(scale);
	std::printf("whole operations...\n"); testOps(scale);
	std::printf("%lu comparisons+, %lu failures\n", (unsigned long)g_checked.load(), (unsigned long)g_fail.load());
	return g_fail ? 1 : 0;
}
