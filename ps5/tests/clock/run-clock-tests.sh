#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - the title's clock from the processor's counter (frontend/ps5_clock.c), on the PC.
#
#   ps5/tests/clock/run-clock-tests.sh <work folder>
#
# Each case in a process of its own (the clock starts once in a process): with the address and
# undefined-behaviour sanitizers, and the threads case with the thread sanitizer.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
front="$here/../../frontend"
work=${1:?usage: run-clock-tests.sh <work folder>}
mkdir -p "$work"
cc -std=gnu11 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all \
    "$here/clock_test.c" "$front/ps5_clock.c" -o "$work/clock_test" -lpthread
cc -std=gnu11 -O1 -g -Wall -Wextra -fsanitize=thread \
    "$here/clock_test.c" "$front/ps5_clock.c" -o "$work/clock_test_tsan" -lpthread
for which in math exact fast slow far-fast far-slow sleep other not-believable; do
    "$work/clock_test" "$which"
done
"$work/clock_test_tsan" threads
echo "clock: all passed"
