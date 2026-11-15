#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - HD texture pack downloads on a PC: the pieces and their order (pieces_test.cpp, with the
# address, undefined-behaviour and thread sanitizers), and, with the PC's libcurl and libarchive
# (libcurl4-openssl-dev, libarchive-dev), the download itself against a made-up Internet Archive on
# 127.0.0.1 (range_server.py, fetch_test.cpp).
#
#   ps5/tests/texturepacks/run-fetch-tests.sh <work folder>
#
#   CXX   the compiler (default c++)
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
front=$(cd "$here/../../frontend" && pwd)
work=${1:?usage: run-fetch-tests.sh <work folder>}
mkdir -p "$work"
CXX=${CXX:-c++}
flags=(-std=c++20 -O1 -g -Wall -Wextra -Werror -pthread -I "$front")
san=(-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
status=0

"$CXX" "${flags[@]}" "${san[@]}" "$here/pieces_test.cpp" -o "$work/pieces_test"
"$work/pieces_test" || status=1
if "$CXX" "${flags[@]}" -fsanitize=thread "$here/pieces_test.cpp" -o "$work/pieces_test_tsan" 2>/dev/null; then
    "$work/pieces_test_tsan" || status=1
fi

if ! pkg-config --exists libcurl libarchive 2>/dev/null; then
    echo "fetch: skipped (needs the PC's libcurl and libarchive: libcurl4-openssl-dev, libarchive-dev)"
    exit $status
fi
# shellcheck disable=SC2046
"$CXX" "${flags[@]}" "${san[@]}" "$here/fetch_test.cpp" "$front/ps5_texture_packs_extract.cpp" $(pkg-config --cflags --libs libcurl libarchive) \
    -o "$work/fetch_test"

# The files: 3 MB and a bit, and a pack of made-up textures zipped.
files="$work/files"
rm -rf "$files"
mkdir -p "$files"
python3 -I - "$files" <<'PY'
import os, random, sys, zipfile
out = sys.argv[1]
r = random.Random(7)
open(os.path.join(out, "data.bin"), "wb").write(bytes(r.getrandbits(8) for _ in range(3 * 1024 * 1024 + 4321)))
base = os.path.join(out, "pack", "SLUS-00000", "replacements")
os.makedirs(base)
with zipfile.ZipFile(os.path.join(out, "pack.zip"), "w") as z:
    for i in range(40):
        name = f"{r.getrandbits(64):016x}-{i}.png"
        data = bytes(r.getrandbits(8) for _ in range(r.randint(1, 60000))) + b"\0" * r.randint(0, 40000)
        open(os.path.join(base, name), "wb").write(data)
        z.write(os.path.join(base, name), f"SLUS-00000/replacements/{name}", zipfile.ZIP_DEFLATED if i % 2 else zipfile.ZIP_STORED)
PY

rm -f "$work/port"
python3 -I "$here/range_server.py" "$files" "$work/port" &
server=$!
trap 'kill $server 2>/dev/null || true' EXIT
for _ in $(seq 100); do
    [[ -s $work/port ]] && break
    python3 -c 'import time; time.sleep(0.05)'
done
[[ -s $work/port ]] || { echo "fetch: the made-up server did not start"; exit 1; }
# (Straight to 127.0.0.1, whatever proxy the PC has.)
env -u http_proxy -u HTTP_PROXY -u https_proxy -u HTTPS_PROXY -u all_proxy -u ALL_PROXY no_proxy='*' \
    "$work/fetch_test" "http://127.0.0.1:$(cat "$work/port")" "$files" "$work" || status=1
exit $status
