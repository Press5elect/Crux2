#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds data/ee_fpu_golden.txt from the console-captured rows ARMSX2 keeps in its tests
# (tests/ctest/core/recompilers/*.cpp|h). Usage: extract_corpora.py ARMSX2_REPO > data/ee_fpu_golden.txt
# One row per line: src op fs ft acc want flags seed   ("-" = not recorded)
import re, sys, pathlib

root = pathlib.Path(sys.argv[1]) / "tests/ctest/core/recompilers"
H = r"(0x[0-9A-Fa-f]+)u?"
rows = []

def hexes(line):
    # 0x... literals, and the bare "0u" the tables use for a zero operand (ordinals have no "u")
    out = []
    for m in re.findall(r"0[xX][0-9A-Fa-f]+|\b\d+u\b", line):
        out.append(m if m[:2] in ("0x", "0X") else "0x%08X" % int(m[:-1]))
    return out

def table(path, name):
    """The text of `constexpr ... name[] = { ... };`"""
    t = (root / path).read_text()
    m = re.search(name + r"\[\]\s*=\s*\{(.*?)\n\};", t, re.S)
    assert m, (path, name)
    return m.group(1)

def entries(body):
    # one {...} per entry, may span lines; strip comments
    body = re.sub(r"//[^\n]*", "", body)
    return re.findall(r"\{([^{}]*)\}", body)

def emit(src, op, fs, ft="-", acc="-", want="-", flags="-", seed="-"):
    rows.append((src, op, fs, ft, acc, want, flags, seed))

# divunit console: fs ft con_sqrt con_div con_rsqrt ...
for e in entries(table("ee_fpu_divunit_console_tests.cpp", "kRows")):
    h = hexes(e)
    if len(h) < 5:
        continue
    fs, ft, sq, dv, rs = h[:5]
    emit("divunit_console", "DIV", fs, ft, "-", dv)
    emit("divunit_console", "SQRT", "-", ft, "-", sq)
    emit("divunit_console", "RSQRT", fs, ft, "-", rs)

# divunit exhaustive witnesses: fs ft con_div ieee_div con_sqrt ieee_sqrt con_rsqrt con_two_step
for e in entries(table("ee_fpu_divunit_exhaustive_tests.cpp", "kRows")):
    h = hexes(e)
    if len(h) < 8:
        continue
    fs, ft, dv, _, sq, _, rs, two = h[:8]
    emit("divunit_witness", "DIV", fs, ft, "-", dv)
    emit("divunit_witness", "SQRT", "-", ft, "-", sq)
    emit("divunit_witness", "RSQRT", fs, ft, "-", rs)
for e in entries(table("ee_fpu_divunit_exhaustive_tests.cpp", "kLawRows")):
    kind = re.match(r"\s*(LAW_\w+)", e).group(1)
    h = hexes(e)
    fs, ft, con = h[0], h[1], h[2]
    if kind == "LAW_DIV_FIRES":
        emit("divunit_law", "DIV", fs, ft, "-", con)
    else:
        emit("divunit_law", "SQRT", "-", ft, "-", con)

# fpuovf: op fs ft acc result fcr31 acc_dest
t = (root / "autocases_fpuovf.h").read_text()
opnames = {"FO_ADD": "ADD", "FO_SUB": "SUB", "FO_MUL": "MUL", "FO_DIV": "DIV", "FO_SQRT": "SQRT", "FO_RSQRT": "RSQRT",
           "FO_ADDA": "ADD", "FO_SUBA": "SUB", "FO_MULA": "MUL", "FO_MADD": "MADD", "FO_MSUB": "MSUB",
           "FO_MADDA": "MADD", "FO_MSUBA": "MSUB"}
for e in entries(re.search(r"kCases\[\]\s*=\s*\{(.*?)\n\};", t, re.S).group(1)):
    m = re.match(r"\s*(FO_\w+)", e)
    h = hexes(e)
    emit("fpuovf", opnames[m.group(1)], h[0], h[1], h[2], h[3], h[4])

# top binade: ordinal, form, fs, ft, acc, want
fm = {"F_ADD": "ADD", "F_SUB": "SUB", "F_MUL": "MUL", "F_ADDA": "ADD", "F_SUBA": "SUB", "F_MULA": "MUL",
      "F_MADD": "MADD", "F_MSUB": "MSUB", "F_MADDA": "MADD", "F_MSUBA": "MSUB",
      "FORM_ADD": "ADD", "FORM_SUB": "SUB", "FORM_MUL": "MUL", "FORM_DIV": "DIV", "FORM_ADDA": "ADD",
      "FORM_SUBA": "SUB", "FORM_MADD": "MADD", "FORM_MSUB": "MSUB", "FORM_MADDA": "MADD", "FORM_MSUBA": "MSUB"}
for e in entries(table("ee_fpu_top_binade_console_tests.cpp", "kConsole")):
    m = re.search(r"(F_\w+)", e)
    h = hexes(e)
    emit("top_binade", fm[m.group(1)], h[0], h[1], h[2], h[3])
for e in entries(table("ee_fpu_guarded_addsub_console_tests.cpp", "kConsole")):
    m = re.search(r"(FORM_\w+)", e)
    h = hexes(e)
    emit("guarded_addsub", fm[m.group(1)], h[0], h[1], h[2], h[3])
for e in entries(table("ee_fpu_underflow_console_tests.cpp", "kConsole")):
    m = re.search(r"(FORM_\w+)", e)
    h = hexes(e)
    emit("underflow", fm[m.group(1)], h[0], h[1], h[2], h[3], h[4])

# zero divisor: fs ft rsqrt console flags (flags in k* names; recompute below from the names)
bits = {"kI": 0x20000, "kD": 0x10000, "kSI": 0x40, "kSD": 0x20}
for e in entries(table("ee_fpu_zero_divisor_console_tests.cpp", "kRows")):
    h = hexes(e)
    isr = "true" in e.split(",")[2]
    flags = 0
    for n in re.findall(r"\bk[A-Z]+\b", e):
        flags |= bits[n]
    emit("zero_divisor", "RSQRT" if isr else "DIV", h[0], h[1], "-", h[2], "0x%08X" % flags)

# rsqrt sign: op fs ft seed console flags
ro = {"kRsqrt": "RSQRT", "kDiv": "DIV", "kSqrt": "SQRT"}
for e in entries(table("ee_fpu_rsqrt_sign_console_tests.cpp", "kRows")):
    m = re.match(r"\s*(k\w+)", e)
    if m.group(1) not in ro:
        continue  # kSqrtDiv is two ops in one probe; the plain rows cover it
    h = hexes(e)
    emit("rsqrt_sign", ro[m.group(1)], h[0], h[1], "-", h[3], h[4], h[2])

# VU0/VU1 EFU rows (ps2autotests vu/lower/efu, real hardware): P after ERCPR/ESQRT/ERSQRT of fs.z
t = (root / "autocases_efu.h").read_text()
for line in t.splitlines():
    m = re.match(r'\s*\{"(ERCPR|ESQRT|ERSQRT|EATAN)"', line)
    if m:
        h = hexes(line)
        assert len(h) == 5, line
        emit("efu", m.group(1), h[2], "-", "-", h[4])

# VU0 macro-mode MADDA/MSUBA (SCPH-90000 capture): acc, fs, ft, sub, ACC.x out
for line in (root / "autocases_vumadd.h").read_text().splitlines():
    m = re.match(r"\s*\{(0x\w+), (0x\w+), (0x\w+), (true|false), (0x\w+)", line)
    if m:
        a, fs, ft, sub, out = m.groups()
        emit("vumadd", "VMSUBA" if sub == "true" else "VMADDA", fs, ft, a, out.rstrip("u"))

print("# Console-captured EE FPU rows, extracted from ARMSX2 (see README.md).")
print("# src op fs ft acc want flags seed   ('-' = not recorded; ops: ADD SUB MUL DIV SQRT RSQRT MADD MSUB, VU: VMADDA VMSUBA, EFU: ERCPR ESQRT ERSQRT EATAN)")
for r in rows:
    print(" ".join(r))
