#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - the two lists the title looks things up in before it asks GitHub for a file:
#
#   ps5/package/resources/ps5/boxarts.txt       the names of libretro's PlayStation 2 box arts
#                                               (github.com/libretro-thumbnails/Sony_-_PlayStation_2,
#                                               Named_Boxarts): one a line, without ".png"
#   ps5/package/resources/ps5/cheat-index.txt   where the .pnach files of the cheat collections are:
#                                               "@id owner/repo branch" for each collection, then
#                                               "id<TAB>path" for each file
#
# Only names are kept here: the pictures and the cheats themselves are fetched by the title, on
# the console, when its user asks for them. Run before a build to bring the lists up to date.
#
#   ps5/tools/make-online-lists.sh [work folder]
set -euo pipefail
ps5=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
work=${1:-$(mktemp -d)}
out="$ps5/package/resources/ps5"
mkdir -p "$work" "$out"

fetch() { # fetch NAME URL: a clone without file contents, brought up to date
    if [[ -d $work/$1/.git ]]; then
        git -C "$work/$1" fetch -q --depth 1 origin && git -C "$work/$1" reset -q --hard FETCH_HEAD 2> /dev/null || true
    else
        git clone -q --depth 1 --filter=blob:none --no-checkout "$2" "$work/$1"
    fi
}

fetch lr-ps2 https://github.com/libretro-thumbnails/Sony_-_PlayStation_2
{
    echo "# libretro-thumbnails/Sony_-_PlayStation_2 Named_Boxarts at $(git -C "$work/lr-ps2" rev-parse --short=12 HEAD), $(git -C "$work/lr-ps2" log -1 --format=%cs)"
    git -C "$work/lr-ps2" -c core.quotepath=off ls-tree --name-only HEAD:Named_Boxarts | sed -n 's/\.png$//p'
} > "$out/boxarts.txt"

# The cheat collections, the most complete first. A collection's files under patches/ are patches
# (60 fps, no interlacing, no blur): the title puts those in the patches folder, beside PCSX2's own
# patches.zip, and the rest in the cheats folder. Templates and documentation are not listed.
sources=(
    "xs xs1l3n7x/pcsx2_cheats_collection main"
    "eu complicatiion/pcsx2_europe_cheats_collection main"
    "hj heavyjam-code/pcsx2-cheats-n-patches main"
    "ta TheAdmiester/PCSX2-Cheats main"
)
{
    echo "# .pnach files of cheat collections on GitHub; made by ps5/tools/make-online-lists.sh"
    for source in "${sources[@]}"; do
        read -r id repo branch <<< "$source"
        fetch "cheats-$id" "https://github.com/$repo"
        echo "@$id $repo $branch $(git -C "$work/cheats-$id" rev-parse --short=12 HEAD) $(git -C "$work/cheats-$id" log -1 --format=%cs)"
        git -C "$work/cheats-$id" -c core.quotepath=off ls-tree -r --name-only HEAD | grep -i '\.pnach$' | grep -iv -e '^templates/' -e '^docs/' | sed "s/^/$id\t/"
    done
} > "$out/cheat-index.txt"
wc -l "$out/boxarts.txt" "$out/cheat-index.txt"
