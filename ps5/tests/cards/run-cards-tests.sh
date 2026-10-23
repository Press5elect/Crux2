#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - the PC tests of the memory card file system and the save files (ps5/frontend/ps5_mc.h),
# with the address and undefined-behaviour sanitizers.
#
#   ps5/tests/cards/run-cards-tests.sh <work folder>
#
# With MYMC_PYTHON set to a Python that has mymc+ installed (pip install mymcplus), every
# operation is also held against that program (cross_check.py). Without it only the checks that
# need nothing else run, and the script says that the comparison did not.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ps5=$(cd "$here/../.." && pwd)
work=${1:?usage: run-cards-tests.sh <work folder>}
mkdir -p "$work"
export ASAN_OPTIONS=detect_leaks=1
clang++ -std=c++20 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer \
	-I "$ps5/frontend" "$here/mc_test.cpp" -lz -o "$work/mc_test"
"$work/mc_test" self "$work"
if [ -n "${MYMC_PYTHON:-}" ] && "$MYMC_PYTHON" -c 'import mymcplus' 2>/dev/null; then
	python3 "$here/cross_check.py" "$work/mc_test" "$MYMC_PYTHON" "$work/cross"
else
	echo "memory cards against mymc+: NOT RUN (set MYMC_PYTHON to a Python with mymcplus installed)"
fi
