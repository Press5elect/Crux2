#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
"""Crux2 - makes runtime/crux2_ee_interp.h from PCSX2's interpreter (MMI.cpp, FPU.cpp and the SA
instructions of R5900OpcodeImpl.cpp).

PS2Recomp's translations of the EE's multimedia (MMI) and FPU instructions disagree with the
real processor in dozens of places (word orders, HI/LO, saturation, the FPU's clamping). In
Crux2 mode the converter emits CRUX2_EE(0x<instruction>) for those instead, and this header runs
PCSX2's own interpreter code for it, on the native code's registers: PCSX2's interpreter is what
Crux2's check mode compares against, so the two agree by construction.

The functions are PCSX2's (GPL-3.0+, the PCSX2 Dev Team), copied unchanged as members of one
struct whose `cpuRegs` and `fpuRegs` are views of R5900Context. Every member is forced inline and
the instruction word is a constant at each use, so a CRUX2_EE compiles to the instruction's own
code, as if written out by hand.

    make-ee-interp.py <pcsx2 source folder (vendor/pcsx2/pcsx2)> <output header>
"""
import re
import sys

SKIP = {  # memory, branches and control registers: the converter keeps these
    "BC1F", "BC1FL", "BC1T", "BC1TL", "LWC1", "SWC1", "CFC1", "CTC1", "MFC1", "MTC1",
}


SA_INSTRUCTIONS = ("MFSA", "MTSA", "MTSAB", "MTSAH")


def strip_comments(s):
    # one pass, in order of appearance: a "//***" line is a line comment, not a block's start
    def keep(m):
        t = m.group(0)
        return t if t.startswith('"') or t.startswith("'") else "\n" * t.count("\n")
    return re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', keep, s, flags=re.S)


def items(src):
    """Top-level definitions of a source file, namespaces flattened: (kind, name, text)."""
    s = strip_comments(src)
    out = []
    i, n = 0, len(s)
    while i < n:
        if s[i].isspace() or s[i] == "}" or s[i] == ";":
            i += 1
            continue
        if s[i] == "#":  # a preprocessor line, with continuations
            j = i
            while True:
                e = s.find("\n", j)
                e = n if e < 0 else e
                if s[e - 1] == "\\":
                    j = e + 1
                    continue
                break
            line = s[i:e]
            if not line.startswith("#include") and not line.startswith("#pragma"):
                out.append(("pp", None, line))
            i = e + 1
            continue
        m = re.match(r"namespace\s+\w+\s*\{", s[i:])
        if m:
            i += m.end()
            continue
        # a declaration: up to ';' or a '{ ... }' body
        j = i
        while j < n and s[j] not in "{;":
            j += 1
        head = s[i:j].strip()
        if j < n and s[j] == ";":
            out.append(("decl", None, head + ";"))
            i = j + 1
            continue
        depth, k = 0, j
        while k < n:
            if s[k] == "{":
                depth += 1
            elif s[k] == "}":
                depth -= 1
                if depth == 0:
                    break
            k += 1
        body = s[j:k + 1]
        fm = re.search(r"(\w+)\s*\([^()]*\)\s*$", head)
        out.append(("fn", fm.group(1) if fm else None, head, body))
        i = k + 1
    return out


def member(head, body):
    head = re.sub(r"\b(static|__fi|__ri|inline)\s+", "", head)
    return f"\tCRUX2_EE_FN {head}\n\t{body}\n"


def main():
    pcsx2, out = sys.argv[1], sys.argv[2]
    pp, fns, names = [], [], []
    for f in ("MMI.cpp", "FPU.cpp"):
        for it in items(open(f"{pcsx2}/{f}", encoding="utf-8").read()):
            if it[0] == "pp":
                pp.append(it[2])
            elif it[0] == "fn":
                _, name, head, body = it
                if name in SKIP:
                    continue
                if name is None:
                    sys.exit(f"cannot read a definition in {f}: {head[:60]}")
                fns.append(member(head, body))
                names.append(name)
            else:
                sys.exit(f"unexpected declaration in {f}: {it[2][:60]}")
    # The SA register's instructions, from R5900OpcodeImpl.cpp (only these: the rest of that file
    # is memory, branches and the system, which the converter keeps)
    impl = strip_comments(open(f"{pcsx2}/R5900OpcodeImpl.cpp", encoding="utf-8").read())
    for name in SA_INSTRUCTIONS:
        m = re.search(r"\bvoid\s+" + name + r"\s*\(\s*\)\s*\{", impl)
        if not m:
            sys.exit(f"R5900OpcodeImpl.cpp has no {name}")
        depth, k = 0, m.end() - 1
        while True:
            depth += {"{": 1, "}": -1}.get(impl[k], 0)
            if depth == 0:
                break
            k += 1
        fns.append(member(f"void {name}()", impl[m.end() - 1:k + 1]))
        names.append(name)
    with open(out, "w", encoding="utf-8") as o:
        o.write(HEADER)
        o.write("\n".join(pp) + "\n\n")
        o.write("namespace crux2::ee\n{\nstruct Interp\n{\n\tCpuView cpuRegs;\n\tFpuView fpuRegs;\n\n")
        o.write("\n".join(fns))
        o.write(dispatch() + "};\n} // namespace crux2::ee\n\n")
        # PCSX2's macros stay out of the code that includes this header
        defined = sorted({m.group(1) for line in pp for m in [re.match(r"#\s*define\s+(\w+)", line)] if m})
        o.write("".join(f"#undef {d}\n" for d in defined))
        o.write(FOOTER)
    print(f"{out}: {len(names)} functions of PCSX2's interpreter")


HEADER = r"""// Crux2 - PCSX2's interpreter for the EE's MMI and FPU instructions, on the native code's state.
// GENERATED by ps5/recomp/tools/make-ee-interp.py from PCSX2's pcsx2/MMI.cpp, pcsx2/FPU.cpp and the
// SA instructions of pcsx2/R5900OpcodeImpl.cpp:
// do not edit; run the tool again after updating PCSX2.
//
// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team (the instruction code)
// SPDX-FileCopyrightText: 2026 Press5elect (the views, the dispatch and the tool)
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "ps2_runtime.h"

namespace crux2::ee
{
	using u8 = uint8_t; using s8 = int8_t; using u16 = uint16_t; using s16 = int16_t;
	using u32 = uint32_t; using s32 = int32_t; using u64 = uint64_t; using s64 = int64_t;
	struct u128 { u64 lo, hi; };
	struct s128 { s64 lo, hi; };

	union __attribute__((may_alias)) GPR_reg
	{
		u128 UQ; s128 SQ; u64 UD[2]; s64 SD[2]; u32 UL[4]; s32 SL[4]; u16 US[8]; s16 SS[8]; u8 UC[16]; s8 SC[16];
	};
	union __attribute__((may_alias)) FPRreg { float f; u32 UL; s32 SL; };

	// cpuRegs: the GPRs in place, HI and LO in place (R5900Context keeps hi,hi1 and lo,lo1 side by
	// side for this), SA in place (R5900Context's sa is PCSX2's own value), and the instruction word.
	struct CpuView
	{
		struct { GPR_reg* r; } GPR;
		GPR_reg& HI;
		GPR_reg& LO;
		u32& sa;
		u32 code;
		u32 pc;
	};
	// fpuRegs: the FPRs and the accumulator in place; every control register index is FCR31
	// (the only one these instructions touch).
	struct FpuView
	{
		FPRreg* fpr;
		struct Fprc { u32& c31; u32& operator[](int) const { return c31; } } fprc;
		FPRreg& ACC;
		u32 ACCflag;
	};

	namespace Common
	{
		inline u32 CountLeadingSignBits(s32 n) { if (n < 0) n = ~n; return n ? __builtin_clz((u32)n) : 32; }
	}
} // namespace crux2::ee

#define CRUX2_EE_FN __attribute__((always_inline)) inline
#define pxAssume(x) ((void)0)
#define pxAssert(x) ((void)0)
#define pxFail(x) ((void)0)
#define _Rd_ ((cpuRegs.code >> 11) & 0x1F)
#define _Rt_ ((cpuRegs.code >> 16) & 0x1F)
#define _Rs_ ((cpuRegs.code >> 21) & 0x1F)
#define _Sa_ ((cpuRegs.code >> 6) & 0x1F)
#define _Imm_ ((s16)cpuRegs.code)
#define _ImmU_ (cpuRegs.code & 0xffff)
"""

MMI_DIRECT = {0x00: "MADD", 0x01: "MADDU", 0x04: "PLZCW", 0x10: "MFHI1", 0x11: "MTHI1", 0x12: "MFLO1", 0x13: "MTLO1",
              0x18: "MULT1", 0x19: "MULTU1", 0x1A: "DIV1", 0x1B: "DIVU1", 0x20: "MADD1", 0x21: "MADDU1", 0x30: "PMFHL",
              0x31: "PMTHL", 0x34: "PSLLH", 0x36: "PSRLH", 0x37: "PSRAH", 0x3C: "PSLLW", 0x3E: "PSRLW", 0x3F: "PSRAW"}
MMI_SUB = {
    0x08: {0: "PADDW", 1: "PSUBW", 2: "PCGTW", 3: "PMAXW", 4: "PADDH", 5: "PSUBH", 6: "PCGTH", 7: "PMAXH", 8: "PADDB",
           9: "PSUBB", 10: "PCGTB", 16: "PADDSW", 17: "PSUBSW", 18: "PEXTLW", 19: "PPACW", 20: "PADDSH", 21: "PSUBSH",
           22: "PEXTLH", 23: "PPACH", 24: "PADDSB", 25: "PSUBSB", 26: "PEXTLB", 27: "PPACB", 30: "PEXT5", 31: "PPAC5"},
    0x28: {1: "PABSW", 2: "PCEQW", 3: "PMINW", 4: "PADSBH", 5: "PABSH", 6: "PCEQH", 7: "PMINH", 10: "PCEQB", 16: "PADDUW",
           17: "PSUBUW", 18: "PEXTUW", 20: "PADDUH", 21: "PSUBUH", 22: "PEXTUH", 24: "PADDUB", 25: "PSUBUB", 26: "PEXTUB",
           27: "QFSRV"},
    0x09: {0: "PMADDW", 2: "PSLLVW", 3: "PSRLVW", 4: "PMSUBW", 8: "PMFHI", 9: "PMFLO", 10: "PINTH", 12: "PMULTW",
           13: "PDIVW", 14: "PCPYLD", 16: "PMADDH", 17: "PHMADH", 18: "PAND", 19: "PXOR", 20: "PMSUBH", 21: "PHMSBH",
           26: "PEXEH", 27: "PREVH", 28: "PMULTH", 29: "PDIVBW", 30: "PEXEW", 31: "PROT3W"},
    0x29: {0: "PMADDUW", 3: "PSRAVW", 8: "PMTHI", 9: "PMTLO", 10: "PINTEH", 12: "PMULTUW", 13: "PDIVUW", 14: "PCPYUD",
           18: "POR", 19: "PNOR", 26: "PEXCH", 27: "PCPYH", 30: "PEXCW"},
}
FPU_S = {0x00: "ADD_S", 0x01: "SUB_S", 0x02: "MUL_S", 0x03: "DIV_S", 0x04: "SQRT_S", 0x05: "ABS_S", 0x06: "MOV_S",
         0x07: "NEG_S", 0x16: "RSQRT_S", 0x18: "ADDA_S", 0x19: "SUBA_S", 0x1A: "MULA_S", 0x1C: "MADD_S", 0x1D: "MSUB_S",
         0x1E: "MADDA_S", 0x1F: "MSUBA_S", 0x24: "CVT_W", 0x28: "MAX_S", 0x29: "MIN_S", 0x30: "C_F", 0x32: "C_EQ",
         0x34: "C_LT", 0x36: "C_LE"}


def dispatch():
    """Exec<word>(): picks the instruction's function at compile time (if constexpr), so a use
    compiles only that function, not every instruction's."""
    rows = []
    for fn, name in MMI_DIRECT.items():
        rows.append((f"op == 0x1C && fn == 0x{fn:02X}", name))
    for fn, subs in MMI_SUB.items():
        for sub, name in subs.items():
            rows.append((f"op == 0x1C && fn == 0x{fn:02X} && sub == {sub}", name))
    rows += [("op == 0x00 && fn == 0x28", "MFSA"), ("op == 0x00 && fn == 0x29", "MTSA"),
             ("op == 0x01 && rt == 0x18", "MTSAB"), ("op == 0x01 && rt == 0x19", "MTSAH")]
    for fn, name in FPU_S.items():
        rows.append((f"op == 0x11 && fmt == 0x10 && fn == 0x{fn:02X}", name))
    rows.append(("op == 0x11 && fmt == 0x14 && fn == 0x20", "CVT_S"))
    out = ["\n\t// The instruction word's function, chosen at compile time.",
           "\ttemplate <u32 c>", "\tCRUX2_EE_FN void Exec()", "\t{",
           "\t\tconstexpr u32 op = c >> 26, fn = c & 0x3F, sub = (c >> 6) & 0x1F, fmt = (c >> 21) & 0x1F, rt = (c >> 16) & 0x1F;",
           "\t\t(void)op; (void)fn; (void)sub; (void)fmt; (void)rt;"]
    for i, (cond, name) in enumerate(rows):
        out.append(f"\t\t{'if' if i == 0 else 'else if'} constexpr ({cond})\n\t\t\t{name}();")
    out.append("\t\t// Anything else is an instruction the processor does not have: PCSX2's interpreter does")
    out.append("\t\t// nothing for these either.")
    out.append("\t}\n")
    return "\n".join(out) + "\n"


FOOTER = r"""#undef CRUX2_EE_FN
#undef _Rd_
#undef _Rt_
#undef _Rs_
#undef _Sa_
#undef _Imm_
#undef _ImmU_

namespace crux2::ee
{
	static_assert(offsetof(R5900Context, hi1) == offsetof(R5900Context, hi) + 8, "HI must be one 128-bit register");
	static_assert(offsetof(R5900Context, lo1) == offsetof(R5900Context, lo) + 8, "LO must be one 128-bit register");
	static_assert(offsetof(R5900Context, hi) % 16 == 0 && offsetof(R5900Context, lo) % 16 == 0, "HI and LO aligned");

	/// Runs one MMI, FPU or SA instruction exactly as PCSX2's interpreter does.
	template <u32 code>
	__attribute__((always_inline)) inline void Run(R5900Context* ctx)
	{
		Interp in{
			{{reinterpret_cast<GPR_reg*>(ctx->r)}, *reinterpret_cast<GPR_reg*>(&ctx->hi), *reinterpret_cast<GPR_reg*>(&ctx->lo),
				ctx->sa, code, 0},
			{reinterpret_cast<FPRreg*>(ctx->f), {ctx->fcr31}, *reinterpret_cast<FPRreg*>(&ctx->f_acc), 0},
		};
		in.template Exec<code>();
		// The interpreter writes r0 like any register (it is cleared again before PCSX2 reads it);
		// the native code reads r0 as zero, so keep it zero.
		ctx->r[0] = _mm_setzero_si128();
	}
} // namespace crux2::ee

#define CRUX2_EE(code) crux2::ee::Run<(code)>(ctx)
"""

if __name__ == "__main__":
    main()
