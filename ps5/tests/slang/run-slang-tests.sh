#!/usr/bin/env bash
# Crux2 - libretro shader presets (ps5/frontend/ps5_slang.cpp and the presets the title carries), run on the PC.
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0+
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
#
#   ps5/tests/slang/run-slang-tests.sh <work folder>
#
#  1. slang_test (built with the address and undefined behaviour sanitizers): presets that must be
#     refused, each with its sentence; every preset in ps5/package/resources/shaders/slang read,
#     every pass's two stages compiled by glslangValidator for Vulkan 1.0, optimised by spirv-opt -O
#     (what shaderc's "performance" level does in PCSX2's VKShaderCache::CompileShaderToSPV) and
#     checked by spirv-val, and the text reflection compared with glslang's (every member's offset,
#     every block's size and binding, every sampler's binding, what is read); the plan at
#     640x448 -> 3840x2160 and 512x448 -> 1920x1080.
#  2. The same for presets of libretro's slang-shaders that use what the importer must handle
#     (#reference, lookup pictures, aliases, feedback, history, frame_count_mod, mipmaps, absolute
#     sizes, 25 passes): slang-shaders at the revision the title's guest files come from, cloned
#     into the work folder, or SLANG_SHADERS=<a checkout>.
#  3. A fuzz pass: the presets' files mutated in memory, a few thousand times, read and planned.
#  4. slang_vk on lavapipe (mesa-vulkan-drivers), with the Khronos validation layer and its
#     synchronization checks when installed (vulkan-validationlayers): every preset the title
#     carries, and some of slang-shaders' (history, feedback), 5 frames at each of the two sizes;
#     no validation message, no NaN, a plausible picture. The pictures: <work folder>/vk/*.png.
#  5. Cost: each pass's fetches counted on lavapipe; with RADV_HOST_BUILD (the work folder of
#     ps5/tests/driver/build-host-driver.sh) also the console's compiler's statistics for every
#     shader, and tools/estimate-cost.py's estimate of each preset's milliseconds at 4K.
# Without glslangValidator, spirv-opt, spirv-val, python3, a C++ compiler, libpng or lavapipe it
# says what is missing and checks nothing. It exits non-zero when anything fails.
set -uo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ps5=$(cd "$here/../.." && pwd)
work=${1:?usage: run-slang-tests.sh <work folder>}
mkdir -p "$work"
work=$(cd "$work" && pwd)
root="$ps5/package/resources/shaders/slang"
icd=/usr/share/vulkan/icd.d/lvp_icd.json
revision=e1d75632a205c70f14a4cc947c46d5abb7b3f7f1
cxx=${CXX:-c++}
for tool in glslangValidator spirv-opt spirv-val python3 "$cxx" git; do
    if ! command -v "$tool" > /dev/null; then
        echo "slang: NOT RUN (needs $tool: glslang-tools, spirv-tools, python3, a C++ compiler, git)"
        exit 0
    fi
done
if ! echo '#include <png.h>' | "$cxx" -E -x c++ - > /dev/null 2>&1; then
    echo "slang: NOT RUN (needs libpng's headers: libpng-dev)"
    exit 0
fi
if [[ ! -f $icd ]]; then
    echo "slang: NOT RUN (needs Mesa's lavapipe, $icd: mesa-vulkan-drivers)"
    exit 0
fi

failed=0
summary=()
note() { summary+=("$1"); }

shipped=$(find "$root" -name '*.slangp' | sort)

# 1. The importer, sanitized.
"$cxx" -std=c++20 -fno-exceptions -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -I"$ps5/frontend" "$ps5/frontend/ps5_slang.cpp" "$here/slang_test.cpp" -o "$work/slang_test" || exit 1
export ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1
echo "  presets that must be refused:"
if "$work/slang_test" refuse > "$work/refuse.txt"; then note "refusals: $(tail -1 "$work/refuse.txt" | sed 's/^ *refuse: //')"; else cat "$work/refuse.txt"; failed=1; note "refusals: FAILED"; fi
tail -1 "$work/refuse.txt"
echo "  the presets the title carries (compiled, reflection against glslang's, planned):"
mkdir -p "$work/check-shipped"
# shellcheck disable=SC2086
if "$work/slang_test" check "$work/check-shipped" "$root" $shipped > "$work/check-shipped.txt" 2>&1; then
    note "shipped presets: $(grep '^  check:' "$work/check-shipped.txt" | sed 's/^  check: //')"
else
    grep -E 'FAIL|VALIDATION' "$work/check-shipped.txt" | head -40; failed=1; note "shipped presets: FAILED (see $work/check-shipped.txt)"
fi
grep -E '^  check:|^         whether' "$work/check-shipped.txt"

# 2. slang-shaders' presets that use everything.
upstream=${SLANG_SHADERS:-}
if [[ -z $upstream ]]; then
    upstream="$work/slang-shaders"
    if [[ ! -d $upstream/.git ]] || [[ $(git -C "$upstream" rev-parse HEAD 2> /dev/null) != "$revision" ]]; then
        rm -rf "$upstream"
        if ! { git init -q "$upstream" && git -C "$upstream" fetch -q --depth 1 https://github.com/libretro/slang-shaders "$revision" &&
            git -C "$upstream" checkout -q FETCH_HEAD; } > "$work/clone.log" 2>&1; then
            upstream=""
        fi
    fi
fi
tricky=(crt/crt-guest-advanced-ntsc.slangp crt/crt-guest-advanced-pal.slangp crt/crt-royale.slangp crt/crt-geom.slangp
    crt/crt-yah.slangp crt/crt-maximus-royale.slangp crt/crt-lottes-multipass-glow.slangp crt/crt-hyllian-ntsc-rainbow.slangp
    crt/cathode-retro_no-signal.slangp ntsc/ntsc-320px-composite-scanline.slangp ntsc/blargg.slangp ntsc/mame-ntsc.slangp
    ntsc/ntsc-adaptive.slangp ntsc/patchy-genplusgx.slangp motionblur/motionblur-blue.slangp motionblur/feedback.slangp
    motionblur/braid-rewind.slangp test/feedback-noncausal.slangp)
if [[ -n $upstream ]]; then
    echo "  slang-shaders' presets ($(git -C "$upstream" rev-parse --short HEAD 2> /dev/null || echo "$upstream")):"
    mkdir -p "$work/check-upstream"
    if "$work/slang_test" check "$work/check-upstream" "$upstream" "${tricky[@]/#/$upstream/}" > "$work/check-upstream.txt" 2>&1; then
        note "slang-shaders presets: $(grep '^  check:' "$work/check-upstream.txt" | sed 's/^  check: //')"
    else
        grep -E 'FAIL' "$work/check-upstream.txt" | head -40; failed=1; note "slang-shaders presets: FAILED (see $work/check-upstream.txt)"
    fi
    grep -E '^  check:|^         whether' "$work/check-upstream.txt"
else
    echo "  slang-shaders' presets: NOT RUN (cannot clone github.com/libretro/slang-shaders at $revision: $work/clone.log; or set SLANG_SHADERS)"
    note "slang-shaders presets: NOT RUN (no clone)"
fi

# 3. Fuzz.
echo "  fuzz:"
# shellcheck disable=SC2086
if "$work/slang_test" fuzz "$root" 1 3000 $shipped > "$work/fuzz.txt" 2>&1; then :; else failed=1; fi
if [[ -n $upstream ]]; then
    "$work/slang_test" fuzz "$upstream" 2 1500 "$upstream/ntsc/blargg.slangp" "$upstream/motionblur/feedback.slangp" \
        "$upstream/crt/crt-yah.slangp" "$upstream/crt/crt-geom.slangp" "$upstream/motionblur/motionblur-blue.slangp" >> "$work/fuzz.txt" 2>&1 || failed=1
fi
grep -E 'fuzz:|FAIL|ERROR|runtime error' "$work/fuzz.txt" | head -20
grep -qE 'ERROR: AddressSanitizer|runtime error' "$work/fuzz.txt" && failed=1
note "fuzz: $(grep -c 'fuzz:' "$work/fuzz.txt") runs, $(grep 'fuzz:' "$work/fuzz.txt" | awk '{n += $2} END {print n}') mutated presets, $(grep -cE 'FAIL|ERROR|runtime error' "$work/fuzz.txt") problems"

# 4. On lavapipe.
"$cxx" -std=c++20 -fno-exceptions -O2 -g -Wall -Wextra -Wno-missing-field-initializers -I"$ps5/frontend" -I"$here" \
    "$ps5/frontend/ps5_slang.cpp" "$here/slang_vk.cpp" -o "$work/slang_vk" -lvulkan -lpng || exit 1
echo "  on lavapipe:"
ok=1
# shellcheck disable=SC2086
VK_ICD_FILENAMES=$icd VK_DRIVER_FILES=$icd "$work/slang_vk" "$work/vk" "$root" $shipped > "$work/vk.txt" 2>&1 || ok=0
if [[ -n $upstream ]]; then
    # History (7 frames), feedback, a plain chain and a single pass of slang-shaders' own.
    VK_ICD_FILENAMES=$icd VK_DRIVER_FILES=$icd "$work/slang_vk" "$work/vk" "$upstream" "$upstream/motionblur/braid-rewind.slangp" \
        "$upstream/motionblur/feedback.slangp" "$upstream/ntsc/blargg.slangp" "$upstream/crt/crt-geom.slangp" >> "$work/vk.txt" 2>&1 || ok=0
fi
grep -E '^  device|ok  |FAIL|VALIDATION|lavapipe ms|slang \(Vulkan\)' "$work/vk.txt"
if grep -q 'validation layer NOT INSTALLED' "$work/vk.txt"; then echo "  (the validation layer is not installed: vulkan-validationlayers)"; fi
[[ $ok == 1 ]] || failed=1
note "lavapipe: $(grep -c '^    ok  ' "$work/vk.txt") runs plausible, $(grep -c '^    FAIL' "$work/vk.txt") failed, $(grep 'slang (Vulkan)' "$work/vk.txt" | awk '{s += $(NF-2)} END {print s+0}') validation messages; pictures in $work/vk"

# 5. Cost.
echo "  cost (fetches counted at 640x448 -> 1920x1080, scaled to each pass's pixels at 4K):"
rm -f "$work/count/fetches.tsv"
# shellcheck disable=SC2086
VK_ICD_FILENAMES=$icd VK_DRIVER_FILES=$icd VK_LOADER_LAYERS_DISABLE='*' "$work/slang_vk" "$work/count" "$root" $shipped --count > "$work/count.txt" 2>&1 || failed=1
grep -E 'cost|at 4K' "$work/count.txt"
radv=${RADV_HOST_BUILD:-}
if [[ -n $radv && -f $radv/build-asan/src/amd/vulkan/libvulkan_radeon.so ]]; then
    build="$radv/build-asan"
    {
        echo '#include <stdbool.h>'
        echo '#include <stdlib.h>'
        nm -D --undefined-only "$build/src/amd/vulkan/libvulkan_radeon.so" | awk '$2 ~ /^radv_gs_compute_/ { print $2 }' | sort -u | while read -r name; do
            case $name in
                radv_gs_compute_wanted) echo "bool $name(void) { return false; }" ;;
                radv_gs_compute_deferred_vs_destroy) echo "void $name(void) { }" ;;
                *) echo "void *$name(void) { abort(); }" ;;
            esac
        done
    } > "$work/driver_stubs.c"
    cc -shared -fPIC -O1 "$work/driver_stubs.c" -o "$work/libdriver_stubs.so"
    printf '{"file_format_version": "1.0.0", "ICD": {"library_path": "%s", "api_version": "1.4.0"}}\n' \
        "$build/src/amd/vulkan/libvulkan_radeon.so" > "$work/radv_icd.json"
    g++ -std=c++20 -fno-exceptions -O1 -g -Wno-missing-field-initializers -Wno-unused-result -fsanitize=address -I"$ps5/frontend" -I"$here" \
        "$ps5/frontend/ps5_slang.cpp" "$here/slang_vk.cpp" -o "$work/slang_vk_radv" -lvulkan -lpng
    mkdir -p "$work/radv"
    for preset in $shipped; do
        name=$(basename "$preset" .slangp)
        RADV_DEBUG=shaderstats,nocache MESA_SHADER_CACHE_DISABLE=1 \
            LD_PRELOAD="$(cc -print-file-name=libasan.so):$build/src/amd/drm-shim/libamdgpu_noop_drm_shim.so:$work/libdriver_stubs.so" \
            AMDGPU_GPU_ID=navi21 VK_ICD_FILENAMES="$work/radv_icd.json" VK_DRIVER_FILES="$work/radv_icd.json" ASAN_OPTIONS=detect_leaks=0 \
            VK_LOADER_LAYERS_DISABLE='*' "$work/slang_vk_radv" "$work/radv" "$root" "$preset" --compile-only > /dev/null 2> "$work/radv/$name.stats"
    done
    echo "  estimated on the PS5 (the console's compiler's statistics, tools/estimate-cost.py):"
    python3 -I "$here/tools/estimate-cost.py" "$work/count/fetches.tsv" "$work/radv" | sed 's/^/    /' | tee "$work/estimate.txt"
    note "cost: $(grep -c 'over 2 ms' "$work/estimate.txt") presets estimated over 2 ms at 4K"
else
    echo "  (the console's driver is not built for this PC: no millisecond estimate; RADV_HOST_BUILD names its folder)"
fi

echo "slang summary:"
for line in "${summary[@]}"; do echo "  $line"; done
if [[ $failed != 0 ]]; then
    echo "slang: FAILED"
    exit 1
fi
echo "slang: all passed"
