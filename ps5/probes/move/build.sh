#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - build the Move probe payload (build/move-probe.elf), as build-notify-relay.sh builds its
# helper: the pinned payload SDK from ps5/tools/setup-sdk.sh, prospero-clang, stripped, and checked
# to end at its section table (the payload loader on port 9021 reads up to there).
#
#   ps5/probes/move/build.sh [output]

set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ps5=$(cd -- "$here/../.." && pwd)
root=$(dirname "$ps5")
sdk=$(bash "$ps5/tools/setup-sdk.sh")
out=${1:-$root/build/move-probe.elf}
src=$here/move_probe.c
export PS5_CLANG=${PS5_CLANG:-$(command -v clang || true)}

mkdir -p "$(dirname "$out")"
# libSceMove is not linked (the SDK has no stub for it, and a missing import would stop the
# payload from starting): it is loaded and looked up while the probe runs.
PS5_PAYLOAD_SDK="$sdk" "$sdk/bin/prospero-clang" -std=c11 -Wall -Wextra -Werror -O2 -Wl,--build-id=none \
    -o "$out" "$src" -lSceUserService
PS5_PAYLOAD_SDK="$sdk" "$sdk/bin/prospero-strip" "$out"
python3 "$ps5/elevation/validate-elevation-helper.py" "$out" >&2
echo "$out"
