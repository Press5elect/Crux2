#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - the interface on a PC (ps5/tests/bigpicture/preview.cpp).
#
#   ps5/tests/bigpicture/run-bigpicture-tests.sh <work folder> [crawl|shots|all]
#
# Builds the interface's screens with PCSX2's own ImGui (and FreeType, as the title has it) and
# the preview's Backend. "crawl" (the default): with the address and undefined-behaviour
# sanitizers, every settings row changed in every way, then 60,000 steps of random buttons across
# all screens, twice with different seeds. "shots": optimised, a picture of each screen (PNG, in
# <work folder>/shots), which is what one looks at after a change to a screen. "all": both.
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ps5=$(cd "$here/../.." && pwd)
root=$(cd "$ps5/.." && pwd)
work=${1:?usage: run-bigpicture-tests.sh <work folder> [crawl|shots|all]}
what=${2:-crawl}
imgui="$root/vendor/pcsx2/3rdparty/imgui"
front="$ps5/frontend/bigpicture"
mkdir -p "$work/fonts" "$work/shots"

# The fonts the title draws with.
cp "$root/vendor/pcsx2/bin/resources/fonts/Roboto-Regular.ttf" "$root/vendor/pcsx2/bin/resources/fonts/fa-solid-900.ttf" \
    "$ps5/package/resources/fonts/Roboto-Bold.ttf" "$ps5/package/resources/fonts/Saira-ExtraBold.ttf" "$work/fonts/"

sources=("$here/preview.cpp" "$front"/bp_*.cpp "$imgui/src/imgui.cpp" "$imgui/src/imgui_draw.cpp" "$imgui/src/imgui_tables.cpp"
    "$imgui/src/imgui_widgets.cpp" "$imgui/src/imgui_freetype.cpp")
sources=("${sources[@]/$front\/bp_pcsx2.cpp/}")   # the console's Backend is PCSX2's business
includes=(-I "$front" -I "$ps5/frontend" -I "$imgui/include" -I "$root/vendor/pcsx2/3rdparty/include" $(pkg-config --cflags freetype2))
build() { # build NAME FLAGS...
    local name=$1
    shift
    mkdir -p "$work/$name"
    local objects=()
    for source in "${sources[@]}"; do
        [[ -n $source ]] || continue
        local object="$work/$name/$(basename "$source").o"
        objects+=("$object")
        if [[ ! -f $object || $source -nt $object || $front/bp.h -nt $object || $front/bp_internal.h -nt $object ]]; then
            clang++ -std=c++20 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers "$@" "${includes[@]}" -c "$source" -o "$object" &
        fi
    done
    wait
    clang++ "$@" "${objects[@]}" $(pkg-config --libs freetype2) -lz -o "$work/$name/preview"
}

status=0
if [[ $what == crawl || $what == all ]]; then
    echo "== the interface, with the sanitizers"
    build asan -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
    pids=()
    for seed in 1 2; do
        ASAN_OPTIONS=detect_leaks=0 "$work/asan/preview" crawl "$work/fonts" "$seed" > "$work/crawl-$seed.log" 2>&1 &
        pids+=($!)
    done
    for index in 0 1; do
        wait "${pids[$index]}" || { tail -40 "$work/crawl-$((index + 1)).log"; status=1; }
        tail -1 "$work/crawl-$((index + 1)).log"
    done
fi

if [[ $what == shots || $what == all ]]; then
    echo "== the pictures"
    build fast -O2
    rm -f "$work/shots"/*.ppm "$work/shots"/*.png
    "$work/fast/preview" shots "$work/fonts" "$work/shots" > "$work/shots.log" 2>&1 || { tail -40 "$work/shots.log"; status=1; }
    tail -1 "$work/shots.log"
    python3 - "$work/shots" <<'PY'
import sys
from pathlib import Path
from PIL import Image
for ppm in sorted(Path(sys.argv[1]).glob("*.ppm")):
    Image.open(ppm).save(ppm.with_suffix(".png"), optimize=True)
    ppm.unlink()
PY
    ls "$work/shots" | wc -l
fi
exit $status
