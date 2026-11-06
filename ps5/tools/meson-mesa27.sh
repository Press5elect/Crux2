#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - meson for PS5_Vulkan's tools/build-radv.sh on Mesa main (27.0-devel).
#
# Mesa main replaced the boolean option amd-use-llvm by the list amd-options, and keeps the old
# name only as a deprecated alias that hands its value to the new option: "false" is not one of
# the list's choices, so meson refuses the setup PS5_Vulkan's script asks for. Without LLVM is an
# empty list there. build-radv-crux2.sh runs the script with MESON set to this file; every other
# argument goes to meson as it is.
set -euo pipefail
real=${CRUX2_REAL_MESON:-$(command -v meson)}
arguments=()
for argument in "$@"; do
    [[ $argument == -Damd-use-llvm=false ]] && argument=-Damd-options=
    arguments+=("$argument")
done
exec "$real" "${arguments[@]}"
