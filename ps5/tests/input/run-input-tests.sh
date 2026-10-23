#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - the PC tests of the controller features: the leave-the-game hold and its buzz, pressure
# from R2, motion aim, the USB keyboard's reports and key names, and USB pads (the HID report
# descriptor parser, the reports' decoding, the hot-plug bookkeeping over a made-up transport).
# With the address and undefined-behaviour sanitizers. Then the keyboard's key names are followed
# through PCSX2's own USB keyboard tables (keymap_check.py).
#
#   ps5/tests/input/run-input-tests.sh <work folder>
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ps5=$(cd "$here/../.." && pwd)
root=$(cd "$ps5/.." && pwd)
work=${1:?usage: run-input-tests.sh <work folder>}
mkdir -p "$work"
export ASAN_OPTIONS=detect_leaks=1
clang++ -std=c++20 -g -O1 -Wall -Wextra -Werror -fno-exceptions -fsanitize=address,undefined -fno-sanitize-recover=undefined \
	-fno-omit-frame-pointer -I "$ps5/frontend" "$here/input_test.cpp" "$ps5/frontend/ps5_usbpad.cpp" -lpthread \
	-o "$work/input_test"
status=0
"$work/input_test" 2> >(grep -v '^usb pads:' >&2) || status=1
python3 -I "$here/keymap_check.py" "$root" || status=1
if [ "$status" -eq 0 ]; then
	echo "input tests passed"
else
	echo "input tests FAILED"
fi
exit "$status"
