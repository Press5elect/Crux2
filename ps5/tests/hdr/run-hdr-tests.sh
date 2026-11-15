#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - the last passes before the screen (HDR10 output and debanding) on the PC.
#
#   ps5/tests/hdr/run-hdr-tests.sh <work folder>
#
# Puts ps5_final.glsl together as PCSX2's Vulkan device does (its header, the stage, the pass), compiles
# both passes with glslang, checks them with spirv-val, and runs them on a software Vulkan driver (Mesa's
# lavapipe: the package mesa-vulkan-drivers; with vulkan-validationlayers the validation layer is on)
# against the maths in double precision (hdr_test.cpp). ps5_hdr.h's choices are checked as well. Without
# glslangValidator, a C++ compiler with Vulkan's headers, or a software driver it says what is missing.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ps5=$(cd "$here/../.." && pwd)
work=${1:?usage: run-hdr-tests.sh <work folder>}
mkdir -p "$work"
shader="$ps5/package/resources/shaders/vulkan/ps5_final.glsl"
icd=/usr/share/vulkan/icd.d/lvp_icd.json
if ! command -v glslangValidator > /dev/null || [[ ! -f /usr/include/vulkan/vulkan.h ]]; then
    echo "final passes: NOT RUN (needs glslangValidator and Vulkan's headers: libvulkan-dev)"
    exit 0
fi

# (GSDeviceVK.cpp's AddShaderHeader and AddShaderStageMacro, and PS5FinalUpdate's macro.)
for pass in 0 1; do
    {
        printf '#version 460 core\n#extension GL_EXT_samplerless_texture_functions : require\n#define FRAGMENT_SHADER 1\n#define FINAL_PASS %d\n' "$pass"
        cat "$shader"
    } > "$work/final$pass.frag"
    glslangValidator -V --target-env vulkan1.1 -S frag "$work/final$pass.frag" -o "$work/final$pass.frag.spv" > "$work/final$pass.log" ||
        { cat "$work/final$pass.log"; echo "final passes: FAILED (pass $pass does not compile)"; exit 1; }
    if command -v spirv-val > /dev/null; then
        spirv-val --target-env vulkan1.1 "$work/final$pass.frag.spv"
    fi
done
# A triangle over the screen, its v_tex as convert.glsl's vertex shader gives it (0,0 at the top left).
cat > "$work/screen.vert" <<'GLSL'
#version 460 core
layout(location = 0) out vec2 v_tex;
void main()
{
	const vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
	v_tex = uv;
	gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
GLSL
glslangValidator -V --target-env vulkan1.1 "$work/screen.vert" -o "$work/screen.vert.spv" > /dev/null
echo "final passes: both compile (glslang)$(command -v spirv-val > /dev/null && echo ', spirv-val clean')"

c++ -std=c++20 -O1 -g -Wall -Wextra -Wno-missing-field-initializers -I "$ps5/frontend" "$here/hdr_test.cpp" -o "$work/hdr_test" -lvulkan
if [[ ! -f $icd ]]; then
    echo "final passes: maths NOT RUN (needs Mesa's lavapipe, $icd)"
    exit 0
fi
VK_ICD_FILENAMES=$icd VK_DRIVER_FILES=$icd "$work/hdr_test" "$work"
