// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
// Crux2 - PS2Float against rows captured on real PS2 hardware (data/ee_fpu_golden.txt, ARMSX2's
// console corpora; see data/README.md): ADD SUB MUL DIV SQRT RSQRT MADD MSUB on the EE FPU, the VU
// EFU's ERCPR/ESQRT/ERSQRT/EATAN, and the FCR31 cause bits after DIV/SQRT/RSQRT.
//
//   softfloat_corpus_test data/ee_fpu_golden.txt
//
// Every disagreement is printed. A row is only tolerated if it is in kKnown below with the reason; a
// listed row that starts to agree fails too, so the list cannot go stale.
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <bit>
#include "common/Pcsx2Defs.h"
#include "PS2Float.h"

void Log::Writev(LOGLEVEL, ConsoleColors, const char* fmt, va_list ap) { std::vfprintf(stderr, fmt, ap); std::fputc('\n', stderr); }

// FPU.cpp's FCR31 bits
static constexpr u32 kI = 0x20000, kD = 0x10000, kO = 0x8000, kU = 0x4000, kSI = 0x40, kSD = 0x20;
static constexpr u32 kCause = kI | kD | kSI | kSD;

struct Row
{
	std::string src, op;
	u32 fs = 0, ft = 0, acc = 0, want = 0, seed = 0, flags = 0;
	bool hasFs = false, hasFt = false, hasAcc = false, hasFlags = false;
	int line = 0;
};

static bool parseHex(const std::string& s, u32& v)
{
	if (s == "-")
		return false;
	v = (u32)std::strtoul(s.c_str(), nullptr, 16);
	return true;
}

// FPU.cpp's vuDouble() with the overflow clamp on (CHECK_VU_OVERFLOW) and denormals flushed.
static float vuDouble(u32 f)
{
	switch (f & 0x7f800000)
	{
		case 0x0: f &= 0x80000000; return std::bit_cast<float>(f);
		case 0x7f800000: f = (f & 0x80000000) | 0x7f7fffff; return std::bit_cast<float>(f);
	}
	return std::bit_cast<float>(f);
}

// VUops.cpp _vuCalculateEATAN + _vuEATAN (the range reduction is in the caller).
static u32 eatan(u32 fsBits)
{
	const float eatanconst[9] = {0.999999344348907f, -0.333298563957214f, 0.199465364217758f, -0.139085337519646f,
		0.096420042216778f, -0.055909886956215f, 0.021861229091883f, -0.004054057877511f, 0.785398185253143f};
	const float fs = vuDouble(fsBits);
	const float in = (fs - 1.0f) / (fs + 1.0f);
	float result = (eatanconst[0] * in) + (eatanconst[1] * pow(in, 3)) + (eatanconst[2] * pow(in, 5)) +
	               (eatanconst[3] * pow(in, 7)) + (eatanconst[4] * pow(in, 9)) + (eatanconst[5] * pow(in, 11)) +
	               (eatanconst[6] * pow(in, 13)) + (eatanconst[7] * pow(in, 15));
	result += eatanconst[8];
	return std::bit_cast<u32>(vuDouble(std::bit_cast<u32>(result)));
}

struct Out { u32 value; u32 fcr; };

// FPU.cpp's soft paths: value and the FCR31 after the op from `seed`. `gate` selects LRPS2 c199233c6's
// RSQRT.S rule (I only for a negative Ft with a nonzero exponent) instead of the sign bit alone.
static Out model(const Row& r, bool gate)
{
	u32 fcr = r.seed;
	auto divZeroInvalid = [&](PS2Float x) {
		if (x.HasDivideByZero()) { fcr |= kD | kSD; return; }
		if (x.HasInvalid()) fcr |= kI | kSI;
	};
	auto mulFlags = [&](PS2Float x) {
		if (x.HasOverflow()) fcr |= kO; else fcr &= ~kO;
		if (x.HasUnderflow()) fcr |= kU; else fcr &= ~kU;
	};
	const PS2Float fs(r.fs), ft(r.ft);
	if (r.op == "ADD") { PS2Float x = fs.raw, y = ft.raw; PS2Float v = x.Add(y); mulFlags(v); return {v.raw, fcr}; }
	if (r.op == "SUB") { PS2Float x = fs.raw, y = ft.raw; PS2Float v = x.Sub(y); mulFlags(v); return {v.raw, fcr}; }
	if (r.op == "MUL") { PS2Float x = fs.raw, y = ft.raw; PS2Float v = x.Mul(y); mulFlags(v); return {v.raw, fcr}; }
	if (r.op == "MADD" || r.op == "MSUB")
	{
		PS2Float mulres = PS2Float(r.fs).Mul(PS2Float(r.ft));
		PS2Float acc(r.acc);
		PS2Float v = r.op == "MADD" ? acc.AddMulResultAcc(mulres) : acc.SubMulResultAcc(mulres);
		return {v.raw, fcr};
	}
	if (r.op == "VMADDA" || r.op == "VMSUBA") // VUops.cpp _vuAccurateOpMADDA / the MSUBA twin, ACC overflow flag clear
	{
		PS2Float acc(r.acc);
		PS2Float v = r.op == "VMADDA" ? acc.MulAddAcc(PS2Float(r.fs), PS2Float(r.ft)) : acc.MulSubAcc(PS2Float(r.fs), PS2Float(r.ft));
		return {v.raw, fcr};
	}
	if (r.op == "DIV")
	{
		fcr &= ~(kI | kD);
		PS2Float v = PS2Float(r.fs).Div(PS2Float(r.ft));
		divZeroInvalid(v);
		return {v.raw, fcr};
	}
	if (r.op == "SQRT")
	{
		fcr &= ~(kI | kD);
		PS2Float v = PS2Float(r.ft).Sqrt();
		divZeroInvalid(v);
		return {v.raw, fcr};
	}
	if (r.op == "RSQRT")
	{
		fcr &= ~(kI | kD);
		PS2Float v = PS2Float(r.fs).Rsqrt(PS2Float(r.ft));
		if ((r.ft & 0x80000000) && (!gate || (r.ft & 0x7F800000)))
			fcr |= kI | kSI;
		divZeroInvalid(v);
		return {v.raw, fcr};
	}
	if (r.op == "ERCPR") return {PS2Float(r.fs).ERCPR().raw, 0};
	if (r.op == "ESQRT") return {PS2Float(r.fs).ESQRT().raw, 0};
	if (r.op == "ERSQRT") return {PS2Float(r.fs).ERSQRT().raw, 0};
	if (r.op == "EATAN") return {eatan(r.fs), 0};
	std::fprintf(stderr, "unknown op %s\n", r.op.c_str());
	std::exit(2);
}

// Rows known to disagree, with the reason. key = "src op fs ft acc".
static const char* const kKnown[][2] = {
	{"efu EATAN 00000000 00000000 00000000", "EATAN(~0): reduced argument -1, the series cancels against pi/4 down to a residue; console 0x33E60000, VUops.cpp float series 0x33800000"},
	{"efu EATAN 80000000 00000000 00000000", "same cancellation"},
	{"efu EATAN 00000001 00000000 00000000", "same cancellation"},
	{"efu EATAN 00001337 00000000 00000000", "same cancellation"},
	{"efu EATAN bf800000 00000000 00000000", "EATAN(-1.0): reduced argument (x-1)/(x+1) = -2/0; console -pi/2, the series model saturates"},
};

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::fprintf(stderr, "usage: %s data/ee_fpu_golden.txt\n", argv[0]);
		return 2;
	}
	std::ifstream in(argv[1]);
	if (!in)
	{
		std::fprintf(stderr, "cannot read %s\n", argv[1]);
		return 2;
	}
	std::vector<Row> rows;
	std::string line;
	int ln = 0;
	while (std::getline(in, line))
	{
		ln++;
		if (line.empty() || line[0] == '#')
			continue;
		std::istringstream ss(line);
		std::string fs, ft, acc, want, flags, seed;
		Row r;
		ss >> r.src >> r.op >> fs >> ft >> acc >> want >> flags >> seed;
		r.hasFs = parseHex(fs, r.fs);
		r.hasFt = parseHex(ft, r.ft);
		r.hasAcc = parseHex(acc, r.acc);
		parseHex(want, r.want);
		r.hasFlags = parseHex(flags, r.flags);
		parseHex(seed, r.seed);
		r.line = ln;
		rows.push_back(r);
	}

	std::map<std::string, std::pair<int, int>> perSrc; // rows, value disagreements
	int valueBad = 0, flagBad = 0, flagBadGated = 0, flagRows = 0;
	std::vector<std::string> known;
	for (auto& k : kKnown) known.push_back(k[0]);
	int unexpected = 0, eatanApprox = 0;
	for (const Row& r : rows)
	{
		const Out o = model(r, false);
		auto& [n, bad] = perSrc[r.src + " " + r.op];
		n++;
		char key[160];
		std::snprintf(key, sizeof key, "%s %s %08x %08x %08x", r.src.c_str(), r.op.c_str(), r.fs, r.ft, r.acc);
		bool isKnown = false;
		for (auto& k : known) if (k == key) isKnown = true;
		// EATAN is VUops.cpp's float-series model, not PS2Float: it is within a few ULP of the console
		// (the series is evaluated in double pow(), the console's EFU is its own circuit) and exactly
		// right on some rows. Tolerate <= 4 ULP; the -1.0 row (reduced argument -2/0) is a recorded gap.
		if (r.op == "EATAN" && o.value != r.want)
		{
			const long d = std::labs((long)(o.value & 0x7FFFFFFF) - (long)(r.want & 0x7FFFFFFF));
			if ((o.value >> 31) == (r.want >> 31) && d <= 4)
			{
				eatanApprox++;
				continue;
			}
		}
		if (o.value != r.want)
		{
			bad++;
			valueBad++;
			std::printf("VALUE %s (line %d): console %08x, PS2Float %08x%s\n", key, r.line, r.want, o.value, isKnown ? " [known]" : "");
			if (!isKnown) unexpected++;
		}
		else if (isKnown)
		{
			std::printf("STALE known row now agrees: %s\n", key);
			unexpected++;
		}
		if (r.hasFlags && (r.src == "rsqrt_sign" || r.src == "zero_divisor"))
		{
			flagRows++;
			if ((o.fcr & kCause) != (r.flags & kCause))
			{
				flagBad++;
				unexpected++;
				std::printf("FLAGS %s (line %d): console %08x, model %08x (seed %08x)\n", key, r.line, r.flags & kCause, o.fcr & kCause, r.seed);
			}
			const Out g = model(r, true);
			if ((g.fcr & kCause) != (r.flags & kCause))
				flagBadGated++;
		}
		else if (r.hasFlags && r.src == "fpuovf" && (r.op == "ADD" || r.op == "SUB" || r.op == "MUL"))
		{
			// FCR31 after one op from zero: O/U (and their sticky bits) only
			flagRows++;
			const u32 mask = kO | kU;
			if ((o.fcr & mask) != (r.flags & mask))
			{
				flagBad++;
				unexpected++;
				std::printf("FLAGS %s (line %d): console O/U %08x, model %08x\n", key, r.line, r.flags & mask, o.fcr & mask);
			}
		}
	}
	for (auto& [k, v] : perSrc)
		std::printf("  %-26s %4d rows, %d value disagreements\n", k.c_str(), v.first, v.second);
	std::printf("EATAN rows within 4 ULP but not exact: %d\n", eatanApprox);
	std::printf("%zu rows; %d value disagreements; %d cause-flag checks, %d disagreements"
	            " (LRPS2 c199233c6's gated RSQRT.S rule would disagree on %d)\n",
		rows.size(), valueBad, flagRows, flagBad, flagBadGated);
	if (unexpected)
	{
		std::printf("FAILED: %d unexpected\n", unexpected);
		return 1;
	}
	std::printf("ok\n");
	return 0;
}
