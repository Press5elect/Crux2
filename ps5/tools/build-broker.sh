#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - build the write broker (build/write-broker.elf), taken from OGBox (ps5/tools/build-broker.sh).
#
# A payload the title sends to the payload loader on port 9021 (elfldr), as it sends the sandbox
# elevation helper (tools/build-elevator.sh); it does file writes for the title in a process of its
# own, which the console does not hold back after 1.3 GiB as it holds back the title's (ps5/broker;
# the title's side: frontend/ps5_broker.cpp). Built as build-elevator.sh builds the helper: the
# pinned payload SDK's prospero-clang++, and the same check of the ELF's end.
#
#   ps5/tools/build-broker.sh [output]

set -euo pipefail

ps5=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
root=$(dirname "$ps5")
sdk=$(bash "$ps5/tools/setup-sdk.sh")
out=${1:-$root/build/write-broker.elf}
export PS5_CLANG=${PS5_CLANG:-$(command -v clang || true)}

sources=("$ps5/broker/payload/main.cpp" "$ps5/broker/server.cpp")
mkdir -p "$(dirname "$out")"
stale=0
[[ -f $out ]] || stale=1
for f in "${sources[@]}" "$ps5"/broker/*.hpp; do
    [[ $f -nt $out ]] && stale=1
done
if ((stale)); then
    "$sdk/bin/prospero-clang++" -std=c++20 -O2 -g -Wall -Wextra -Werror -Wl,--build-id=none \
        -o "$out" "${sources[@]}"
fi
# elfldr reads the ELF up to the end of its section table and hands the rest of the connection to
# the payload: one byte after that table would be read as the first request.
python3 "$ps5/elevation/validate-elevation-helper.py" "$out" >&2
echo "$out"
