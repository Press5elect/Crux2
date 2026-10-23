#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - the graphics driver's threaded recording, checked on the PC.
#
#   ps5/tests/driver/run-driver-tests.sh <work directory of build-host-driver.sh>
#
# With the "asan" build: records one frame shaped like PCSX2's (threaded_recording_test.c) with
# threaded recording off and on, has the driver print the commands it recorded (RADV_DEBUG=
# dumpibs), and requires the two texts to be the same, with the address sanitizer watching both
# runs. With the "plain" build, when there is one: prints what recording costs the calling thread
# either way (a measurement of this PC, not of the console).
#
# What it cannot show is the console: its kernel's threads, its GPU. That is a console run with
# a "threaded-recording" line in test-run.txt.
set -euo pipefail

tests=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
work=$(cd -- "$1" && pwd)
out="$work/test"
mkdir -p "$out"
status=0

# The driver's timed waits (ps5/patches/mesa/0004): util/ps5_timed_wait.h from the patched tree,
# with stand-ins for Mesa's c11/threads.h and os_time.h.
radv_src=${RADV_SRC:-$(cd -- "$tests/../../.." && pwd)/../PS5_Vulkan/.deps/work/radv-src}
if [[ -f $radv_src/src/util/ps5_timed_wait.h ]]; then
    mkdir -p "$out/tw/c11" "$out/tw/util"
    echo '#include <threads.h>' > "$out/tw/c11/threads.h"
    printf '%s\n' '#include <stdint.h>' '#include <time.h>' \
        'static inline int64_t os_time_get_nano(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * INT64_C(1000000000) + t.tv_nsec; }' \
        > "$out/tw/util/os_time.h"
    cp "$radv_src/src/util/ps5_timed_wait.h" "$out/tw/util/"
    cc -std=gnu11 -O1 -g -Wall -Wextra -fsanitize=address,undefined -I "$out/tw" "$tests/timed_wait_test.c" -o "$out/timed_wait_test" -lpthread
    "$out/timed_wait_test" || status=1
else
    echo "timed waits: no patched driver tree at $radv_src (skipped)"
fi

glslangValidator -V "$tests/test.vert" -o "$out/vert.spv" > /dev/null
glslangValidator -V "$tests/test.frag" -o "$out/frag.spv" > /dev/null

# The functions the fork builds for the console only (geometry shaders run as compute), which
# the rest of the driver names: stand-ins, since no test here has a geometry shader.
stubs="$out/driver_stubs.c"
{
    echo '#include <stdbool.h>'
    echo '#include <stdlib.h>'
    for build in "$work"/build-*; do
        [[ -f $build/src/amd/vulkan/libvulkan_radeon.so ]] || continue
        nm -D --undefined-only "$build/src/amd/vulkan/libvulkan_radeon.so" | awk '$2 ~ /^radv_gs_compute_/ { print $2 }'
    done | sort -u | while read -r name; do
        case $name in
            radv_gs_compute_wanted) echo "bool $name(void) { return false; }" ;;
            radv_gs_compute_deferred_vs_destroy) echo "void $name(void) { }" ;;
            *) echo "void *$name(void) { abort(); }" ;;
        esac
    done
} > "$stubs"

run() { # run BUILD PROGRAM THREADED MODE...
    local build=$1 program=$2 threaded=$3
    shift 3
    LD_PRELOAD="${preload:+$preload:}$build/src/amd/drm-shim/libamdgpu_noop_drm_shim.so" AMDGPU_GPU_ID=navi21 \
        ASAN_OPTIONS=detect_leaks=0 RADV_THREADED_RECORDING=$threaded \
        "$program" "$build/src/amd/vulkan/libvulkan_radeon.so" "$out/vert.spv" "$out/frag.spv" "$@"
}

build="$work/build-asan"
if [[ -f $build/src/amd/vulkan/libvulkan_radeon.so ]]; then
    echo "== threaded recording: the same commands"
    cc -std=gnu11 -O1 -g -Wall -Wextra -fsanitize=address -rdynamic -I "$work/src/include" \
        "$tests/threaded_recording_test.c" "$stubs" -o "$out/threaded_recording_test" -ldl
    preload=$(cc -print-file-name=libasan.so)
    for threaded in 0 1; do
        RADV_DEBUG=dumpibs run "$build" "$out/threaded_recording_test" $threaded check \
            > "$out/check-$threaded.txt" 2> "$out/commands-$threaded.txt" ||
            { echo "FAILED: the run with threaded recording $threaded"; tail -20 "$out/commands-$threaded.txt"; status=1; }
        cat "$out/check-$threaded.txt"
    done
    draws=$(grep -c "DRAW_INDEX" "$out/commands-0.txt" || true)
    if [[ $draws -lt 1000 ]]; then
        echo "FAILED: the driver printed $draws draws (RADV_DEBUG=dumpibs did not take?)"
        status=1
    elif cmp -s "$out/commands-0.txt" "$out/commands-1.txt"; then
        echo "the same $(wc -l < "$out/commands-0.txt") lines of commands either way ($draws draws)"
    else
        echo "FAILED: the recorded commands differ with threaded recording:"
        diff "$out/commands-0.txt" "$out/commands-1.txt" | head -20
        status=1
    fi
else
    echo "== threaded recording: skipped (no $build: build-host-driver.sh <Mesa tree> $work asan)"
fi

build="$work/build-plain"
if [[ -f $build/src/amd/vulkan/libvulkan_radeon.so ]]; then
    echo "== threaded recording: what it costs the calling thread on this PC"
    cc -std=gnu11 -O2 -g -Wall -Wextra -rdynamic -I "$work/src/include" \
        "$tests/threaded_recording_test.c" "$stubs" -o "$out/threaded_recording_time" -ldl
    preload=
    for threaded in 0 1; do
        run "$build" "$out/threaded_recording_time" $threaded time 60 1200 || status=1
    done
fi

[[ $status == 0 ]] && echo "driver tests passed" || echo "driver tests FAILED"
exit $status
