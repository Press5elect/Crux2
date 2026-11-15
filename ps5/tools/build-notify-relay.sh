#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - build the notification relay (build/notify-relay.elf).
#
# The title sends a copy of it, with a notification's JSON written in, to the payload loader on
# port 9021, which runs it to post RetroAchievements' unlocks as the console's own trophy
# notifications when the title itself may not use libSceNotification (frontend/ps5_notify.cpp).
# From Porpoise 1679c95 (tools/build-porpoise.sh, GPL-3.0-or-later): ps5/elevation/notify-relay/.
# It goes into the title folder as notify-relay.elf (link-title.sh); without it the notifications
# are still tried through libSceNotification, then as plain text.
#
#   ps5/tools/build-notify-relay.sh [output]

set -euo pipefail

ps5=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
root=$(dirname "$ps5")
sdk=$(bash "$ps5/tools/setup-sdk.sh")
out=${1:-$root/build/notify-relay.elf}
src=$ps5/elevation/notify-relay/notify_relay.c
export PS5_CLANG=${PS5_CLANG:-$(command -v clang || true)}

mkdir -p "$(dirname "$out")"
if [[ ! -f $out || $src -nt $out ]]; then
    PS5_PAYLOAD_SDK="$sdk" "$sdk/bin/prospero-clang" -Wall -Wextra -Werror -O2 -Wl,--build-id=none -o "$out" "$src" -lSceNotification
    PS5_PAYLOAD_SDK="$sdk" "$sdk/bin/prospero-strip" "$out"
fi
# (The loader reads an ELF up to the end of its section table: nothing may follow it.)
python3 "$ps5/elevation/validate-elevation-helper.py" "$out" >&2
# The title looks for the marker followed by the whole room, as ps5_notify.cpp does.
python3 - "$out" <<'PY' >&2
import sys
data = open(sys.argv[1], "rb").read()
marker = b"CRUX2-NOTIFY-RELAY-JSON-V1"
at = data.find(marker)
room = 16384
assert at >= 0 and data[at + len(marker):at + room] == bytes(room - len(marker)), "the relay's room for the JSON is not in the file"
print("notify relay: room for the notification found at %#x" % at)
PY
echo "$out"
