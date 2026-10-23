#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - build the sandbox elevation helper (build/sandbox-elevator.elf).
#
# A title's sandbox hides /mnt, where the console mounts USB drives. The title sends this helper
# to the payload loader on port 9021 (elfldr, which kstuff setups run); the helper gives this one
# title (by its exact title ID) the file system outside its sandbox. It is the sandbox-elevation
# example of BlackBearReloaded's ps5-native-app-boilerplate (GPL-3.0-or-later), with this title's
# ID: ps5/elevation/.
#
#   ps5/tools/build-elevator.sh [output]

set -euo pipefail

ps5=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
root=$(dirname "$ps5")
sdk=$(bash "$ps5/tools/setup-sdk.sh")
out=${1:-$root/build/sandbox-elevator.elf}
export PS5_CLANG=${PS5_CLANG:-$(command -v clang || true)}

mkdir -p "$(dirname "$out")"
if [[ ! -f $out || $ps5/elevation/payload/main.cpp -nt $out || $ps5/elevation/protocol.hpp -nt $out ]]; then
    "$sdk/bin/prospero-clang++" -std=c++20 -O2 -g -Wall -Wextra -Werror -Wl,--build-id=none \
        -o "$out" "$ps5/elevation/payload/main.cpp"
fi
# elfldr reads the ELF up to the end of its section table and hands the rest of the connection to
# the helper: one byte after that table would be read as the first message.
python3 "$ps5/elevation/validate-elevation-helper.py" "$out" >&2
echo "$out"
