#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - fetch PCSX2 at the pinned revision into vendor/pcsx2 and apply
# the port's changes (ps5/patches/pcsx2/ps5.patch).
#
#   ps5/tools/fetch-pcsx2.sh            fetch and patch (does nothing when it is there)
#   ps5/tools/fetch-pcsx2.sh --export   write the working tree's changes back to the patch
#
# PCSX2's source is not kept in this repository: the build takes upstream's tree
# as it is at the revision below and lays the port's changes over it, so what the
# port changed in PCSX2 is one file to read.

set -euo pipefail

ps5=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
root=$(dirname "$ps5")
vendor="$root/vendor/pcsx2"
patch="$ps5/patches/pcsx2/ps5.patch"
revision=ab0b401f99729ef8da3009f9af28d8ee96e0e7ba
url=https://github.com/PCSX2/pcsx2

if [[ ${1:-} == --export ]]; then
    mkdir -p "$(dirname "$patch")"
    git -C "$vendor" add -N .
    {
        echo "Crux2's changes to PCSX2 (the PS5 port), on PCSX2 $revision."
        echo "Copyright (C) 2026 Press5elect. GPL-3.0-or-later, with the attribution term of NOTICE"
        echo "(GPL-3.0 section 7(b)): keep \"Crux2 by Press5elect\". PCSX2 itself: PCSX2 Dev Team."
        echo
        git -C "$vendor" diff "$revision"
    } > "$patch"
    echo "wrote $patch ($(grep -c '^diff --git' "$patch") files)"
    exit 0
fi

if [[ -f $vendor/CMakeLists.txt ]]; then
    echo "vendor/pcsx2 is there ($(git -C "$vendor" rev-parse --short HEAD))" >&2
    exit 0
fi

mkdir -p "$vendor"
git -C "$vendor" init --quiet
git -C "$vendor" remote add origin "$url"
git -C "$vendor" fetch --quiet --depth 1 origin "$revision"
git -C "$vendor" checkout --quiet FETCH_HEAD
git -C "$vendor" apply --whitespace=nowarn "$patch"
echo "vendor/pcsx2: $revision with the PS5 changes" >&2
