#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - build the RADV archive Crux2 links: PS5 Mesa at the revision PS5_Vulkan pins, with every
# patch of ps5/patches/mesa applied in order, built by PS5_Vulkan's tools/build-radv.sh release.
#
#   ps5/tools/build-radv-crux2.sh
#   CRUX2_MESA_PATCHES=ps5/patches/mesa ps5/tools/build-radv-crux2.sh   (Mesa 26.2.5 instead)
#
# CRUX2_MESA_PATCHES names the folder of patches applied to the pin (ps5/patches/mesa-27 unless set;
# a relative path is taken from the repository's root): ps5/patches/mesa-27 carries the merge of
# Mesa main (27.0-devel) in place of the 26.2.x one. Either way the result is radv-crux2.
#
# Makes branch crux2-radv in ../PS5_Mesa (the pin plus one commit a patch; nothing is pushed),
# builds it, and puts the archive in PS5_Vulkan's .deps/native/radv-crux2, which ps5/tools/build.sh
# links when it is there. PS5_Vulkan's own release archive (.deps/native/radv-release) is left as
# it was. Without radv-crux2, build.sh links that one, which has none of the patches.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
vulkan=${PS5_VULKAN_DIR:-$root/../PS5_Vulkan}
mesa=${PS5_MESA_FORK:-$root/../PS5_Mesa}
patches=${CRUX2_MESA_PATCHES:-ps5/patches/mesa-27}
[[ $patches == /* ]] || patches="$root/$patches"
[[ -d $patches ]] && compgen -G "$patches/*.patch" > /dev/null ||
	{ echo "build-radv-crux2: no patches in $patches" >&2; exit 1; }
patches_name=${patches#"$root"/}

pin=$(sed -n 's/^mesa_revision=\${PS5_MESA_REVISION:-\([0-9a-f]*\)}$/\1/p' "$vulkan/tools/build-radv.sh")
[[ -n "$pin" ]] || { echo "build-radv-crux2: PS5_Vulkan's tools/build-radv.sh names no Mesa revision" >&2; exit 1; }
echo "==> PS5 Mesa ${pin:0:11} (PS5_Vulkan's pin) with $(ls "$patches"/*.patch | wc -l) patches"

git -C "$mesa" cat-file -e "$pin^{commit}" 2>/dev/null || git -C "$mesa" fetch --quiet origin
git -C "$mesa" checkout --quiet -B crux2-radv "$pin"
for patch in "$patches"/*.patch; do
	name=$(basename "$patch" .patch)
	if head -1 "$patch" | grep -q '^From [0-9a-f]\{40\}'; then
		# A git format-patch (author, date and message kept).
		git -C "$mesa" -c user.name=Crux2 -c user.email=noreply@anthropic.com am --quiet -3 "$patch"
	else
		git -C "$mesa" apply --index "$patch"
		git -C "$mesa" -c user.name=Crux2 -c user.email=noreply@anthropic.com commit --quiet -m "Crux2: $name"
	fi
	echo "    $name"
done
revision=$(git -C "$mesa" rev-parse HEAD)

native="$vulkan/.deps/native"
hold=""
if [[ -d "$native/radv-release" ]]; then
	hold=$(mktemp -d "$native/.radv-release-hold.XXXXXX")
	mv "$native/radv-release" "$hold/"
fi
restore() { [[ -n "$hold" && -d "$hold/radv-release" ]] && { rm -rf "$native/radv-release"; mv "$hold/radv-release" "$native/"; rmdir "$hold"; }; }
trap restore EXIT
(cd "$vulkan" && PS5_MESA_REVISION="$revision" bash tools/build-radv.sh release)
rm -rf "$native/radv-crux2"
mv "$native/radv-release" "$native/radv-crux2"
{
	echo
	echo "Crux2: PS5 Mesa $pin with $patches_name:"
	for patch in "$patches"/*.patch; do echo "  $(basename "$patch")"; done
} >> "$native/radv-crux2/PROVENANCE.txt"
echo "==> $native/radv-crux2 (Mesa crux2-radv ${revision:0:11}); ps5/tools/build.sh links it"
