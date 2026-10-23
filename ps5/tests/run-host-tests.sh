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
#   - the game list's letter jumps (frontend/ps5_letter_jump.h): the order, the groups, and where
#     L2 and R2 land on sorted and unsorted lists;
#   - the profiler's folding (frontend/ps5_profile_fold.h): symbols.txt read and refused, the
#     function an address is in, and the counts of a hand-made and of random sets of samples;
#   - the profiler itself (frontend/ps5_profiler.cpp): threads whose work is known are sampled
#     and written up by name, a thread's sleeps go on after the signals, and threads start and
#     end under the sampler; plain, and with the address and thread sanitizers;
#   - the threads' self-reported processor times (common/PS5ThreadTimes.h), with the thread
#     sanitizer: readings from another thread never go back;
#   - the memory card held in memory (frontend/ps5_card_image.h): random saves on the image and
#     on a copy, the file compared with the copy after each flush;
#   - the memory code of common/PS5HostSys.cpp (guest memory, the code area, the fastmem window,
#     page protection and the fault handler), over a stand-in for the platform layer with the same
#     argument rules (mock_platform.cpp), compiled for 16 KiB host pages as the console build is;
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

[[ $status == 0 ]] && echo "all host tests passed" || echo "host tests FAILED"
exit $status
