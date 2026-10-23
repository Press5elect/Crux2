#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 Recomp - fetch paraLLEl-GS (Arntzen Software, LGPL-3.0-or-later), the GS on the graphics card,
# with the parts of Granite (Hans-Kristian Arntzen, MIT) it is built on, at the pinned revisions into
# vendor/parallel-gs, with def-jam-vendetta-recomp's changes (jcsix694, GPL-3.0) and this title's
# (ps5/patches/parallel-gs/ps5.patch) applied.
#
#   ps5/tools/fetch-parallel-gs.sh            fetch and patch (again, when a pin or a patch changed)
#   ps5/tools/fetch-parallel-gs.sh --export   write the working tree's changes (past the Vendetta
#                                             patch) back to ps5.patch
set -euo pipefail
pgs_url=https://github.com/Arntzen-Software/parallel-gs.git
pgs_revision=cc6184af7e0c03da603045ca371ffa5dae9b0655
granite_url=https://github.com/Themaister/Granite.git
granite_revision=16e7395f6a4858c1783dbf6f521f90b9d5f82ac5
volk_url=https://github.com/zeux/volk.git
volk_revision=47cddf7ed97b94118a08aacb548a411188e016cc
headers_url=https://github.com/KhronosGroup/Vulkan-Headers.git
headers_revision=11d6898377797e07dbd543aaaa367e4465074597
ps5=$(cd "$(dirname "$0")/.." && pwd)
root=$(dirname "$ps5")
tree="$root/vendor/parallel-gs"
vendetta="$ps5/patches/parallel-gs/def-jam-vendetta-recomp.patch"
patch="$ps5/patches/parallel-gs/ps5.patch"

if [[ ${1:-} == --export ]]; then
	git -C "$tree" add -N gs; git -C "$tree/Granite" add -N vulkan util math
	{
		echo "Crux2 Recomp's changes to paraLLEl-GS and Granite for the PS5 ($pgs_url $pgs_revision,"
		echo "$granite_url $granite_revision). Copyright (C) 2026 Press5elect; under the licences of the"
		echo "files they change (paraLLEl-GS: LGPL-3.0-or-later, Granite: MIT)."
		echo
		git -C "$tree" diff HEAD -- . ":(exclude)Granite"
		git -C "$tree/Granite" diff HEAD --src-prefix=a/Granite/ --dst-prefix=b/Granite/
	} > "$patch"
	echo "wrote $patch"
	exit 0
fi

hash_of() { [[ -f $1 ]] && sha256sum "$1" | cut -c1-12 || echo none; }
stamp="$tree/.crux2-recomp-$pgs_revision-$(hash_of "$vendetta")-$(hash_of "$patch")"
if [[ -f $stamp ]]; then exit 0; fi

checkout() { # url revision folder
	if [[ ! -d $3/.git ]]; then
		mkdir -p "$3"
		git -C "$3" init --quiet
		git -C "$3" remote add origin "$1"
	fi
	git -C "$3" fetch --quiet --depth 1 origin "$2" 2>/dev/null || git -C "$3" fetch --quiet origin
	git -C "$3" checkout --quiet --force "$2"
	git -C "$3" clean -fdq
}
checkout "$pgs_url" "$pgs_revision" "$tree"
checkout "$granite_url" "$granite_revision" "$tree/Granite"
checkout "$volk_url" "$volk_revision" "$tree/Granite/third_party/volk"
checkout "$headers_url" "$headers_revision" "$tree/Granite/third_party/khronos/vulkan-headers"
# def-jam-vendetta-recomp's changes (paraLLEl-GS's files), committed so that --export writes only ours
git -C "$tree" apply "$vendetta"
git -C "$tree" -c user.name=crux2 -c user.email=crux2@local commit --quiet -am "def-jam-vendetta-recomp's changes"
if [[ -f $patch ]]; then
	# (Granite's part goes to Granite's own tree)
	python3 - "$patch" "$tree" <<'EOF'
import re, subprocess, sys
text = open(sys.argv[1]).read()
parts = re.split(r'(?m)^(?=diff --git )', text)
outer = ''.join(p for p in parts if p.startswith('diff --git') and not p.startswith('diff --git a/Granite/'))
inner = ''.join(p for p in parts if p.startswith('diff --git a/Granite/'))
for body, cwd, strip in ((outer, sys.argv[2], 1), (inner, sys.argv[2] + '/Granite', 2)):
    if body:
        subprocess.run(['git', 'apply', f'-p{strip}', '-'], input=body.encode(), cwd=cwd, check=True)
EOF
fi
rm -f "$tree"/.crux2-recomp-*
touch "$stamp"
echo "paraLLEl-GS $pgs_revision in $tree"
