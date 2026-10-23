#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - frame generation's passes as the title runs them, on the PC.
#
#   ps5/tests/framegen/run-framegen-tests.sh <work folder>
#
# Puts the title's shader under the header PCSX2's Vulkan device gives its utility shaders, one
# file for each pass, compiles them with glslang, checks the result with spirv-val, and runs them
# on a software Vulkan driver (Mesa's lavapipe: the package mesa-vulkan-drivers) on scenes whose
# every moment can be drawn (fg_test.c).
# Without glslangValidator or a software driver it says what is missing and checks nothing.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ps5=$(cd "$here/../.." && pwd)
work=${1:?usage: run-framegen-tests.sh <work folder>}
mkdir -p "$work"
shaders="$ps5/package/resources/shaders"
icd=/usr/share/vulkan/icd.d/lvp_icd.json
if ! command -v glslangValidator > /dev/null || [[ ! -f $icd ]]; then
    echo "frame generation: NOT RUN (needs glslangValidator and Mesa's lavapipe, $icd)"
    exit 0
fi

python3 - "$shaders" "$work" <<'PY'
import sys
shaders, work = sys.argv[1], sys.argv[2]
source = open(shaders + "/vulkan/framegen.glsl").read()
# (The header of GSDeviceVK.cpp's AddShaderHeader and AddShaderStageMacro, and CompileFrameGenPipelines' macro.)
header = "#version 460 core\n#extension GL_EXT_samplerless_texture_functions : require\n#define FRAGMENT_SHADER 1\n"
for number in range(9):
    open("%s/pass%d.frag" % (work, number), "w").write(header + "#define FG_PASS %d\n" % number + source)
open(work + "/screen.vert", "w").write("""#version 460 core
void main()
{
	const vec2 corner = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
	gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
""")
PY
for number in 0 1 2 3 4 5 6 7 8; do
    glslangValidator -V "$work/pass$number.frag" -o "$work/pass$number.spv" > "$work/pass$number.log" || { cat "$work/pass$number.log"; exit 1; }
    spirv-val "$work/pass$number.spv"
done
glslangValidator -V "$work/screen.vert" -o "$work/vert.spv" > /dev/null
cc -std=gnu11 -O2 -g -Wall -Wextra -fsanitize=address,undefined "$here/fg_test.c" -o "$work/fg_test" -lvulkan -lm
VK_ICD_FILENAMES=$icd VK_DRIVER_FILES=$icd ASAN_OPTIONS=detect_leaks=0 "$work/fg_test" "$work" "$work"

# The console's own driver, where it was built for the PC (the work folder of
# ps5/tests/driver/build-host-driver.sh, as RADV_HOST_BUILD): it has no graphics chip to draw
# with there, but its compiler makes the eight pipelines as it will on the console.
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
    LD_PRELOAD="$(cc -print-file-name=libasan.so):$build/src/amd/drm-shim/libamdgpu_noop_drm_shim.so:$work/libdriver_stubs.so" AMDGPU_GPU_ID=navi21 \
        VK_ICD_FILENAMES="$work/radv_icd.json" VK_DRIVER_FILES="$work/radv_icd.json" ASAN_OPTIONS=detect_leaks=0 \
        "$work/fg_test" "$work" --compile-only
else
    echo "  (the console's driver is not built for this PC: its compiler was not asked; RADV_HOST_BUILD names its folder)"
fi
