#!/usr/bin/env bash
# Crux2 - the PC test of the updater (ps5/frontend/ps5_update.cpp), with the address and
# undefined-behaviour sanitizers.
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0+
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
#
#   ps5/tests/update/run-update-tests.sh <work folder>
#
# libzip: the PC's own (libzip-dev, found by pkg-config) when there is one; otherwise PCSX2's copy
# in vendor/pcsx2/3rdparty/libzip is built once into the work folder (libzip-pc/CMakeLists.txt,
# deflate only; needs cmake and zlib). rapidjson is PCSX2's copy, header-only. When a release ZIP
# was made in this tree (dist/release-<tag>/Crux2-<tag>.zip) the newest is installed as well.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ps5=$(cd "$here/../.." && pwd)
root=$(cd "$ps5/.." && pwd)
work=${1:?usage: run-update-tests.sh <work folder>}
mkdir -p "$work"
work=$(cd "$work" && pwd)

if pkg-config --exists libzip 2>/dev/null; then
	read -r -a zipflags <<< "$(pkg-config --cflags --libs libzip)"
else
	if [ ! -f "$work/libzip/libzip.a" ]; then
		generator=()
		command -v ninja > /dev/null && generator=(-G Ninja)
		cmake -S "$here/libzip-pc" -B "$work/libzip" "${generator[@]}" -DCMAKE_BUILD_TYPE=Release \
			-DLIBZIP_SOURCE="$root/vendor/pcsx2/3rdparty/libzip" > "$work/libzip-build.log" 2>&1 &&
			cmake --build "$work/libzip" >> "$work/libzip-build.log" 2>&1 ||
			{ echo "update: libzip could not be built (see $work/libzip-build.log)"; exit 2; }
	fi
	zipflags=(-isystem "$root/vendor/pcsx2/3rdparty/libzip/lib" -isystem "$work/libzip" "$work/libzip/libzip.a" -lz)
fi

export ASAN_OPTIONS=detect_leaks=1
export UBSAN_OPTIONS=print_stacktrace=1
clang++ -std=c++20 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
	-fno-omit-frame-pointer -pthread -DCRUX2_UPDATE_TEST -I "$ps5/frontend" \
	-isystem "$root/vendor/pcsx2/3rdparty/rapidjson/include" \
	"$here/update_test.cpp" "$ps5/frontend/ps5_update.cpp" "${zipflags[@]}" -o "$work/update_test"
# A real release ZIP as well, when one was made here (dist/release-<tag>/Crux2-<tag>.zip, the newest).
real=$(ls -1 "$root"/dist/release-*/Crux2-v*.zip 2>/dev/null | grep -v -e '-source\.zip$' -e '-part[0-9]' | sort -V | tail -n 1 || true)
rm -rf "$work/run"
# The steps go to update_test.log; the summary line to the screen. On a failure, the log too.
"$work/update_test" "$work/run" ${real:+"$real"} 2> "$work/update_test.log" || { cat "$work/update_test.log"; exit 1; }
