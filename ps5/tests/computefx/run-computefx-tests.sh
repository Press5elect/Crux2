#!/usr/bin/env bash
# Crux2 - the compute effects (FSR 3 optical flow frame generation, NVIDIA Image Scaling), run on the PC.
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0+
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
#
#   ps5/tests/computefx/run-computefx-tests.sh <work folder> [--with-framegen]
#
#  1. The SPIR-V the title carries (ps5/frontend/ps5_compute_fx_spirv.inc) is what its sources make
#     (ps5/tools/make-compute-fx.sh --check), and every shader passes spirv-val.
#  2. FSR 3's own frame interpolation passes (fsr3fi/, AMD's GLSL entry points over the vendored
#     2.3.0 headers) are compiled too: the test runs them beside Crux2's composite to compare.
#  3. computefx_test.cpp and ps5_compute_fx.cpp are built (with the address and undefined
#     behaviour sanitizers) and run on a software Vulkan driver (Mesa's lavapipe, the package
#     mesa-vulkan-drivers): frame generation on scenes whose every moment can be drawn, the scene
#     change detection, NIS against the truth and a bilinear stretch, timings. The pictures it
#     makes are left in <work folder>/out.
#  4. --with-framegen: Crux2's own frame generation (ps5/tests/framegen/run-framegen-tests.sh) is
#     run as well, for its numbers on the same scenes.
#  5. With RADV_HOST_BUILD (the work folder of ps5/tests/driver/build-host-driver.sh), the console's
#     driver compiles every pipeline of both effects, as run-framegen-tests.sh has it do.
# Without glslangValidator, spirv-val, a C++ compiler or lavapipe it says what is missing and
# checks nothing. It exits non-zero when anything fails.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ps5=$(cd "$here/../.." && pwd)
work=${1:?usage: run-computefx-tests.sh <work folder> [--with-framegen]}
framegen=0
[[ ${2:-} == --with-framegen ]] && framegen=1
mkdir -p "$work/out" "$work/fsr3fi"
icd=/usr/share/vulkan/icd.d/lvp_icd.json
for tool in glslangValidator spirv-val spirv-opt python3 c++; do
    if ! command -v "$tool" > /dev/null; then
        echo "compute effects: NOT RUN (needs $tool: glslang-tools, spirv-tools, python3, a C++ compiler)"
        exit 0
    fi
done
if [[ ! -f $icd ]]; then
    echo "compute effects: NOT RUN (needs Mesa's lavapipe, $icd)"
    exit 0
fi

# 1. The title's SPIR-V.
"$ps5/tools/make-compute-fx.sh" --check --out "$work/spirv"

# 2. FSR 3's frame interpolation, for the comparison (FSR 3's permutation without low resolution or
#    jittered motion vectors, depth not inverted).
ffx="$ps5/frontend/third_party/fidelityfx/gpu"
for source in "$here"/fsr3fi/*.glsl; do
    name=$(basename "$source" .glsl)
    if ! glslangValidator -V --target-env vulkan1.1 -S comp -DFFX_GPU=1 -DFFX_GLSL=1 -DFFX_HALF=0 \
        -DFFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS=0 -DFFX_FRAMEINTERPOLATION_OPTION_JITTERED_MOTION_VECTORS=0 \
        -DFFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH=0 -I"$ffx" "$source" -o "$work/fsr3fi/$name.spv" > "$work/fsr3fi/$name.log"; then
        cat "$work/fsr3fi/$name.log"
        exit 1
    fi
    spirv-val --target-env vulkan1.1 "$work/fsr3fi/$name.spv"
done

# 3. The test, on lavapipe.
c++ -std=c++20 -fno-exceptions -O2 -g -Wall -Wextra -Wno-missing-field-initializers -fsanitize=address,undefined \
    -I"$here" "$here/computefx_test.cpp" "$ps5/frontend/ps5_compute_fx.cpp" -o "$work/computefx_test" -lvulkan
VK_ICD_FILENAMES=$icd VK_DRIVER_FILES=$icd ASAN_OPTIONS=detect_leaks=0 "$work/computefx_test" "$work/out" --fsr3-fi "$work/fsr3fi"

# 4. Crux2's own frame generation on the same scenes.
if [[ $framegen == 1 ]]; then
    echo "  Crux2's own frame generation (ps5/tests/framegen), for comparison:"
    "$ps5/tests/framegen/run-framegen-tests.sh" "$work/framegen"
fi

# 5. The console's driver, where it was built for the PC: it has no graphics chip to run on there,
#    but its compiler makes the pipelines as it will on the console.
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
    c++ -std=c++20 -fno-exceptions -O1 -g -Wno-missing-field-initializers -fsanitize=address \
        -I"$here" "$here/computefx_test.cpp" "$ps5/frontend/ps5_compute_fx.cpp" -o "$work/computefx_test_radv" -lvulkan
    RADV_DEBUG=shaderstats LD_PRELOAD="$(cc -print-file-name=libasan.so):$build/src/amd/drm-shim/libamdgpu_noop_drm_shim.so:$work/libdriver_stubs.so" \
        AMDGPU_GPU_ID=navi21 VK_ICD_FILENAMES="$work/radv_icd.json" VK_DRIVER_FILES="$work/radv_icd.json" ASAN_OPTIONS=detect_leaks=0 \
        "$work/computefx_test_radv" "$work/out" --compile-only 2> "$work/radv-shaderstats.txt"
    echo "  (the console's compiler's statistics for every shader: $work/radv-shaderstats.txt)"
else
    echo "  (the console's driver is not built for this PC: its compiler was not asked; RADV_HOST_BUILD names its folder)"
fi
