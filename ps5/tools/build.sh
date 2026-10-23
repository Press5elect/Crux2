#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - build the title into dist/<TITLE_ID>/.
#
#   ps5/tools/build.sh             everything: libraries, link, sign, package
#   ps5/tools/build.sh --compile   stop after compiling (no link)
#
# The title is made on the PS5 Vulkan Template's PS5 layer and tools. It builds
# vendor/pcsx2 (upstream at the pinned revision with ps5/patches applied:
# tools/fetch-pcsx2.sh) and the port layer in ps5/common and ps5/frontend with
# the payload SDK's CMake toolchain, then links with RADV and packages the title
# folder (tools/link-title.sh).
#
#   PS5_VULKAN_DIR   the PS5_Vulkan checkout (default ../PS5_Vulkan): the RADV
#                    release archive, the link recipe, the native tool and libc.prx
#   PS5_CLANG        the host clang the compiler wrappers drive (default: clang)
#   PS5PX_VERSION    the version this build is made as ("v0.2.1"), shown on the About page and
#                    under the mark over a running game; none by default
#   PS5PX_PGO        profile-guided optimisation. Unset: "use" when the profile is there (the
#                    standard build), otherwise "off".
#                      use          compiles with the counts merged into PS5PX_PGO_PROFILE
#                                   (default ps5/pgo/crux2.profdata; in build/ps5-pgo-use)
#                      generate     a profiling build (build/ps5-pgo-generate): counts what runs
#                                   and writes the counts to <data>/pgo/ (frontend/ps5_pgo.h)
#                      cs-generate  a second-round profiling build (build/ps5-pgo-cs-generate):
#                                   compiled with the profile, then counts again after inlining, so
#                                   each inlined copy of a function gets counts of its own
#                                   (context-sensitive PGO); its counts are merged with the first
#                                   round's (ps5/tools/merge-profiles.sh)
#                      off          a normal build in build/ps5
#   PS5PX_PGO_SPLIT  with "use", 1 also splits the cold blocks of hot functions away from them,
#                    lays the functions out by cache-directed sort and applies a second round's
#                    counts. Off by default: measured on the console (2026-10-12), the build with
#                    these and both rounds played worse than the first round's profile alone.
#
# Made from PS5 Vulkan Template's ps5/tools/build.sh (Copyright (C) 2026 Mihawk, MIT).

set -euo pipefail

ps5=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
root=$(dirname "$ps5")
vulkan=$(cd -- "${PS5_VULKAN_DIR:-$root/../PS5_Vulkan}" && pwd)
sdk=$(bash "$ps5/tools/setup-sdk.sh")
build="$root/build/ps5"
default_profile="$ps5/pgo/crux2.profdata"
pgo=${PS5PX_PGO:-}
if [[ -z $pgo ]]; then
    if [[ -f ${PS5PX_PGO_PROFILE:-$default_profile} ]]; then pgo=use; else pgo=off; fi
fi
pgo_flags=
pgo_link_flags=
pgo_profile=
pgo_use_flags=" -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date -Wno-backend-plugin"
find_profile() {
    pgo_profile=$(realpath "${PS5PX_PGO_PROFILE:-$default_profile}")
    [[ -f $pgo_profile ]] || { echo "PS5PX_PGO=$pgo: no profile at $pgo_profile" >&2; exit 1; }
}
profile_runtime() {
    # The runtime first: clang names it in every instrumented object, and CMake's checks link.
    bash "$ps5/tools/build-profile-runtime.sh" "$sdk" > /dev/null
    pgo_link_flags="-L$root/.deps/pgo/lib"
}
case $pgo in
    off) build="$root/build/ps5" ;;
    generate)
        build="$root/build/ps5-pgo-generate"
        pgo_flags=" -fprofile-generate -DPS5PX_PGO_GENERATE=1"
        profile_runtime
        ;;
    cs-generate)
        # The first round's profile is used as in "use", and the code is counted once more after
        # inlining (in the link-time optimised core, at link time: tools/link-title.sh).
        build="$root/build/ps5-pgo-cs-generate"
        find_profile
        pgo_flags=" -fprofile-use=$pgo_profile -fcs-profile-generate -DPS5PX_PGO_GENERATE=1$pgo_use_flags"
        profile_runtime
        ;;
    use)
        build="$root/build/ps5-pgo-use"
        find_profile
        pgo_flags=" -fprofile-use=$pgo_profile$pgo_use_flags"
        # -fsplit-machine-functions: the blocks of a hot function the profile never saw run go
        # to a cold part of their own, away from the hot code. In the link-time optimised core
        # this is done by the linker's code generation (tools/link-title.sh).
        if [[ ${PS5PX_PGO_SPLIT:-} == 1 ]]; then
            build="$root/build/ps5-pgo-use-split"
            pgo_flags=" -fprofile-use=$pgo_profile -fsplit-machine-functions$pgo_use_flags"
        fi
        ;;
    *) echo "PS5PX_PGO is use, generate, cs-generate or off, not '$pgo'" >&2; exit 1 ;;
esac
export PS5PX_PGO_PROFILE_PATH=$pgo_profile
export PS5PX_PGO=$pgo
ninja=$(command -v ninja || echo "$HOME/.local/bin/ninja")
export PS5_CLANG=${PS5_CLANG:-$(command -v clang || true)}

[[ -f $root/vendor/pcsx2/CMakeLists.txt ]] || bash "$ps5/tools/fetch-pcsx2.sh"
bash "$ps5/tools/build-deps.sh"

# (Precompiled headers are off: with them, clang 18 for this target rejects va_start
# in a template instantiated from the header, "__va_list_tag *" against "__va_list_tag[1]".
# No Qt interface, tests or GS runner; the one renderer is Vulkan; the whole title is
# compiled for the console's Zen 2, so no second copy of the vector code for other CPUs.
# LTO_PCSX2_CORE is PCSX2's own option: the core, the recompilers and the GS are optimised
# across files at link time, as PCSX2's release builds are.)
# (The flags are given at configure time only: a build folder configured with other flags is
# configured again, so a change of them here reaches every object.)
configure_flags="$pgo_link_flags|$pgo_flags"
if [[ -f $build/build.ninja && $(cat "$build/configure-flags.txt" 2>/dev/null) != "$configure_flags" ]]; then
    rm -f "$build/CMakeCache.txt" "$build/build.ninja"
fi
if [[ ! -f $build/build.ninja ]]; then
    mkdir -p "$build"
    printf '%s\n' "$configure_flags" > "$build/configure-flags.txt"
    cmake -S "$root/vendor/pcsx2" -B "$build" -G Ninja -DCMAKE_MAKE_PROGRAM="$ninja" \
        -DCMAKE_TOOLCHAIN_FILE="$sdk/toolchain/prospero.cmake" \
        -DPS5PX_ROOT="$ps5" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_VERBOSE_MAKEFILE=OFF -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
        -DENABLE_QT_UI=OFF -DENABLE_TESTS=OFF -DENABLE_GSRUNNER=OFF \
        -DUSE_OPENGL=OFF -DUSE_VULKAN=ON -DX11_API=OFF -DWAYLAND_API=OFF -DUSE_BACKTRACE=OFF \
        -DDISABLE_ADVANCE_SIMD=OFF -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON -DLTO_PCSX2_CORE=ON \
        ${pgo_link_flags:+"-DCMAKE_EXE_LINKER_FLAGS=$pgo_link_flags"} \
        "-DCMAKE_C_FLAGS=-ffunction-sections -fdata-sections -fno-omit-frame-pointer$pgo_flags" \
        "-DCMAKE_CXX_FLAGS=-ffunction-sections -fdata-sections -fno-omit-frame-pointer$pgo_flags" \
        > "$build.configure.log" 2>&1 || { cat "$build.configure.log" >&2; exit 1; }
fi

# A changed profile changes no flag, and clang does not name the profile as a dependency: the
# objects are compiled again when the profile is not the one they were compiled with.
if [[ -n $pgo_profile ]]; then
    profile_digest=$(sha256sum "$pgo_profile" | cut -d ' ' -f 1)
    if [[ $(cat "$build/pgo-profile.sha256" 2>/dev/null) != "$profile_digest" ]]; then
        "${ninja:-ninja}" -C "$build" -t clean > /dev/null
        echo "$profile_digest" > "$build/pgo-profile.sha256"
    fi
fi

# What the title folder's resources/ must hold, compiled into the title: a start with files
# missing (a copy that was cut short, a part of the download not unpacked) says which ones.
# (Not the Direct3D and OpenGL shaders, which the title folder leaves out.)
mkdir -p "$build/ps5px-generated"
# (The emulator's own, and what the port adds to them: package/resources.)
{ (cd "$root/vendor/pcsx2/bin/resources" && find . -type f -not -path './shaders/dx11/*' -not -path './shaders/opengl/*' -printf '%P\t%s\n')
  (cd "$ps5/package/resources" && find . -type f -printf '%P\t%s\n'); } | LC_ALL=C sort \
    | awk -F '\t' '{ gsub(/\\/, "\\\\", $1); gsub(/"/, "\\\"", $1); printf "{\"%s\", %s},\n", $1, $2 }' \
    > "$build/ps5px-generated/ps5_resource_manifest.inc.new"
if cmp -s "$build/ps5px-generated/ps5_resource_manifest.inc.new" "$build/ps5px-generated/ps5_resource_manifest.inc"; then
    rm "$build/ps5px-generated/ps5_resource_manifest.inc.new"
else
    mv "$build/ps5px-generated/ps5_resource_manifest.inc.new" "$build/ps5px-generated/ps5_resource_manifest.inc"
fi

# The build's identity: one digest over every input that decides the binary (PCSX2's revision
# and the port's changes to it, the port's own sources and tools, the SDK pin, PS5_Vulkan's
# revision, the RADV archive, the libraries built by build-deps.sh), then the revisions by name.
# (Of the tools, those that build: a change to the run or release tools makes no new binary.)
# The title prints it first thing, so a log says which build wrote it, and a crash is symbolised
# against the ELF of that build and no other (build/ps5/build-identity.txt names it).
# Crux2's RADV: PS5_Vulkan's release build of PS5 Mesa at its pin with the patches in
# ps5/patches/mesa (tools/build-radv-crux2.sh makes it into .deps/native/radv-crux2); without
# it, PS5_Vulkan's own release archive.
# paraLLEl-GS, the second GS renderer: built from Crux2 Recomp's sources into build/ps5-pgs.
export PS5PX_PGS_OBJECT=$(bash "$ps5/tools/build-parallel-gs.sh" "$sdk" "$root/build/ps5-pgs")
crux2_radv="$vulkan/.deps/native/radv-crux2"
if [[ -z ${RADV_ARCHIVE:-} && -f "$crux2_radv/lib/libvulkan_radeon.ps5.a" ]]; then
    export RADV_ARCHIVE="$crux2_radv/lib/libvulkan_radeon.ps5.a"
fi
archive=${RADV_ARCHIVE:-$vulkan/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a}
radv_provenance="$(dirname "$(dirname "$archive")")/PROVENANCE.txt"
pcsx2_revision=$(git -C "$root/vendor/pcsx2" rev-parse HEAD)
sdk_revision=$(cat "$sdk/.ps5-sdk-revision" 2>/dev/null || echo unknown)
vulkan_revision=$(git -C "$vulkan" rev-parse HEAD 2>/dev/null || echo unknown)
mesa_revision=$(sed -n 's/^revision: //p' "$radv_provenance" 2>/dev/null || true)
archive_digest=$(sha256sum "$archive" | cut -d ' ' -f 1)
radv_note=
[[ $archive == "$vulkan/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a" ]] || radv_note=" (not PS5_Vulkan's release archive: $(basename "$(dirname "$(dirname "$archive")")"))"
digest=$( {
    echo "$pcsx2_revision"
    git -C "$root/vendor/pcsx2" diff HEAD | sha256sum
    (cd "$ps5" && { find cmake common elevation frontend package pgs src sce_sys patches -type f -print0
        printf '%s\0' tools/build-parallel-gs.sh tools/build.sh tools/build-deps.sh tools/build-elevator.sh tools/build-profile-runtime.sh tools/link-title.sh tools/make-symbols.py tools/setup-sdk.sh tools/fetch-pcsx2.sh
        } | LC_ALL=C sort -z | xargs -0 sha256sum)
    echo "$sdk_revision $vulkan_revision $archive_digest"
    sha256sum "$PS5PX_PGS_OBJECT" | cut -d ' ' -f 1
    echo "version ${PS5PX_VERSION:-}"
    echo "pgo $pgo ${pgo_profile:+$(sha256sum "$pgo_profile" | cut -d ' ' -f 1)}"
    sha256sum "$root"/.deps/ports/lib/*.a | cut -d ' ' -f 1
    } | sha256sum | cut -c 1-16)
pgo_note=
[[ $pgo == generate ]] && pgo_note="; profiling build"
[[ $pgo == cs-generate ]] && pgo_note="; profiling build, second round on profile ${profile_digest:0:12}"
[[ $pgo == use ]] && pgo_note="; profile ${profile_digest:0:12}"
identity="$digest (pcsx2 ${pcsx2_revision:0:12} + ps5 patch; sdk ${sdk_revision:0:7}; PS5_Vulkan ${vulkan_revision:0:7}; RADV ${archive_digest:0:12} from Mesa ${mesa_revision:0:7}$radv_note$pgo_note)"
echo "$identity" > "$build/build-identity.txt"

# The identity and the time of this build, for the boot log.
{
    printf '#define PS5PX_BUILD_STAMP "%s"\n' "$(date -u '+%Y-%m-%d %H:%M UTC')"
    printf '#define PS5PX_BUILD_IDENTITY "%s"\n' "$identity"
    printf '#define PS5PX_BUILD_VERSION "%s"\n' "${PS5PX_VERSION:-}"
} > "$build/ps5px-generated/ps5_build_stamp.inc"
echo "build identity: $identity"

"$ninja" -C "$build" pcsx2-ps5
[[ ${1:-} == --compile ]] && exit 0
bash "$ps5/tools/link-title.sh" "$build" "$vulkan" "$sdk"
