#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - the HD texture packs' names and unpacking (frontend/ps5_texture_packs_logic.h and
# ps5_texture_packs_extract.cpp) on a PC, with the address and undefined-behaviour sanitizers.
#
#   ps5/tests/texturepacks/run-texturepacks-tests.sh <work folder>
#
# Builds the libarchive the title links (.deps/src/libarchive, ps5/tools/build-deps.sh fetches
# it) for the PC, and unpacks libarchive's own RAR and RAR5 test archives as a stream of small
# pieces, the way a download hands them over. Prints "texturepacks: N checks passed".
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ps5=$(cd "$here/../.." && pwd)
front="$ps5/frontend"
src="$ps5/../.deps/src/libarchive"
work=${1:?usage: run-texturepacks-tests.sh <work folder>}
mkdir -p "$work"
[[ -d $src ]] || { echo "no libarchive source: run ps5/tools/build-deps.sh" >&2; exit 2; }
if [[ ! -f $work/libarchive/libarchive/libarchive.a ]]; then
    cmake -S "$src" -B "$work/libarchive" -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_TAR=OFF -DENABLE_CPIO=OFF -DENABLE_CAT=OFF \
        -DENABLE_UNZIP=OFF -DENABLE_TEST=OFF -DENABLE_OPENSSL=OFF -DENABLE_MBEDTLS=OFF -DENABLE_NETTLE=OFF -DENABLE_LIBB2=OFF \
        -DENABLE_LZ4=OFF -DENABLE_LZMA=OFF -DENABLE_ZSTD=OFF -DENABLE_BZip2=OFF -DENABLE_LIBXML2=OFF -DENABLE_EXPAT=OFF \
        -DENABLE_ICONV=OFF -DENABLE_ACL=OFF -DENABLE_XATTR=OFF -DENABLE_PCREPOSIX=OFF -DENABLE_PCRE2POSIX=OFF -DBUILD_SHARED_LIBS=OFF \
        >"$work/libarchive.log" 2>&1
    cmake --build "$work/libarchive" --target archive_static >>"$work/libarchive.log" 2>&1
fi
# libarchive's test archives, decoded from uuencode.
mkdir -p "$work/archives"
python3 - "$src/libarchive/test" "$work/archives" <<'PY'
import binascii, sys, pathlib
src, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
for name in ["test_read_format_rar.rar", "test_read_format_rar_compress_normal.rar", "test_read_format_rar_multi_lzss_blocks.rar",
             "test_read_format_rar5_multiple_files_solid.rar", "test_read_format_rar5_stored_manyfiles.rar", "test_read_format_rar5_compressed.rar"]:
    data = bytearray()
    for line in (src / (name + ".uu")).read_text().splitlines():
        if line.startswith("begin") or line.strip() in ("", "end", "`"):
            continue
        try:
            data += binascii.a2b_uu(line)
        except binascii.Error:
            nbytes = (((ord(line[0]) - 32) & 63) * 4 + 5) // 3
            data += binascii.a2b_uu(line[:nbytes])
    (out / name).write_bytes(bytes(data))
PY
flags=(-g -O1 -Wall -Wextra -Werror -std=c++20 -fsanitize=address,undefined -fno-sanitize-recover=undefined -I "$front" -I "$src/libarchive")
clang++ "${flags[@]}" "$here/texture_packs_test.cpp" "$front/ps5_texture_packs_extract.cpp" "$work/libarchive/libarchive/libarchive.a" -lz \
    -o "$work/texture_packs_test"
"$work/texture_packs_test" "$work/archives" "$work/out"
