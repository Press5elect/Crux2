#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - the PC tests of the console's features: the interface's sound, the cover matching and the
# cheat lists, the console network module and the hard drive image, the disc drive's watcher, the
# memory cards, FSR 1, frame generation, frame pacing, the updater, the phone's page, the input
# features, FSR 3's optical flow and NIS, and the looks.
# With the address and undefined-behaviour sanitizers.
#
#   ps5/tests/run-feature-tests.sh <work folder> [a start-up sound of one's own.wav]
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ps5=$(cd "$here/.." && pwd)
root=$(cd "$ps5/.." && pwd)
work=${1:?usage: run-feature-tests.sh <work folder> [startup.wav]}
mkdir -p "$work"/{uiaudio,bootstep,pgs,online,net,disc,cards,fsr,openup,framegen,pacing,update,web,computefx,slang,input,clock}
flags=(-std=c++20 -g -O1 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -I "$ps5/frontend")
export ASAN_OPTIONS=detect_leaks=0

echo "== the interface's sound"
bash "$here/uiaudio/run-uiaudio-tests.sh" "$work/uiaudio" ${2:+"$2"} 2>&1 | grep -v '^interface sound:'

echo "== covers and cheats"
clang++ "${flags[@]}" -O2 "$here/online/covers_match_test.cpp" -o "$work/online/covers_match_test"
"$work/online/covers_match_test" "$ps5/package/resources/ps5/boxarts.txt" "$root/vendor/pcsx2/bin/resources/GameIndex.yaml"
clang++ "${flags[@]}" "$here/online/cheats_logic_test.cpp" -o "$work/online/cheats_logic_test"
"$work/online/cheats_logic_test" "$ps5/package/resources/ps5/cheat-index.txt"
clang++ "${flags[@]}" -I "$root/vendor/pcsx2/3rdparty/rapidjson/include" "$here/online/about_logic_test.cpp" -o "$work/online/about_logic_test"
"$work/online/about_logic_test"

echo "== the network module and the hard drive image"
clang++ "${flags[@]}" "$here/net/net_test.cpp" "$ps5/frontend/ps5_net.cpp" -lpthread -o "$work/net/net_test"
"$work/net/net_test" "$work/net"

echo "== disc images: ISO, CHD and CSO, read back with libchdr (needs the zstd and zlib headers)"
bash "$here/discimage/run-discimage-test.sh" "$work/discimage"

echo "== the disc drive's watcher"
clang++ "${flags[@]}" "$here/disc/disc_test.cpp" "$ps5/frontend/ps5_disc.cpp" "$ps5/frontend/ps5_disc_image.cpp" -I "$root/vendor/pcsx2/3rdparty/lzma/include" \
	"$work"/discimage/obj/Lz*.o "$work"/discimage/obj/CpuArch.o "$work"/discimage/obj/Alloc.o -lz -lzstd -lpthread -o "$work/disc/disc_test"
"$work/disc/disc_test" "$work/disc"

echo "== a disc held in memory as it is read"
clang++ "${flags[@]}" "$here/disc/disc_cache_test.cpp" -lpthread -o "$work/disc/disc_cache_test"
ASAN_OPTIONS=detect_leaks=1 "$work/disc/disc_cache_test"

echo "== a card for each game, texture packs' places, the state saved by itself"
clang++ "${flags[@]}" "$here/extras/extras_logic_test.cpp" -o "$work/disc/extras_logic_test"
ASAN_OPTIONS=detect_leaks=1 "$work/disc/extras_logic_test"

echo "== memory cards and save files"
bash "$here/cards/run-cards-tests.sh" "$work/cards"
echo "== FSR 1's two passes, on a software Vulkan driver"
bash "$here/fsr/run-fsr-tests.sh" "$work/fsr"
echo "== opening up the title's files for an FTP client"
clang++ "${flags[@]}" "$here/openup/openup_test.cpp" -lpthread -o "$work/openup/openup_test"
ASAN_OPTIONS=detect_leaks=1 "$work/openup/openup_test" "$work/openup"
echo "== frame generation: which picture is due at each refresh, and the passes on a software Vulkan driver"
clang++ "${flags[@]}" "$here/framegen/pacer_test.cpp" -o "$work/framegen/pacer_test"
"$work/framegen/pacer_test"
bash "$here/framegen/run-framegen-tests.sh" "$work/framegen"
echo "== frame pacing: the plans, the watch over the screen's pacing, frame times, how late sleeps wake"
clang++ "${flags[@]}" -I "$ps5/common" "$here/pacing/pacing_test.cpp" -o "$work/pacing/pacing_test"
"$work/pacing/pacing_test"
echo "== paraLLEl-GS: the GIF packets handed on from VU1 memory"
clang++ "${flags[@]}" "$here/pgs/pgs_packet_test.cpp" -o "$work/pgs/pgs_packet_test"
"$work/pgs/pgs_packet_test"
echo "== a slow start: the step named on the loading screen"
clang++ "${flags[@]}" "$here/bootstep/bootstep_test.cpp" -o "$work/bootstep/bootstep_test"
"$work/bootstep/bootstep_test"
echo "== the updater"
bash "$here/update/run-update-tests.sh" "$work/update"
echo "== the phone's page"
bash "$here/web/run-web-tests.sh" "$work/web"
echo "== the keyboard, the leave-game combination, motion aiming, pressure, USB pads' reports"
bash "$here/input/run-input-tests.sh" "$work/input"
echo "== the clock from the processor's counter"
bash "$here/clock/run-clock-tests.sh" "$work/clock" | grep -E '^  counter|threads|all passed'
echo "== AMD FSR 3's optical flow and NVIDIA's NIS, on a software Vulkan driver"
bash "$here/computefx/run-computefx-tests.sh" "$work/computefx"
echo "== looks: libretro presets, their reflection and their chains on a software Vulkan driver"
bash "$here/slang/run-slang-tests.sh" "$work/slang"
echo "== native modules: PS2Recomp's Crux2 mode, the module builder, the loader, a run against stand-in memory"
bash "$here/../recomp/tests/run-native-tests.sh" "$work/native" | grep -E '^  native|^native:|skipped'
echo "feature tests passed"
