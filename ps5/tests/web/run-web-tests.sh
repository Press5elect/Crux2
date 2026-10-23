#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - the web panel on a PC, with the address and undefined-behaviour sanitizers.
#
#   ps5/tests/web/run-web-tests.sh <work folder>
#
# Builds ps5_web.cpp and the QR code library with web_test.cpp, starts the server on 127.0.0.1
# (on a port the system picks) and asks it everything the page can, and a few thousand requests
# of random bytes. Prints "web: N checks passed"; a failure makes the exit status non-zero.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
front=$(cd "$here/../../frontend" && pwd)
work=${1:?usage: run-web-tests.sh <work folder>}
mkdir -p "$work"
sanitize=(-g -O1 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
# ps5_web.cpp as the title may build it: without exceptions, and with no warning let through.
clang "${sanitize[@]}" -std=c11 -c "$front/third_party/qrcodegen/qrcodegen.c" -o "$work/qrcodegen.o" & one=$!
clang++ "${sanitize[@]}" -std=c++20 -fno-exceptions -Werror -I "$front" -c "$front/ps5_web.cpp" -o "$work/ps5_web.o" & two=$!
clang++ "${sanitize[@]}" -std=c++20 -I "$front" -c "$here/web_test.cpp" -o "$work/web_test.o" & three=$!
wait $one
wait $two
wait $three
clang++ "${sanitize[@]}" "$work/web_test.o" "$work/ps5_web.o" "$work/qrcodegen.o" -lpthread -o "$work/web_test"
ASAN_OPTIONS=detect_leaks=1 "$work/web_test"
