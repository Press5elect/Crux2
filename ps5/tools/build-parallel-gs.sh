#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - build the paraLLEl-GS renderer object (ps5/pgs) from Crux2 Recomp's sources.
#
#   ps5/tools/build-parallel-gs.sh <sdk> <build folder>     prints the object's path
#
#   PS5_CRUX2RECOMP_DIR   where the sources come from: a PS5_Crux2Recomp checkout (default
#                         ../PS5_Crux2Recomp when there is one, else this repository's crux2recomp/,
#                         the same scripts and patches). Its tools/fetch-parallel-gs.sh and
#                         tools/fetch-ps2recomp.sh fetch and patch the sources (they do nothing when
#                         they are already there).
set -euo pipefail
ps5=$(cd "$(dirname "$0")/.." && pwd)
root=$(dirname "$ps5")
sdk=${1:?usage: build-parallel-gs.sh <sdk> <build folder>}
build=${2:?usage: build-parallel-gs.sh <sdk> <build folder>}
if [[ -n ${PS5_CRUX2RECOMP_DIR:-} ]]; then
	recomp=$(cd -- "$PS5_CRUX2RECOMP_DIR" && pwd)
elif [[ -d $root/../PS5_Crux2Recomp/ps5/tools ]]; then
	recomp=$(cd -- "$root/../PS5_Crux2Recomp" && pwd)
else
	recomp="$root/crux2recomp"
fi
bash "$recomp/ps5/tools/fetch-parallel-gs.sh" >&2
bash "$recomp/ps5/tools/fetch-ps2recomp.sh" >&2
ninja=$(command -v ninja || echo "$HOME/.local/bin/ninja")
if [[ ! -f $build/build.ninja ]]; then
	cmake -S "$ps5/pgs" -B "$build" -G Ninja -DCMAKE_MAKE_PROGRAM="$ninja" \
		-DCMAKE_TOOLCHAIN_FILE="$sdk/toolchain/prospero.cmake" -DCMAKE_BUILD_TYPE=Release \
		-DPGS="$recomp/vendor/parallel-gs" -DPS2R="$recomp/vendor/PS2Recomp" -DPS5_PAYLOAD_SDK="$sdk" >&2
fi
"$ninja" -C "$build" parallelgs >&2
# The renderer's interface, as the object was built with it, for the title's side (ps5_pgs.cpp):
# ps5/pgs/gs_gpu.h is a copy of def-jam-vendetta-recomp's runtime/gs/gs_gpu.h with Crux2 Recomp's
# changes (GPL-3.0), kept equal to it here.
header="$recomp/vendor/PS2Recomp/ps2xRuntime/include/runtime/gs/gs_gpu.h"
cmp -s "$header" "$ps5/pgs/gs_gpu.h" || cp "$header" "$ps5/pgs/gs_gpu.h"
echo "$build/parallel-gs.o"
