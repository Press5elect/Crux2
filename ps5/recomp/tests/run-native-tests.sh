#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - the native-module test on the PC: the test program (program/) built for the PS2, turned
# into C++ by PS2Recomp in Crux2 mode, built into a module by build-module.py, loaded by Crux2's
# loader and run against a stand-in for PCSX2's memory (native_test.cpp).
#
#   ps5/recomp/tests/run-native-tests.sh <work folder>
#
# Needs the PS2 toolchain (PS2DEV, with $PS2DEV/ee/bin/mips64r5900el-ps2-elf-gcc and PS2SDK) and
# PS2Recomp's converter built with ps5/recomp/patches/ps2recomp-crux2.patch (PS2RECOMP, the
# ps2_recomp program; recomp-game.sh builds one). Without them the test says so and passes.
set -euo pipefail
work=${1:?work folder}
here=$(cd "$(dirname "$0")" && pwd)
ps5=$(cd "$here/../.." && pwd)
mkdir -p "$work"

recomp=${PS2RECOMP:-}
if [[ -z "${PS2DEV:-}" || ! -x "${PS2DEV}/ee/bin/mips64r5900el-ps2-elf-gcc" || -z "$recomp" || ! -x "$recomp" ]]; then
	echo "  native modules: skipped (set PS2DEV, PS2SDK and PS2RECOMP to run it)"
	exit 0
fi

rm -rf "$work/program" && cp -r "$here/program" "$work/program"
PATH="$PS2DEV/ee/bin:$PATH" make -s -C "$work/program" >/dev/null
elf="$work/program/test.elf"

addr() { "$PS2DEV/ee/bin/mips64r5900el-ps2-elf-nm" "$elf" | awk -v n="$1" '$3 == n { print $1; exit }'; }
fib=$(addr fib) strlen=$(addr strlen) memcpy=$(addr memcpy) strcpy=$(addr strcpy) main=$(addr main)

(cd "$work/program" && PS2RECOMP_CRUX2=1 "$recomp" hello-crux2.toml >/dev/null)
python3 "$ps5/recomp/tools/build-module.py" --elf "$elf" --generated "$work/program/crux2out" --out "$work/test.crux2native"

clang++ -std=c++20 -O1 -g -fsanitize=address,undefined -I "$ps5/recomp/runtime" -I "$ps5/frontend" \
	"$here/native_test.cpp" "$ps5/frontend/ps5_native_loader.cpp" -o "$work/native_test"
"$work/native_test" "$work/test.crux2native" "$elf" "$fib" "$strlen" "$memcpy" "$strcpy" "$main"
