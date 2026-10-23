#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - the disc image writer: each format made from a made-up disc and read back (CHD by
# libchdr, the reader PCSX2 uses). Needs the zlib and zstd development headers.
#
#   ps5/tests/discimage/run-discimage-test.sh <work folder>
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ps5=$(cd "$here/../.." && pwd)
v=$(cd "$ps5/../vendor/pcsx2/3rdparty" && pwd)
work=${1:?usage: run-discimage-test.sh <work folder>}
mkdir -p "$work/obj"
for f in LzmaDec LzmaEnc LzFind LzFindOpt CpuArch Alloc; do
	clang -O2 -c -D_7ZIP_ST -DZ7_ST -I "$v/lzma/include" "$v/lzma/src/$f.c" -o "$work/obj/$f.o"
done
for f in libchdr_bitstream libchdr_cdrom libchdr_chd libchdr_flac libchdr_huffman; do
	clang -O2 -w -c -D_7ZIP_ST -I "$v/libchdr/include" -I "$v/lzma/include" "$v/libchdr/src/$f.c" -o "$work/obj/$f.o"
done
clang++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined -I "$ps5/frontend" -I "$v/libchdr/include" \
	-I "$v/lzma/include" "$here/disc_image_test.cpp" "$ps5/frontend/ps5_disc_image.cpp" "$work"/obj/*.o -lz -lzstd -lpthread -o "$work/disc_image_test"
"$work/disc_image_test" "$work"
