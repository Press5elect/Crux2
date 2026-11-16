#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - downsampling and SMAA (ps5_aa.glsl) on the PC.
#
#   ps5/tests/aa/run-aa-tests.sh <work folder>
#
# Checks ps5_aa.h's decisions (aa_logic_test.cpp, with the sanitizers); puts ps5_aa.glsl together as
# PCSX2's Vulkan device does (its header, the stage, the pass; SMAA.hlsl in place of its #include line, as
# GSDeviceVK_PS5.cpp's PS5PrepareAA does), compiles the five passes with glslang (and the two downsample
# passes without SMAA.hlsl, as the device makes them when only downsampling is asked for), checks them with
# spirv-val, and runs them on a software Vulkan driver (Mesa's lavapipe: the package mesa-vulkan-drivers;
# with vulkan-validationlayers the validation layer is on) against the maths in double precision
# (aa_test.cpp). Without glslangValidator, Vulkan's headers or a software driver it says what is missing.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ps5=$(cd "$here/../.." && pwd)
work=${1:?usage: run-aa-tests.sh <work folder>}
mkdir -p "$work"
CXX=${CXX:-c++}
"$CXX" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined -I "$ps5/frontend" \
    "$here/aa_logic_test.cpp" -o "$work/aa_logic_test"
"$work/aa_logic_test"

shaders="$ps5/package/resources/shaders"
icd=/usr/share/vulkan/icd.d/lvp_icd.json
if ! command -v glslangValidator > /dev/null || ! command -v python3 > /dev/null || [[ ! -f /usr/include/vulkan/vulkan.h ]]; then
    echo "downsampling and SMAA passes: NOT RUN (needs glslangValidator, python3 and Vulkan's headers: libvulkan-dev)"
    exit 0
fi

# (GSDeviceVK.cpp's AddShaderHeader and AddShaderStageMacro, PS5PrepareAA's macro and SMAA.hlsl put in.)
python3 -I - "$shaders" "$work" <<'PY'
import sys
shaders, work = sys.argv[1], sys.argv[2]
with open(shaders + "/vulkan/ps5_aa.glsl", "rb") as f:
    source = f.read()
with open(shaders + "/common/smaa/SMAA.hlsl", "rb") as f:
    smaa = f.read()
header = b"#version 460 core\n#extension GL_EXT_samplerless_texture_functions : require\n#define FRAGMENT_SHADER 1\n"
whole = source.replace(b'#include "smaa/SMAA.hlsl"', smaa)
assert whole != source, "ps5_aa.glsl has no #include line for SMAA.hlsl"
for p in range(5):
    with open(f"{work}/aa{p}.frag", "wb") as f:
        f.write(header + b"#define AA_PASS %d\n" % p + whole)
for p in range(2):
    with open(f"{work}/aa{p}-alone.frag", "wb") as f:
        f.write(header + b"#define AA_PASS %d\n" % p + source)
PY
for name in aa0 aa1 aa2 aa3 aa4 aa0-alone aa1-alone; do
    glslangValidator -V --target-env vulkan1.1 -S frag "$work/$name.frag" -o "$work/$name.frag.spv" > "$work/$name.log" ||
        { cat "$work/$name.log"; echo "downsampling and SMAA passes: FAILED ($name does not compile)"; exit 1; }
    if command -v spirv-val > /dev/null; then
        spirv-val --target-env vulkan1.1 "$work/$name.frag.spv"
    fi
done
# A triangle over the picture, its v_tex as convert.glsl's vertex shader gives it (the passes read gl_FragCoord).
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
echo "downsampling and SMAA passes: all five compile (glslang)$(command -v spirv-val > /dev/null && echo ', spirv-val clean')"

"$CXX" -std=c++20 -O1 -g -Wall -Wextra -Wno-missing-field-initializers -I "$ps5/frontend" "$here/aa_test.cpp" -o "$work/aa_test" -lvulkan
if [[ ! -f $icd ]]; then
    echo "downsampling and SMAA passes: maths NOT RUN (needs Mesa's lavapipe, $icd)"
    exit 0
fi
VK_ICD_FILENAMES=$icd VK_DRIVER_FILES=$icd "$work/aa_test" "$work"
