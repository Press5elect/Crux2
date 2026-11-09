#!/usr/bin/env bash
# PS5 Vulkan Template - the payload SDK this title builds with, at a pinned revision.
#
#   ps5/tools/setup-sdk.sh       install it into .deps/native/ps5-payload-sdk (once a revision)
#
# The SDK is my fork, ../PS5_PayloadSDK: the upstream release with the PS5
# platform layer (libps5platform.a, include/ps5platform) installed over it. The
# pinned revision is exported with git archive, so a build never depends on the
# fork's working tree, and the SDK directory records it, as PS5 RetroArch and
# PS5 vkQuake pin theirs. RADV's archive, its link recipe and the packaging
# tools stay PS5_Vulkan's.
#
# Copyright (C) 2026 Mihawk
# SPDX-FileCopyrightText: 2026 Press5elect (the changes for Crux2)
# SPDX-License-Identifier: MIT
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cache="$root/.deps/native"
sdk="$cache/ps5-payload-sdk"
sdk_fork="${PS5_PAYLOAD_SDK_FORK:-$root/../PS5_PayloadSDK}"
# b5efad5: the newest of the fork (2026-10-04): iswctype_l answers from the rune type (a crash in
# std::locale on the console), files opened past the per-process limit, contended heap waits that
# block, and the /data client; 6b63a2a before it gave localeconv in the C locale (ps5_localeconv).
sdk_revision=b5efad528e0ac1b8289f39d72a0e890ff68ff0a6

if [[ ! -f $sdk/.ps5-sdk-revision || $(<"$sdk/.ps5-sdk-revision") != "$sdk_revision" ]]; then
    git -C "$sdk_fork" cat-file -e "$sdk_revision^{commit}" 2>/dev/null || {
        echo "the payload SDK fork at $sdk_fork does not have $sdk_revision" >&2
        exit 2
    }
    mkdir -p "$cache"
    sdk_tree=$(mktemp -d)
    git -C "$sdk_fork" archive "$sdk_revision" | tar -x -C "$sdk_tree"
    bash "$sdk_tree/platform/tools/setup-sdk.sh" "$sdk" "$sdk_revision" "$cache" >&2
    rm -rf -- "$sdk_tree"
fi
[[ -x $sdk/bin/prospero-lld && -d $sdk/target/include ]] || { echo "the pinned PS5 payload SDK is incomplete" >&2; exit 2; }
echo "$sdk"
