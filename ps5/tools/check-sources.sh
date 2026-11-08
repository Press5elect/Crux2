#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - before a build: is every source it is made from the newest that should be taken?
#
#   ps5/tools/check-sources.sh            report; exit 1 when a pin is behind
#   ps5/tools/check-sources.sh --warn     report only (exit 0)
#
# Each source is compared with what it follows (git ls-remote, or a shallow fetch of one file into
# a scratch folder; nothing is cloned):
#
#   PCSX2            its master (its builds are nightly: a pin behind master is behind); the newest
#                    release tag is shown too
#   Mesa             main on freedesktop.org, against the main revision ps5/patches/mesa-27's
#                    0012 is merged from (its README's first "main at `...`")
#   PS5 Mesa         the revision PS5_Vulkan pins (tools/build-radv.sh): Crux2's patches go on
#                    that; the fork's newest commit is shown, as information
#   PS5_Vulkan       its newest commit, against the README's "PS5_Vulkan `...`"
#   PS5_PayloadSDK   its newest commit
#   paraLLEl-GS      its newest commit
#   Granite, volk    the revisions paraLLEl-GS itself pins (its submodules), not their newest
#   PS2Recomp        its newest commit
#
# build.sh runs this first and stops when something is behind; PS5PX_ALLOW_STALE=1 builds anyway
# (say why in the commit that does). A source that cannot be reached is said so and does not stop
# the build.
set -uo pipefail

ps5=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
root=$(dirname "$ps5")
warn=false
[[ ${1:-} == --warn ]] && warn=true
behind=0
unreachable=0
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT

pin_of() { sed -n "s/^$2=\\([0-9a-f]\\{40\\}\\).*/\\1/p" "$1" | head -1; }
head_of() { timeout 60 git ls-remote "$1" "${2:-HEAD}" 2> /dev/null | awk 'NR==1{print $1}'; }

# A shallow, blob-less fetch of one revision of a repository (its tree; blobs on demand).
fetched() {
    local name=$1 url=$2 ref=${3:-HEAD} dir="$scratch/$1"
    if [[ ! -d $dir ]]; then
        git init -q "$dir" && git -C "$dir" remote add origin "$url" &&
            timeout 120 git -C "$dir" fetch -q --depth=1 --filter=blob:none origin "$ref" 2> /dev/null ||
            return 1
    fi
    echo "$dir"
}

# name  pinned  wanted  [what wanted is]  (a pin may be short: compared as a prefix)
report() {
    local name=$1 pin=$2 want=$3 what=${4:-upstream}
    if [[ -z $pin ]]; then
        printf '  ?       %-18s no pin found\n' "$name"
    elif [[ -z $want ]]; then
        printf '  offline %-18s %s could not be read\n' "$name" "$what"
        unreachable=$((unreachable + 1))
    elif [[ $want == "$pin"* || $pin == "$want"* ]]; then
        printf '  current %-18s %s\n' "$name" "${pin:0:12}"
    else
        printf '  BEHIND  %-18s pinned %s, %s %s\n' "$name" "${pin:0:12}" "$what" "${want:0:12}"
        behind=$((behind + 1))
    fi
}
info() { printf '  info    %-18s %s\n' "$1" "$2"; }

echo "==> sources against what they follow"

report PCSX2 "$(pin_of "$ps5/tools/fetch-pcsx2.sh" revision)" "$(head_of https://github.com/PCSX2/pcsx2.git)" master
tag=$(timeout 60 git ls-remote --tags --refs https://github.com/PCSX2/pcsx2.git 'v2.*' 2> /dev/null |
    awk '{sub("refs/tags/","",$2); print $2}' | sort -V | tail -1)
[[ -n $tag ]] && info PCSX2 "newest release tag $tag"

mesa_base=$(sed -n 's/.*main at `\([0-9a-f]\{7,\}\)`.*/\1/p' "$ps5/patches/mesa-27/README.md" | head -1)
report "Mesa (0012)" "$mesa_base" "$(head_of https://gitlab.freedesktop.org/mesa/mesa.git refs/heads/main)" "Mesa main"

# PS5 Mesa: the revision PS5_Vulkan pins (its checkout beside this repository, else its newest commit
# on GitHub), which build-radv-crux2.sh puts Crux2's patches on.
vulkan=${PS5_VULKAN_DIR:-$root/../PS5_Vulkan}
radv_script=
if [[ -f $vulkan/tools/build-radv.sh ]]; then
    radv_script=$(cat "$vulkan/tools/build-radv.sh")
elif dir=$(fetched PS5_Vulkan https://github.com/mihawk-99/PS5_Vulkan.git); then
    radv_script=$(git -C "$dir" show FETCH_HEAD:tools/build-radv.sh 2> /dev/null)
fi
vk_mesa_pin=$(sed -n 's/^mesa_revision=\${PS5_MESA_REVISION:-\([0-9a-f]*\)}$/\1/p' <<< "$radv_script")
readme_mesa=$(sed -n 's/.*RADV is PS5 Mesa `\([0-9a-f]\{7,\}\)`.*/\1/p' "$root/README.md" | head -1)
report "PS5 Mesa" "$readme_mesa" "$vk_mesa_pin" "PS5_Vulkan pins"
fork_head=$(head_of https://github.com/mihawk-99/PS5_Mesa.git)
[[ -n $fork_head && -n $vk_mesa_pin && $fork_head != "$vk_mesa_pin" ]] &&
    info "PS5 Mesa" "the fork's newest is ${fork_head:0:12} (not pinned by PS5_Vulkan yet)"

readme_vulkan=$(sed -n 's/.*PS5_Vulkan `\([0-9a-f]\{7,\}\)`.*/\1/p' "$root/README.md" | head -1)
report PS5_Vulkan "$readme_vulkan" "$(head_of https://github.com/mihawk-99/PS5_Vulkan.git)"
report PS5_PayloadSDK "$(pin_of "$ps5/tools/setup-sdk.sh" sdk_revision)" "$(head_of https://github.com/mihawk-99/PS5_PayloadSDK.git)"

pgs_script=$root/crux2recomp/ps5/tools/fetch-parallel-gs.sh
pgs_pin=$(pin_of "$pgs_script" pgs_revision)
report paraLLEl-GS "$pgs_pin" "$(head_of https://github.com/Arntzen-Software/parallel-gs.git)"
granite_want= volk_want=
if [[ -n $pgs_pin ]] && dir=$(fetched parallel-gs https://github.com/Arntzen-Software/parallel-gs.git "$pgs_pin"); then
    granite_want=$(git -C "$dir" ls-tree FETCH_HEAD Granite 2> /dev/null | awk '{print $3}')
    if [[ -n $granite_want ]] && gdir=$(fetched Granite https://github.com/Themaister/Granite.git "$granite_want"); then
        volk_want=$(git -C "$gdir" ls-tree -r FETCH_HEAD third_party/volk 2> /dev/null | awk '$2=="commit"{print $3; exit}')
    fi
fi
report Granite "$(pin_of "$pgs_script" granite_revision)" "$granite_want" "paraLLEl-GS pins"
report volk "$(pin_of "$pgs_script" volk_revision)" "$volk_want" "Granite pins"

report PS2Recomp "$(pin_of "$root/crux2recomp/ps5/tools/fetch-ps2recomp.sh" revision)" "$(head_of https://github.com/ran-j/PS2Recomp.git)"

echo
if ((behind > 0)); then
    echo "$behind source(s) behind: move the pin (and re-export the patches), or PS5PX_ALLOW_STALE=1" >&2
    $warn || exit 1
elif ((unreachable > 0)); then
    echo "all sources that could be read are current ($unreachable could not be checked)"
else
    echo "all sources are current"
fi
