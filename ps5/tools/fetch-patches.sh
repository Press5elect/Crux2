#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - the game patches (widescreen, no-interlacing, and the fixes the game database
# switches on): patches.zip of PCSX2's own patch repository, which PCSX2's builds package the same
# way. Prints the file's path.
#
#   ps5/tools/fetch-patches.sh
#
# The file is fetched once into .deps/patches/ and kept; delete it to fetch the current one.
set -euo pipefail

ps5=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
root=$(dirname "$ps5")
dir="$root/.deps/patches"
file="$dir/patches.zip"
url=https://github.com/PCSX2/pcsx2_patches/releases/latest/download/patches.zip

if [[ ! -s $file ]]; then
    mkdir -p "$dir"
    curl --fail --silent --show-error --location --retry 3 "$url" -o "$file.download"
    # A zip archive, not an error page.
    python3 -c 'import sys, zipfile; z = zipfile.ZipFile(sys.argv[1]); assert len(z.namelist()) > 100' "$file.download"
    mv -- "$file.download" "$file"
fi
echo "$file"
