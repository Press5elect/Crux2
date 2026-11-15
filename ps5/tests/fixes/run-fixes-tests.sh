#!/usr/bin/env bash
# Crux2 - the PC test of the game fixes fetched at start (frontend/ps5_fixes_fetch.cpp), with the
# address and undefined-behaviour sanitizers, and the check that ps5/game-fixes.txt is current.
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0+
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
#
#   ps5/tests/fixes/run-fixes-tests.sh <work folder>
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ps5=$(cd "$here/../.." && pwd)
work=${1:?usage: run-fixes-tests.sh <work folder>}
mkdir -p "$work"
work=$(cd "$work" && pwd)
python3 "$ps5/tools/game-fixes-index.py" --check
"${CXX:-c++}" -std=c++20 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
	-fno-omit-frame-pointer -DCRUX2_FIXES_TEST -I "$ps5/frontend" \
	"$here/fixes_test.cpp" "$ps5/frontend/ps5_fixes_fetch.cpp" -o "$work/fixes_test"
"$work/fixes_test" "$work/run" 2> "$work/fixes_test.log" || { cat "$work/fixes_test.log"; exit 1; }
