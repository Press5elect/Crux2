#!/usr/bin/env bash
# Crux2 - the compute effects' shaders, compiled to SPIR-V for ps5/frontend/ps5_compute_fx.cpp.
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0+
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
#
#   ps5/tools/make-compute-fx.sh            writes ps5/frontend/ps5_compute_fx_spirv.inc
#   ps5/tools/make-compute-fx.sh --check    compiles everything and says whether the checked-in
#                                           file is what these sources make (exit 1 if not)
#   ps5/tools/make-compute-fx.sh --out DIR  also leaves each .spv in DIR (for the tests)
#
# The title compiles no shader of these at run time: the SPIR-V is in the .inc as uint32_t arrays,
# which is checked in, so a build of the title needs no shader compiler for them. Run this after
# changing any of the sources:
#   ps5/frontend/third_party/fidelityfx/crux2/of_passes.comp   AMD's FSR 3 optical flow (7 passes, and Crux2's luma gain)
#   ps5/frontend/third_party/fidelityfx/crux2/fg_warp.comp     Crux2's camera, refine, verdict, composite
#   ps5/frontend/third_party/nis/crux2/nis_main.comp           NVIDIA Image Scaling
# and what they include (third_party/fidelityfx/gpu, third_party/nis/NIS_Scaler.h).
#
# Needs glslangValidator and spirv-val (and spirv-opt, to drop names and source text), the
# packages glslang-tools and spirv-tools. SPIR-V 1.3 (Vulkan 1.1), which is what PCSX2 asks for.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ps5=$(cd "$here/.." && pwd)
ffx="$ps5/frontend/third_party/fidelityfx"
nis="$ps5/frontend/third_party/nis"
inc="$ps5/frontend/ps5_compute_fx_spirv.inc"

check=0
keep=""
while [[ $# -gt 0 ]]; do
    case $1 in
        --check) check=1 ;;
        --out) keep=${2:?--out needs a folder}; shift ;;
        *) echo "usage: make-compute-fx.sh [--check] [--out folder]" >&2; exit 2 ;;
    esac
    shift
done

for tool in glslangValidator spirv-val spirv-opt python3; do
    if ! command -v "$tool" > /dev/null; then
        echo "make-compute-fx: $tool is missing (glslang-tools, spirv-tools, python3)" >&2
        exit 2
    fi
done

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# name | source | defines
variants=(
    "OfPrepareLuma|$ffx/crux2/of_passes.comp|-DOF_PASS=0"
    "OfLumaPyramid|$ffx/crux2/of_passes.comp|-DOF_PASS=1"
    "OfScdHistogram|$ffx/crux2/of_passes.comp|-DOF_PASS=2"
    "OfScdDivergence|$ffx/crux2/of_passes.comp|-DOF_PASS=3"
    "OfSearch|$ffx/crux2/of_passes.comp|-DOF_PASS=4"
    "OfFilter|$ffx/crux2/of_passes.comp|-DOF_PASS=5"
    "OfScale|$ffx/crux2/of_passes.comp|-DOF_PASS=6"
    "OfLumaGain|$ffx/crux2/of_passes.comp|-DOF_PASS=7"
    "FgRefine|$ffx/crux2/fg_warp.comp|-DFG_PASS=0 -DOUT_FORMAT=0"
    "FgCamera|$ffx/crux2/fg_warp.comp|-DFG_PASS=2 -DOUT_FORMAT=0"
    "FgVerdict|$ffx/crux2/fg_warp.comp|-DFG_PASS=3 -DOUT_FORMAT=0"
    "FgCompositeRgba8|$ffx/crux2/fg_warp.comp|-DFG_PASS=1 -DOUT_FORMAT=0"
    "FgCompositeRgba16f|$ffx/crux2/fg_warp.comp|-DFG_PASS=1 -DOUT_FORMAT=1"
    "FgCompositeUnformatted|$ffx/crux2/fg_warp.comp|-DFG_PASS=1 -DOUT_FORMAT=2"
    "NisScalerRgba8|$nis/crux2/nis_main.comp|-DNIS_SCALER=1 -DNIS_USE_HALF_PRECISION=0 -DOUT_FORMAT=0"
    "NisScalerRgba16f|$nis/crux2/nis_main.comp|-DNIS_SCALER=1 -DNIS_USE_HALF_PRECISION=0 -DOUT_FORMAT=1"
    "NisScalerUnformatted|$nis/crux2/nis_main.comp|-DNIS_SCALER=1 -DNIS_USE_HALF_PRECISION=0 -DOUT_FORMAT=2"
    "NisScalerHalfRgba8|$nis/crux2/nis_main.comp|-DNIS_SCALER=1 -DNIS_USE_HALF_PRECISION=1 -DOUT_FORMAT=0"
    "NisScalerHalfRgba16f|$nis/crux2/nis_main.comp|-DNIS_SCALER=1 -DNIS_USE_HALF_PRECISION=1 -DOUT_FORMAT=1"
    "NisScalerHalfUnformatted|$nis/crux2/nis_main.comp|-DNIS_SCALER=1 -DNIS_USE_HALF_PRECISION=1 -DOUT_FORMAT=2"
    "NisSharpenRgba8|$nis/crux2/nis_main.comp|-DNIS_SCALER=0 -DNIS_USE_HALF_PRECISION=0 -DOUT_FORMAT=0"
    "NisSharpenRgba16f|$nis/crux2/nis_main.comp|-DNIS_SCALER=0 -DNIS_USE_HALF_PRECISION=0 -DOUT_FORMAT=1"
    "NisSharpenUnformatted|$nis/crux2/nis_main.comp|-DNIS_SCALER=0 -DNIS_USE_HALF_PRECISION=0 -DOUT_FORMAT=2"
)

names=()
for variant in "${variants[@]}"; do
    IFS='|' read -r name source defines <<< "$variant"
    # shellcheck disable=SC2086
    if ! glslangValidator -V --target-env vulkan1.1 -S comp $defines -I"$ffx/gpu" -I"$ffx/crux2" -I"$nis" \
        "$source" -o "$work/$name.raw.spv" > "$work/$name.log" 2>&1; then
        echo "make-compute-fx: $name did not compile:" >&2
        cat "$work/$name.log" >&2
        exit 1
    fi
    spirv-opt --strip-debug "$work/$name.raw.spv" -o "$work/$name.spv"
    if ! spirv-val --target-env vulkan1.1 "$work/$name.spv"; then
        echo "make-compute-fx: $name is not valid SPIR-V" >&2
        exit 1
    fi
    names+=("$name")
    if [[ -n $keep ]]; then
        mkdir -p "$keep"
        cp "$work/$name.spv" "$keep/$name.spv"
    fi
done

python3 - "$work" "$work/out.inc" "${names[@]}" <<'PY'
import struct, sys
work, out, names = sys.argv[1], sys.argv[2], sys.argv[3:]
lines = [
    "// Crux2 - the SPIR-V of the compute effects (FSR 3 optical flow, frame generation, NIS).",
    "// SPDX-License-Identifier: GPL-3.0+",
    "//",
    "// Made by ps5/tools/make-compute-fx.sh from ps5/frontend/third_party/fidelityfx/crux2/*.comp and",
    "// ps5/frontend/third_party/nis/crux2/nis_main.comp; do not edit, run the script. The shaders",
    "// carry AMD's FidelityFX code (MIT) and NVIDIA's Image Scaling code (MIT): see the READMEs there.",
    "",
]
total = 0
for name in names:
    data = open("%s/%s.spv" % (work, name), "rb").read()
    words = struct.unpack("<%dI" % (len(data) // 4), data)
    total += len(data)
    lines.append("static const uint32_t kSpirv%s[%d] = {" % (name, len(words)))
    for i in range(0, len(words), 10):
        lines.append("\t" + ", ".join("0x%08x" % w for w in words[i:i + 10]) + ",")
    lines.append("};")
    lines.append("")
lines.append("// %d shaders, %d bytes of SPIR-V." % (len(names), total))
open(out, "w").write("\n".join(lines) + "\n")
PY

if [[ $check == 1 ]]; then
    if cmp -s "$work/out.inc" "$inc"; then
        echo "make-compute-fx: $inc is up to date (${#names[@]} shaders)"
        exit 0
    fi
    echo "make-compute-fx: $inc is NOT what the sources make; run ps5/tools/make-compute-fx.sh" >&2
    exit 1
fi
cp "$work/out.inc" "$inc"
echo "make-compute-fx: wrote $inc ($(tail -1 "$inc" | sed 's|^// ||'))"
