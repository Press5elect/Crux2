#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - the tests that run on the PC.
#
#   ps5/tests/run-host-tests.sh
#
# The parts of the port that have no console in them are built for the PC, with the
# undefined-behaviour sanitizer (and the address sanitizer where the test maps no memory of its
# own), and run:
#   - the libc stand-ins of frontend/ps5_libc.c, each against the PC's own libc;
#   - the quick menu's entries and their order (frontend/bigpicture/bp_quick_layout.h), and frame
#     generation's figures for the on-screen display and the log (frontend/ps5_pacing.h);
#   - the performance overlay's levels (white, amber, red), its sizes' presets of the switches and
#     what the settings keep after a switch changes (frontend/ps5_osd_logic.h);
#   - the game list's letter jumps (frontend/ps5_letter_jump.h): the order, the groups, and where
#     L2 and R2 land on sorted and unsorted lists;
#   - the profiler's folding (frontend/ps5_profile_fold.h): symbols.txt read and refused, the
#     function an address is in, and the counts of a hand-made and of random sets of samples;
#   - the profiler itself (frontend/ps5_profiler.cpp): threads whose work is known are sampled
#     and written up by name, a thread's sleeps go on after the signals, and threads start and
#     end under the sampler; plain, and with the address and thread sanitizers;
#   - the threads' self-reported processor times (common/PS5ThreadTimes.h), with the thread
#     sanitizer: readings from another thread never go back;
#   - a quick tap held for the game at least 50 ms (frontend/ps5_input_source_logic.h, TapHold);
#   - the GunCon 2's steady motion aim (frontend/ps5_aim_logic.h) over a made-up controller, the
#     sticks' response curve and R2's click (ps5_input_source_logic.h);
#   - USB pads through /dev/ugen (frontend/ps5_ugen.h): the transfers over a made-up device node;
#   - the write broker (ps5/broker, frontend/ps5_broker.cpp): copies and written files byte for byte
#     through the payload's request handler, and whole when it dies part-way;
#   - the updater (frontend/ps5_update.cpp, tests/update): a check, a download and an install
#     over a made-up GitHub, developer builds, the files a new version dropped;
#   - HD texture pack downloads (frontend/ps5_texture_packs_pieces.h and _fetch.h): pieces that
#     finish out of order handed over in order, the memory they may hold, failures, slow parts and
#     a stop; and, with the PC's libcurl and libarchive, the download over 10 connections from a
#     made-up server with ranges, without them, cutting parts off, and a .zip unpacked as it comes;
#   - the texture pack manager's file work (frontend/ps5_texture_pack_store.cpp): packs found,
#     measured, moved (stopped half-way, failing, begun again: the source kept), deleted, queued;
#   - the memory card held in memory (frontend/ps5_card_image.h): random saves on the image and
#     on a copy, the file compared with the copy after each flush;
#   - the memory code of common/PS5HostSys.cpp (guest memory, the code area, the fastmem window,
#     page protection and the fault handler), over a stand-in for the platform layer with the same
#     argument rules (mock_platform.cpp), compiled for 16 KiB host pages as the console build is;
#   - downsampling and SMAA (frontend/ps5_aa.h, tests/aa): when they run and at what size; their passes
#     on a software Vulkan driver against the maths in double precision;
#   - the interface's and host's sources: no setting read through Host while the settings' lock
#     is held (check-settings-lock.py; such a read ends the title);
#   - the settings' no-loss test (bigpicture/inventory/run-inventory-test.sh): every row of the
#     settings made by the console's own code (bp_options.cpp, bp_pcsx2.cpp) on the PC, against the
#     baseline settings-inventory.json (no option, layout or skin gone or changed) and against
#     settings-text.yaml (every row has its words there, no stale one);
#   - the virtual file layer of frontend/vfs (run-vfs-tests.sh: needs a libsmb2 for the PC, and
#     reads from a real Samba and a real FTP server when they are installed).
#
# What they cannot show is the console's own behaviour: which addresses its kernel grants, what
# its pad and audio services answer, what the graphics driver does. That is a console run.
#
#   VFS_TEST_DIR         where they work (default: a new temporary directory)
#   LIBSMB2_PREFIX       a libsmb2 built for the PC; without it the virtual file layer is skipped
#   PS5_PAYLOAD_SDK_FORK the SDK fork's checkout, for the platform layer's headers
#                        (default ../PS5_PayloadSDK)
#   CC, CXX              the compilers (default cc and c++; the host memory test is clang's)

set -euo pipefail

tests=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ps5=$(dirname "$tests")
root=$(dirname "$ps5")
frontend="$ps5/frontend"
pcsx2="$root/vendor/pcsx2"
sdk_fork="${PS5_PAYLOAD_SDK_FORK:-$root/../PS5_PayloadSDK}"
work=${VFS_TEST_DIR:-$(mktemp -d)}
mkdir -p "$work"
status=0
sanitize="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
# The compilers: CC and CXX when given (the CI runs the tests with gcc and with clang), and every
# warning of the tests' own code an error.
CC=${CC:-cc}
CXX=${CXX:-c++}
export CC CXX
cc() { command "$CC" -Werror "$@"; }
c++() { command "$CXX" -Werror "$@"; }
echo "== compilers: $("$CC" --version | head -n 1); $("$CXX" --version | head -n 1)"

echo "== libc stand-ins"
# shellcheck disable=SC2086
cc -std=gnu11 -O1 -g -Wall -Wextra $sanitize "$tests/libc_test.c" "$frontend/ps5_libc.c" -o "$work/libc_test"
"$work/libc_test" || status=1

echo "== memory card image"
# shellcheck disable=SC2086
c++ -std=c++20 -O1 -g -Wall -Wextra $sanitize -I "$frontend" "$tests/card_image_test.cpp" -o "$work/card_image_test"
"$work/card_image_test" "$work" || status=1

echo "== letter jumps"
# shellcheck disable=SC2086
c++ -std=c++20 -O1 -g -Wall -Wextra $sanitize -I "$frontend" "$tests/letter_jump_test.cpp" -o "$work/letter_jump_test"
"$work/letter_jump_test" || status=1

echo "== the quick menu's entries and their order (frontend/bigpicture/bp_quick_layout.h)"
# shellcheck disable=SC2086
c++ -std=c++20 -O1 -g -Wall -Wextra $sanitize -I "$frontend/bigpicture" "$tests/bigpicture/quick_layout_test.cpp" -o "$work/quick_layout_test"
"$work/quick_layout_test" || status=1

echo "== frame generation's figures (frontend/ps5_pacing.h, FrameGenFigures)"
# shellcheck disable=SC2086
c++ -std=c++20 -O1 -g -Wall -Wextra $sanitize -I "$frontend" "$tests/pacing/framegen_figures_test.cpp" -o "$work/framegen_figures_test"
"$work/framegen_figures_test" || status=1

echo "== frame generation's Steady 30 (frontend/ps5_framegen_pacer.h)"
# shellcheck disable=SC2086
c++ -std=c++20 -O1 -g -Wall -Wextra $sanitize -I "$frontend" "$tests/pacing/steady30_test.cpp" -o "$work/steady30_test"
"$work/steady30_test" || status=1

echo "== the performance overlay's levels and sizes (frontend/ps5_osd_logic.h)"
# shellcheck disable=SC2086
c++ -std=c++20 -O1 -g -Wall -Wextra $sanitize -I "$frontend" "$tests/osd/osd_logic_test.cpp" -o "$work/osd_logic_test"
"$work/osd_logic_test" || status=1

echo "== a picture's size from its header, huge ones refused before decoding (frontend/ps5_image_header.h)"
mkdir -p "$work/image"
# shellcheck disable=SC2086
c++ -std=c++20 -O1 -g -Wall -Wextra $sanitize -I "$frontend" "$tests/image/image_header_test.cpp" -o "$work/image_header_test"
"$work/image_header_test" "$work/image" || status=1

echo "== PlayStation Move light gun (Move Management's loopback stream, a stand-in producer)"
# shellcheck disable=SC2086
c++ -std=c++20 -O1 -g -Wall -Wextra $sanitize -pthread -I "$frontend" "$tests/move/move_test.cpp" -o "$work/move_test"
"$work/move_test" || status=1

echo "== the GunCon 2's steady motion aim, the sticks' curve, R2's click (a made-up controller turned)"
# shellcheck disable=SC2086
c++ -std=c++20 -O1 -g -Wall -Wextra $sanitize -I "$frontend" "$tests/aim/aim_test.cpp" -o "$work/aim_test"
"$work/aim_test" || status=1

echo "== USB pads through ugen nodes (a made-up FreeBSD USB stack)"
# shellcheck disable=SC2086
c++ -std=c++20 -O1 -g -Wall -Wextra $sanitize -pthread -I "$frontend" "$tests/usbpad/ugen_test.cpp" "$frontend/ps5_usbpad.cpp" \
	-o "$work/ugen_test"
"$work/ugen_test" 2> >(grep -v '^usb pads:' >&2) || status=1

echo "== profile folding"
# shellcheck disable=SC2086
c++ -std=c++20 -O1 -g -Wall -Wextra $sanitize -I "$frontend" "$tests/profile_fold_test.cpp" -o "$work/profile_fold_test"
"$work/profile_fold_test" > "$work/profile_fold_test.log" || { cat "$work/profile_fold_test.log"; status=1; }
tail -1 "$work/profile_fold_test.log"

echo "== profiler"
# The profiler's own source, on threads whose work is known: plain (the figures are checked),
# with the address sanitizer, and with the thread sanitizer (no figures: it delivers signals
# late). The names it prints are this test program's, from its own symbols.txt.
profiler_build() { # profiler_build NAME FLAGS...
    local name=$1
    shift
    mkdir -p "$work/profiler-$name"
    c++ -std=c++20 -O1 -g -Wall -Wextra -fno-omit-frame-pointer -pthread -DPS5PX_PROFILE_WINDOW_MS=500 "$@" \
        -I "$frontend" "$tests/profiler_test.cpp" "$frontend/ps5_profiler.cpp" -o "$work/profiler-$name/profiler_test"
    python3 - "$ps5/tools/make-symbols.py" "$work/profiler-$name/profiler_test" "$work/profiler-$name/symbols.txt" <<'PY'
# symbols.txt of the test program, in the title's format: the same reading of the ELF's code
# sections as the title's tool (its link map is lld's, which the PC's linker does not write).
import importlib.util, subprocess, sys
from pathlib import Path
spec = importlib.util.spec_from_file_location("make_symbols", sys.argv[1])
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)
end = max(section[1] for section in tool.code_sections(Path(sys.argv[2])).values())
base, functions = None, {}
for line in subprocess.run(["nm", "-n", "--defined-only", sys.argv[2]], check=True, capture_output=True,
                           text=True).stdout.splitlines():
    parts = line.split(" ", 2)
    if len(parts) != 3:
        continue
    address, kind, name = int(parts[0], 16), parts[1], parts[2]
    if name == "__executable_start":
        base = address
    if kind in "tTwW" and address < end:
        functions.setdefault(address, name)
assert base is not None, "no __executable_start in the test program"
framed = tool.frame_test(Path(sys.argv[2]))
with open(sys.argv[3], "w") as out:
    for address in sorted(a for a in functions if a > base):
        out.write(f"{address - base:x} {'E' if framed(address) else 'e'} {functions[address]}\n")
    out.write(f"{end - base:x} - end\n")
PY
}
profiler_build plain
"$work/profiler-plain/profiler_test" "$work/profiler-plain" > "$work/profiler-plain.log" 2>&1 ||
    { cat "$work/profiler-plain.log"; status=1; }
tail -3 "$work/profiler-plain.log"
profiler_build asan -fsanitize=address,undefined -fno-sanitize-recover=undefined
ASAN_OPTIONS=detect_stack_use_after_return=0 "$work/profiler-asan/profiler_test" "$work/profiler-asan" > "$work/profiler-asan.log" 2>&1 ||
    { tail -40 "$work/profiler-asan.log"; status=1; }
tail -1 "$work/profiler-asan.log"
profiler_build tsan -fsanitize=thread
"$work/profiler-tsan/profiler_test" "$work/profiler-tsan" relaxed > "$work/profiler-tsan.log" 2>&1 ||
    { tail -40 "$work/profiler-tsan.log"; status=1; }
tail -1 "$work/profiler-tsan.log"

echo "== thread times"
c++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=thread -pthread -I "$ps5/common" "$tests/thread_times_test.cpp" -o "$work/thread_times_test"
"$work/thread_times_test" || status=1

echo "== quick taps held for the game"
# shellcheck disable=SC2086
c++ -std=c++20 -O1 -g -Wall -Wextra $sanitize -I "$frontend" "$tests/input/tap_hold_test.cpp" -o "$work/tap_hold_test"
"$work/tap_hold_test" || status=1
c++ -std=c++20 -O1 -g -Wall -Wextra $sanitize -I "$frontend" "$tests/input/touch_click_test.cpp" -o "$work/touch_click_test"
"$work/touch_click_test" || status=1

echo "== the updater (developer builds, the files a new version dropped)"
# libzip: the PC's (pkg-config) or PCSX2's copy built once; rapidjson: the PC's or PCSX2's.
if [[ -f $pcsx2/3rdparty/rapidjson/include/rapidjson/document.h || -f /usr/include/rapidjson/document.h ]] &&
    { pkg-config --exists libzip 2>/dev/null || [[ -d $pcsx2/3rdparty/libzip ]]; }; then
    bash "$tests/update/run-update-tests.sh" "$work/update" || status=1
else
    echo "skipped (needs libzip and rapidjson: the PC's, or vendor/pcsx2's)"
fi

echo "== host memory"
if [[ -f $pcsx2/common/HostSys.h && -f $sdk_fork/platform/include/ps5platform/shm.h ]]; then
    clang++ -std=c++20 -O1 -g -Wall -Wno-unknown-pragmas -Wno-unused-parameter -fsanitize=undefined \
        -fno-sanitize-recover=undefined -DOVERRIDE_HOST_PAGE_SIZE=0x4000 -DOVERRIDE_HOST_CACHE_LINE_SIZE=64 \
        -D_M_X86=1 -D__POSIX__ -msse4.1 \
        -I "$tests/mock" -I "$sdk_fork/platform/include" -I "$pcsx2" -I "$pcsx2/3rdparty/fmt/include" \
        -I "$pcsx2/3rdparty/include" \
        "$tests/hostsys_test.cpp" "$tests/mock_platform.cpp" "$ps5/common/PS5HostSys.cpp" -o "$work/hostsys_test"
    "$work/hostsys_test" || status=1
else
    echo "skipped (needs vendor/pcsx2 and the SDK fork at $sdk_fork)"
fi

if [[ -n ${LIBSMB2_PREFIX:-} ]]; then
    VFS_TEST_DIR="$work" bash "$tests/run-vfs-tests.sh" || status=1
else
    echo "== virtual file layer: skipped (set LIBSMB2_PREFIX to a libsmb2 built for the PC)"
fi

echo "== virtual file layer alone: a read held (the share that stops answering), the shares woken"
# (No network backend: they are stubbed in the test.)
# shellcheck disable=SC2086
c++ -std=c++20 -O1 -g -Wall -Wextra $sanitize -I "$frontend" "$tests/vfs_stall_test.cpp" "$frontend/vfs/ps5_vfs.cpp" -lpthread \
    -o "$work/vfs_stall_test"
"$work/vfs_stall_test" || status=1

echo "== network shares set up on the console, the sockets helper, and the name server test"
# frontend/ps5_shares.cpp without the layer (PS5_SHARES_HOST_TEST); vfs/ps5_socket.h's non-blocking
# sockets and port check on 127.0.0.1; ps5_net.cpp's name server test and its public DNS for a name
# the console's name server answers 0.0.0.0, against a made-up name server (tests/net).
mkdir -p "$work/net-shares"
# shellcheck disable=SC2086
c++ -std=c++20 -O1 -g -Wall -Wextra $sanitize -DPS5_SHARES_HOST_TEST -I "$frontend" "$tests/net/shares_test.cpp" \
    "$frontend/ps5_shares.cpp" -o "$work/shares_test"
"$work/shares_test" "$work/net-shares" || status=1
# shellcheck disable=SC2086
c++ -std=c++20 -O1 -g -Wall -Wextra $sanitize -I "$frontend" "$tests/net/dns_test.cpp" "$frontend/ps5_net.cpp" -lpthread \
    -o "$work/dns_test"
"$work/dns_test" > "$work/dns_test.log" || { cat "$work/dns_test.log"; status=1; }
tail -1 "$work/dns_test.log"

echo "== the boot log kept in memory"
# shellcheck disable=SC2086
c++ -std=c++20 -O1 -g -Wall -Wextra $sanitize -I "$frontend" "$tests/logs/memory_log_test.cpp" "$frontend/ps5_logs.cpp" \
    "$frontend/ps5_backup.cpp" "$frontend/vfs/ps5_vfs.cpp" -lpthread -o "$work/memory_log_test"
"$work/memory_log_test" "$work" || status=1

echo "== backups that meet a folder they cannot read (frontend/ps5_backup.cpp)"
# Read errors made at the link (opendir, readdir and stat wrapped): the backup fails and the one
# before it is kept (tests/backup/backup_errors_test.cpp).
# shellcheck disable=SC2086
c++ -std=c++20 -O1 -g -Wall -Wextra $sanitize -I "$frontend" "$tests/backup/backup_errors_test.cpp" "$frontend/ps5_backup.cpp" \
    "$frontend/vfs/ps5_vfs.cpp" -lpthread -Wl,--wrap=opendir,--wrap=readdir,--wrap=stat -o "$work/backup_errors_test"
"$work/backup_errors_test" "$work/backup-errors" || status=1

echo "== the elevation helper asked again for a few seconds (ps5/elevation/elevation.cpp)"
# shellcheck disable=SC2086
c++ -std=c++20 -O1 -g -Wall -Wextra $sanitize -I "$ps5/elevation" "$tests/elevation/elevation_retry_test.cpp" \
    "$ps5/elevation/elevation.cpp" -o "$work/elevation_retry_test"
"$work/elevation_retry_test" || status=1

echo "== the write broker (ps5/broker) and its client (frontend/ps5_broker.cpp) over a socketpair"
# shellcheck disable=SC2086
c++ -std=c++20 -O1 -g -Wall -Wextra -Werror $sanitize -pthread -I "$frontend" "$tests/broker/broker_test.cpp" \
    "$frontend/ps5_broker.cpp" "$ps5/broker/server.cpp" -o "$work/broker_test"
rm -rf "$work/broker"
"$work/broker_test" "$work/broker" 2> "$work/broker_test.log" && tail -n 1 "$work/broker_test.log" || { cat "$work/broker_test.log"; status=1; }
rm -rf "$work/broker"

echo "== launch arguments (frontend/ps5_launch.h: --rom, --exit-after-game)"
# shellcheck disable=SC2086
c++ -std=c++20 -O1 -g -Wall -Wextra $sanitize -I "$frontend" "$tests/launch/launch_test.cpp" -o "$work/launch_test"
"$work/launch_test" || status=1

echo "== the console's trophy notifications (frontend/ps5_notify.cpp, a stand-in payload loader)"
mkdir -p "$work/notify"
# shellcheck disable=SC2086
c++ -std=c++20 -O1 -g -Wall -Wextra $sanitize -pthread -DCRUX2_NOTIFY_TEST -I "$frontend" "$tests/notify/notify_test.cpp" \
    "$frontend/ps5_notify.cpp" -o "$work/notify_test"
"$work/notify_test" "$work/notify" 2> "$work/notify_test.log" || { cat "$work/notify_test.log"; status=1; }

echo "== recommended settings per game (frontend/ps5_recommend.cpp, ps5/recommended.ini)"
# shellcheck disable=SC2086
c++ -std=c++20 -O1 -g -Wall -Wextra $sanitize -DCRUX2_FIXES_TEST -DCRUX2_RECOMMEND_TEST -I "$frontend" \
    "$tests/recommend/recommend_test.cpp" "$frontend/ps5_recommend.cpp" "$frontend/ps5_fixes_fetch.cpp" -o "$work/recommend_test"
"$work/recommend_test" "$work/recommend" "$ps5/recommended.ini" 2> "$work/recommend_test.log" ||
    { cat "$work/recommend_test.log"; status=1; }

echo "== HD texture pack downloads: pieces over several connections, in order (frontend/ps5_texture_packs_pieces.h, _fetch.h)"
# The pieces' scheduling always; the download itself over curl against a made-up server on
# 127.0.0.1 when the PC has libcurl and libarchive (tests/texturepacks/run-fetch-tests.sh).
bash "$tests/texturepacks/run-fetch-tests.sh" "$work/texturepack-fetch" || status=1

echo "== the texture pack manager's file work (frontend/ps5_texture_pack_store.cpp)"
# Packs found on made-up places, measured, moved to a drive and to a share that cannot rename,
# moves stopped half-way and failing (the source as it was) and begun again, deletes, the queue.
# shellcheck disable=SC2086
c++ -std=c++20 -O1 -g -Wall -Wextra -Werror $sanitize -I "$frontend" "$tests/texturepacks/store_test.cpp" \
    "$frontend/ps5_texture_pack_store.cpp" -o "$work/texture_pack_store_test"
"$work/texture_pack_store_test" "$work/texture-pack-store" || status=1

echo "== Crux2's own screenshots (frontend/ps5_shots.h)"
# shellcheck disable=SC2086
c++ -std=c++20 -O1 -g -Wall -Wextra $sanitize -I "$frontend" "$tests/shots/shots_test.cpp" -o "$work/shots_test"
"$work/shots_test" || status=1

echo "== game fixes from GitHub"
# The list fetched, kept, refused, withdrawn and not reached (frontend/ps5_fixes_fetch.cpp), over a
# stand-in for the network; and ps5/game-fixes.txt current.
bash "$tests/fixes/run-fixes-tests.sh" "$work/fixes" || status=1

echo "== HDR output and debanding (the last passes before the screen)"
# ps5_final.glsl compiled as the device does and run on lavapipe against the maths in double precision,
# and ps5_hdr.h's choices (run-hdr-tests.sh: says what is missing when glslang or a driver is not there).
bash "$tests/hdr/run-hdr-tests.sh" "$work/hdr" || status=1

echo "== downsampling and SMAA (before the picture is sharpened and shown)"
# ps5_aa.h's decisions under the sanitizers; ps5_aa.glsl's five passes compiled as the device does and run on
# lavapipe against the maths in double precision, SMAA on staircases (run-aa-tests.sh: says what is missing).
bash "$tests/aa/run-aa-tests.sh" "$work/aa" || status=1

echo "== no setting read through Host while the settings' lock is held"
python3 "$tests/check-settings-lock.py" || status=1

echo "== the settings: nothing lost (the inventory against its baseline, settings-text.yaml complete)"
# clang++ whatever CXX is (PCSX2's headers, as the interface's tests build them); needs vendor/pcsx2.
if command -v clang++ > /dev/null && [[ -f $pcsx2/pcsx2/Host.cpp ]]; then
    bash "$tests/bigpicture/inventory/run-inventory-test.sh" "$work/inventory" || status=1
else
    echo "skipped: no clang++ or no vendor/pcsx2 (ps5/tools/fetch-pcsx2.sh)"
fi

[[ $status == 0 ]] && echo "all host tests passed" || echo "host tests FAILED"
exit $status
