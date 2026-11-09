#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 Recomp - fetch PS2Recomp's runtime (ran-j/PS2Recomp, GPL-3.0) at the pinned revision into
# vendor/PS2Recomp, with def-jam-vendetta-recomp's runtime work (jcsix694, GPL-3.0:
# ps5/patches/ps2recomp/def-jam-vendetta-recomp.patch, committed there) and this title's changes
# (ps5/patches/ps2recomp/runner.patch) applied.
#
#   ps5/tools/fetch-ps2recomp.sh            fetch and patch (again, when the pin or the patch changed)
#   ps5/tools/fetch-ps2recomp.sh --export   write the working tree's changes back to the patch
set -euo pipefail
url=https://github.com/ran-j/PS2Recomp.git
revision=eb8688b2fd191129f103a39543002c005b33409c
ps5=$(cd "$(dirname "$0")/.." && pwd)
root=$(dirname "$ps5")
tree="$root/vendor/PS2Recomp"
patch="$ps5/patches/ps2recomp/runner.patch"
vendetta="$ps5/patches/ps2recomp/def-jam-vendetta-recomp.patch"

if [[ ${1:-} == --export ]]; then
	git -C "$tree" add -N ps2xRecomp ps2xRuntime ps2xIOP
	{
		echo "Crux2 Recomp's changes to PS2Recomp ($url $revision). Copyright (C) 2026 Press5elect."
		echo "GPL-3.0-or-later, with the attribution term of the NOTICE: keep \"Crux2 by Press5elect\"."
		echo
		git -C "$tree" diff HEAD
	} > "$patch"
	echo "wrote $patch"
	exit 0
fi

patch_hash=none
[[ -f $patch ]] && patch_hash=$(cat "$vendetta" "$patch" | sha256sum | cut -c1-12)
stamp="$tree/.crux2-recomp-$revision-$patch_hash"
if [[ -f $stamp ]]; then exit 0; fi
if [[ ! -d $tree/.git ]]; then
	git clone --quiet --no-checkout "${PS2RECOMP_MIRROR:-$url}" "$tree"
	git -C "$tree" remote set-url origin "$url"
fi
git -C "$tree" fetch --quiet origin "$revision" 2>/dev/null || git -C "$tree" fetch --quiet origin
git -C "$tree" checkout --quiet --force "$revision"
git -C "$tree" clean -fdq
git -C "$tree" apply "$vendetta"
git -C "$tree" add -A ps2xRecomp ps2xRuntime ps2xIOP
git -C "$tree" -c user.name=crux2 -c user.email=crux2@local commit --quiet -m "def-jam-vendetta-recomp's runtime changes"
if [[ -f $patch ]]; then git -C "$tree" apply "$patch"; fi
rm -f "$tree"/.crux2-recomp-*
touch "$stamp"
echo "PS2Recomp $revision in $tree"
