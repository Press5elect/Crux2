#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - FSR 1's two passes as the title runs them, on the PC.
#
#   ps5/tests/fsr/run-fsr-tests.sh <work folder>
#
# Puts the title's shader together as PCSX2's Vulkan device does (its header, the stage, the pass,
# AMD's two headers in place of the #include lines), compiles it with glslang, checks the result
# with spirv-val, and runs it on a software Vulkan driver (Mesa's lavapipe: the package
# mesa-vulkan-drivers) against a picture that is known at every size (fsr_test.c).
# Without glslangValidator or a software driver it says what is missing and checks nothing.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ps5=$(cd "$here/../.." && pwd)
work=${1:?usage: run-fsr-tests.sh <work folder>}
mkdir -p "$work"
shaders="$ps5/package/resources/shaders"
icd=/usr/share/vulkan/icd.d/lvp_icd.json
if ! command -v glslangValidator > /dev/null || [[ ! -f $icd ]]; then
    echo "FSR 1: NOT RUN (needs glslangValidator and Mesa's lavapipe, $icd)"
    exit 0
fi

python3 - "$shaders" "$work" <<'PY'
import sys
shaders, work = sys.argv[1], sys.argv[2]
source = open(shaders + "/vulkan/fsr1.glsl").read()
for name in ("fsr1/ffx_a.h", "fsr1/ffx_fsr1.h"):
    include = '#include "%s"' % name
    assert include in source, name
    source = source.replace(include, open(shaders + "/common/" + name).read())
assert not any(line.lstrip().startswith("#include") for line in source.splitlines())
# (The header of GSDeviceVK.cpp's AddShaderHeader and AddShaderStageMacro, and CompileFSRPipelines' macro.)
header = "#version 460 core\n#extension GL_EXT_samplerless_texture_functions : require\n#define FRAGMENT_SHADER 1\n"
for number, name in ((0, "easu"), (1, "rcas")):
    open("%s/%s.frag" % (work, name), "w").write(header + "#define FSR_PASS %d\n" % number + source)
open(work + "/screen.vert", "w").write("""#version 460 core
void main()
{
	const vec2 corner = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
	gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
""")
PY
for name in easu rcas; do
    glslangValidator -V "$work/$name.frag" -o "$work/$name.spv" > "$work/$name.log" || { cat "$work/$name.log"; exit 1; }
    spirv-val "$work/$name.spv"
done
glslangValidator -V "$work/screen.vert" -o "$work/vert.spv" > /dev/null
cc -std=gnu11 -O2 -g -Wall -Wextra -fsanitize=address,undefined "$here/fsr_test.c" -o "$work/fsr_test" -lvulkan -lm
VK_ICD_FILENAMES=$icd VK_DRIVER_FILES=$icd ASAN_OPTIONS=detect_leaks=0 "$work/fsr_test" "$work/easu.spv" "$work/rcas.spv" "$work/vert.spv" "$work"

# The console's own driver, where it was built for the PC (the work folder of
# ps5/tests/driver/build-host-driver.sh, as RADV_HOST_BUILD): it has no graphics chip to draw
# with there, but its compiler makes the two pipelines as it will on the console.
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
        "$work/fsr_test" "$work/easu.spv" "$work/rcas.spv" "$work/vert.spv" --compile-only
else
    echo "  (the console's driver is not built for this PC: its compiler was not asked; RADV_HOST_BUILD names its folder)"
fi
