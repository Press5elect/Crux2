# Console-captured rows for the accurate-float model

`ee_fpu_golden.txt` holds values a real PS2 (SCPH-90000, and the ps2autotests captures) returned. They are
copied out of ARMSX2's test tables by `../extract_corpora.py`; nothing is generated and nothing is edited by
hand.

- Source: ARMSX2 (tests/ctest/core/recompilers), at commit 70bd63f6e; the tables are in
  `ee_fpu_divunit_console_tests.cpp`, `ee_fpu_divunit_exhaustive_tests.cpp`, `autocases_fpuovf.h`,
  `ee_fpu_top_binade_console_tests.cpp`, `ee_fpu_guarded_addsub_console_tests.cpp`,
  `ee_fpu_underflow_console_tests.cpp`, `ee_fpu_zero_divisor_console_tests.cpp`,
  `ee_fpu_rsqrt_sign_console_tests.cpp`, `autocases_efu.h` and `autocases_vumadd.h`.
- Licence: GPL-3.0-or-later, as ARMSX2 and Crux2. The rows are measurements; the file keeps ARMSX2's table
  values only.
- Not included: the 150-million-row exhaustive divide capture (ARMSX2 keeps only its generator and
  witnesses; the witnesses are here as `divunit_witness`/`divunit_law`), the multiplier sweep, and the VU
  branch/cache/latency autocases, which do not exercise PS2Float.

Line format: `src op fs ft acc want flags seed`, hex, `-` where a column is not recorded. Ops: EE FPU `ADD SUB
MUL DIV SQRT RSQRT MADD MSUB` (A-forms give the same value as their plain forms), VU `VMADDA VMSUBA`, EFU
`ERCPR ESQRT ERSQRT EATAN` (operand `fs` is the lane the capture read, `want` is P). `flags` is the console's
FCR31 after the op (`rsqrt_sign`, `zero_divisor`: the I/D cause bits and their sticky SI/SD; `fpuovf`: O/U) and
`seed` the FCR31 written with CTC1 before it.

`../softfloat_corpus_test.cpp` checks Crux2's `PS2Float` (and, for the flags, the logic of FPU.cpp's soft
paths) against every row and prints each disagreement.
