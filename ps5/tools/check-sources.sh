#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - before a build: is every source it is made from the newest upstream has?
#
#   ps5/tools/check-sources.sh            report; exit 1 when a pin is behind its upstream
#   ps5/tools/check-sources.sh --warn     report only (exit 0)
#
# Each pinned source is compared with its upstream's newest commit (git ls-remote, nothing is
# cloned) and, for PCSX2, with its newest release tag too (PCSX2's builds are nightly: a pin
# behind master is behind). build.sh runs this first and stops when something is behind;
# PS5PX_ALLOW_STALE=1 builds anyway (say why in the commit that does). A source that cannot be
# reached is said so and does not stop the build.
set -uo pipefail

ps5=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
root=$(dirname "$ps5")
warn=false
[[ ${1:-} == --warn ]] && warn=true
behind=0
unreachable=0

pin_of() { sed -n "s/^$2=\\([0-9a-f]\\{40\\}\\).*/\\1/p" "$1" | head -1; }

# name  url  pinned-revision  [branch-ref]
check() {
    local name=$1 url=$2 pin=$3 ref=${4:-HEAD} head
    if [[ -z $pin ]]; then
        printf '  ?       %-26s no pin found\n' "$name"
        return
    fi
    head=$(timeout 30 git ls-remote "$url" "$ref" 2> /dev/null | awk 'NR==1{print $1}')
    if [[ -z $head ]]; then
        printf '  offline %-26s %s: could not be reached\n' "$name" "$url"
        unreachable=$((unreachable + 1))
    elif [[ $head == "$pin" ]]; then
        printf '  current %-26s %s\n' "$name" "${pin:0:12}"
    else
        printf '  BEHIND  %-26s pinned %s, upstream %s\n' "$name" "${pin:0:12}" "${head:0:12}"
        behind=$((behind + 1))
    fi
}

echo "==> sources against their upstreams"
check PCSX2 https://github.com/PCSX2/pcsx2.git "$(pin_of "$ps5/tools/fetch-pcsx2.sh" revision)"
tag=$(timeout 30 git ls-remote --tags --refs https://github.com/PCSX2/pcsx2.git 'v2.*' 2> /dev/null |
    awk '{sub("refs/tags/","",$2); print $2}' | sort -V | tail -1)
[[ -n $tag ]] && printf '  info    %-26s newest release tag %s\n' PCSX2 "$tag"
check PS5_PayloadSDK https://github.com/mihawk-99/PS5_PayloadSDK.git "$(pin_of "$ps5/tools/setup-sdk.sh" sdk_revision)"

# Mesa and PS5_Vulkan's pins are in the PS5_Vulkan checkout beside this repository when it is there.
vulkan=${PS5_VULKAN_DIR:-$root/../PS5_Vulkan}
if [[ -f $vulkan/tools/build-radv.sh ]]; then
    mesa_pin=$(sed -n 's/^mesa_revision=\${PS5_MESA_REVISION:-\([0-9a-f]*\)}$/\1/p' "$vulkan/tools/build-radv.sh")
    vulkan_pin=$(git -C "$vulkan" rev-parse HEAD 2> /dev/null)
    check "PS5_Mesa (RADV)" https://github.com/mihawk-99/PS5_Mesa.git "$mesa_pin"
    check PS5_Vulkan https://github.com/mihawk-99/PS5_Vulkan.git "$vulkan_pin"
else
    # No checkout: the README names the pin (short), which is compared as a prefix.
    short=$(sed -n 's/.*RADV is PS5 Mesa `\([0-9a-f]\{7,\}\)`.*/\1/p' "$root/README.md" | head -1)
    mesa_head=$(timeout 30 git ls-remote https://github.com/mihawk-99/PS5_Mesa.git HEAD 2> /dev/null | awk 'NR==1{print $1}')
    if [[ -z $short || -z $mesa_head ]]; then
        printf '  ?       %-26s no PS5_Vulkan checkout, and no pin or upstream could be read\n' "PS5_Mesa (RADV)"
    elif [[ $mesa_head == "$short"* ]]; then
        printf '  current %-26s %s (README pin)\n' "PS5_Mesa (RADV)" "$short"
    else
        printf '  BEHIND  %-26s README pins %s, upstream %s\n' "PS5_Mesa (RADV)" "$short" "${mesa_head:0:12}"
        behind=$((behind + 1))
    fi
fi
# (The Mesa the RADV archive was built from, and the archive's date, are in the boot log; a
#  newer PS5_Mesa means build-radv-crux2.sh should run again, which this script does not do.)

pgs=$root/crux2recomp/ps5/tools/fetch-parallel-gs.sh
check paraLLEl-GS https://github.com/Arntzen-Software/parallel-gs.git "$(pin_of "$pgs" pgs_revision)"
check Granite https://github.com/Themaister/Granite.git "$(pin_of "$pgs" granite_revision)"
check volk https://github.com/zeux/volk.git "$(pin_of "$pgs" volk_revision)"
check PS2Recomp https://github.com/ran-j/PS2Recomp.git "$(pin_of "$root/crux2recomp/ps5/tools/fetch-ps2recomp.sh" revision)"

echo
if ((behind > 0)); then
    echo "$behind source(s) behind upstream: move the pin (and re-export the patches), or PS5PX_ALLOW_STALE=1" >&2
    $warn || exit 1
elif ((unreachable > 0)); then
    echo "all reachable sources are current ($unreachable could not be checked)"
else
    echo "all sources are current"
fi
