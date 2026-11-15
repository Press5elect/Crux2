#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - the settings inventory, built for the PC and run (inventory.cpp says what it is).
#
#   ps5/tests/bigpicture/inventory/build-inventory.sh <work folder>
#
# Writes <work folder>/dump.json: every row of the settings as the console's own code makes them.
# Needs vendor/pcsx2 (ps5/tools/fetch-pcsx2.sh), clang++ and readelf; not SDL, libzip or the SDK.
# The interface's files and a few of PCSX2's are compiled as they are; bp_pcsx2.cpp is compiled
# inside inventory.cpp; everything else it names is a stub (make-stubs.py) that stops the program
# with the symbol's name if a page calls it.
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=$(cd "$here/../../../.." && pwd)
work=${1:?usage: build-inventory.sh <work folder>}
mkdir -p "$work/obj" "$work/gen"
vendor="$root/vendor/pcsx2"
front="$root/ps5/frontend/bigpicture"
imgui="$vendor/3rdparty/imgui"
[[ -f $vendor/pcsx2/Host.cpp ]] || { echo "inventory: vendor/pcsx2 is missing (ps5/tools/fetch-pcsx2.sh)"; exit 1; }

# PCSX2's files the pages need for real: the settings layers, the controller types and their
# settings, the hotkeys (PCSX2's and the graphics'), the configuration's defaults.
pcsx2_files=(pcsx2/Host.cpp pcsx2/LayeredSettingsInterface.cpp pcsx2/INISettingsInterface.cpp pcsx2/Pcsx2Config.cpp pcsx2/Hotkeys.cpp
    pcsx2/GS/GS.cpp pcsx2/SIO/Sio.cpp pcsx2/SIO/Pad/Pad.cpp pcsx2/SIO/Pad/PadBase.cpp pcsx2/SIO/Pad/PadDualshock1.cpp
    pcsx2/SIO/Pad/PadDualshock2.cpp pcsx2/SIO/Pad/PadGuitar.cpp pcsx2/SIO/Pad/PadJogcon.cpp pcsx2/SIO/Pad/PadNegcon.cpp
    pcsx2/SIO/Pad/PadNotConnected.cpp pcsx2/SIO/Pad/PadPopn.cpp common/MemorySettingsInterface.cpp common/StringUtil.cpp
    common/SmallString.cpp common/Timer.cpp common/Error.cpp common/FileSystem.cpp common/Console.cpp common/Assertions.cpp
    3rdparty/fmt/src/format.cc)

# The build's revision header, as CMake would write it.
[[ -f $work/gen/svnrev.h ]] || printf '#define GIT_TAG ""\n#define GIT_TAGGED_COMMIT 0\n#define GIT_TAG_HI 0\n#define GIT_TAG_MID 0\n#define GIT_TAG_LO 0\n#define GIT_REV "inventory"\n#define GIT_HASH "inventory"\n#define GIT_DATE ""\n' > "$work/gen/svnrev.h"

includes=(-I"$work/gen" -I"$vendor" -I"$vendor/pcsx2" -I"$vendor/common/include" -I"$vendor/3rdparty/include" -I"$vendor/3rdparty/fmt/include"
    -I"$vendor/3rdparty/simpleini/include" -I"$vendor/3rdparty/fast_float/include" -I"$vendor/3rdparty/xbyak" -I"$vendor/3rdparty/vulkan/include"
    -I"$vendor/3rdparty/imgui/include" -I"$vendor/3rdparty/rcheevos/include" -I"$vendor/3rdparty/promptfont" -I"$vendor/3rdparty/libchdr/include"
    -I"$vendor/3rdparty/cpuinfo/include" -I"$vendor/3rdparty/ccc/src" -I"$vendor/3rdparty/rapidjson/include" -I"$vendor/3rdparty/libzip/lib"
    -I"$root/ps5/frontend" -I"$root/ps5/frontend/vfs" -I"$root/ps5/elevation" -I"$root/ps5/src" -I"$front")
flags=(-std=c++20 -g -O0 -w -msse4.1 -mavx2 -mbmi2 -mfma -D_M_X86=1 -DPCSX2_CORE -DENABLE_VULKAN -DST_NO_EXCEPTION_HANDLING)

objects=()
pids=()
compile() { # compile SOURCE FLAGS...
    local source=$1
    shift
    local object="$work/obj/$(echo "${source#"$root"/}" | tr '/' '_').o"
    objects+=("$object")
    if [[ ! -f $object || $source -nt $object || $front/bp.h -nt $object || $front/bp_internal.h -nt $object || $front/bp_pcsx2.cpp -nt $object ||
        $here/host_fakes.cpp -nt $object ]]; then
        clang++ "$@" -c "$source" -o "$object" &
        pids+=($!)
    fi
}
compile "$here/inventory.cpp" "${flags[@]}" "${includes[@]}"
compile "$here/host_fakes.cpp" "${flags[@]}" "${includes[@]}"
for file in "${pcsx2_files[@]}"; do
    # (Pad.cpp names SDL's input source: the shim stands in for its header.)
    compile "$vendor/$file" "${flags[@]}" -I"$here/shim" "${includes[@]}"
done
for source in "$front"/bp_*.cpp; do
    [[ $source == */bp_pcsx2.cpp ]] && continue
    compile "$source" -std=c++20 -g -O0 -w -I"$front" -I"$root/ps5/frontend" -I"$imgui/include" -I"$vendor/3rdparty/include"
done
for file in imgui imgui_draw imgui_tables imgui_widgets; do
    compile "$imgui/src/$file.cpp" -std=c++20 -O1 -w -I"$imgui/include"
done
for pid in "${pids[@]}"; do
    wait "$pid" || { echo "inventory: a file did not compile"; exit 1; }
done

# Linked; what is not found becomes a stub, and it is linked again.
rm -f "$work/stubs.S"
touch "$work/stubs.S"
linked=0
for attempt in 1 2 3; do
    if clang++ -g "${objects[@]}" "$work/stubs.S" -lpthread -Wl,--no-demangle -o "$work/inventory" 2> "$work/link.log"; then
        linked=1
        break
    fi
    python3 "$here/make-stubs.py" "$work/link.log" "$work/stubs.S" "${objects[@]}" > /dev/null || { tail -20 "$work/link.log"; exit 1; }
done
[[ $linked == 1 ]] || { tail -20 "$work/link.log"; exit 1; }
"$work/inventory" "$work/dump.json"
echo "inventory: $(grep -c '"state"' "$work/dump.json") rows written to $work/dump.json ($(grep -c '\.globl' "$work/stubs.S") symbols stubbed)"
