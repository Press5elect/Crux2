#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - a PC build of the console's graphics driver sources, for run-driver-tests.sh.
#
#   ps5/tests/driver/build-host-driver.sh <Mesa tree> <work directory> [asan|plain]
#
# <Mesa tree> is the PS5_Mesa fork at the revision the title is built from, with ps5/patches/mesa
# applied (PS5_Vulkan's .deps/work/radv-src is one, once the patches are in it). It is copied,
# and the copy is built for the PC as RADV for Linux, with Mesa's amdgpu "noop" shim (a fake
# kernel driver: no AMD GPU is needed, nothing is executed). "asan" (the default) builds with
# assertions and the address sanitizer, for the checks; "plain" as the console's archive is
# built (optimised, no assertions), for timing.
#
# The copy differs from the tree in three places, none of them in what the tests look at:
#   - the libdrm it asks for: the PC's own (the fork asks for a newer one than Ubuntu 24.04 has,
#     for a query the shim does not answer either, which is skipped);
#   - the boot-log lines of patch 0001, which only the console build defines;
# and the test program stands in for the geometry-shader code the fork builds for the console
# only (run-driver-tests.sh writes those few functions; no test has a geometry shader).
#
# Needs: meson, ninja, a C and C++ compiler, libdrm's development files, python3 with mako and
# yaml, and mesa_clc and vtn_bindgen2 on PATH (PS5_Vulkan's tools/build-radv.sh builds them into
# its .deps/work/radv-clc-bin).
set -euo pipefail

source_tree=$(cd -- "$1" && pwd)
work=$2
variant=${3:-asan}
mkdir -p "$work"
work=$(cd -- "$work" && pwd)
copy="$work/src"
build="$work/build-$variant"

rsync -a --delete --exclude=/.git "$source_tree/" "$copy/"
installed=$(pkg-config --modversion libdrm_amdgpu)
python3 - "$copy" "$installed" <<'PY'
import re, sys
from pathlib import Path
copy, installed = Path(sys.argv[1]), sys.argv[2]

path = copy / "meson.build"
text, count = re.subn(r"^_drm_amdgpu_ver = '[0-9.]+'", f"_drm_amdgpu_ver = '{installed}'", path.read_text(), flags=re.M)
assert count == 1, "meson.build: the libdrm_amdgpu version line was not found"
path.write_text(text)

path = copy / "src/amd/common/ac_gpu_info.c"
text = path.read_text()
start = text.index("r = ac_drm_query_sw_info(dev, amdgpu_sw_info_address_prt_wa_control_bit")
end = text.index(";", start) + 1
path.write_text(text[:start] + "r = 0; /* PC test build: not asked; no GPU runs anything */" + text[end:])

path = copy / "src/amd/vulkan/radv_physical_device.c"
text = path.read_text()
if "PS5_CRUMB" in text:
    marker = '#include "git_sha1.h"\n'
    assert text.count(marker) == 1
    path.write_text(text.replace(marker, "#ifndef PS5_CRUMB\n#define PS5_CRUMB(text) do { } while (0)\n#endif\n" + marker))
PY

case $variant in
    asan) options=(-Db_ndebug=false -Db_sanitize=address) ;;
    plain) options=(-Db_ndebug=true) ;;
    *) echo "usage: build-host-driver.sh <Mesa tree> <work directory> [asan|plain]" >&2; exit 2 ;;
esac
if [[ ! -f $build/build.ninja ]]; then
    meson setup "$build" "$copy" -Dvulkan-drivers=amd -Dgallium-drivers= -Dplatforms= -Dllvm=disabled \
        -Damd-use-llvm=false -Dvideo-codecs= -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false \
        -Dgles1=disabled -Dgles2=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=disabled \
        -Dexpat=disabled -Dxmlconfig=disabled -Dshader-cache=disabled -Dbuild-tests=false -Dvulkan-layers= \
        -Dtools=drm-shim -Dmesa-clc=system -Dbuildtype=debugoptimized -Db_lundef=false "${options[@]}" \
        > "$build.setup.log" 2>&1 || { tail -20 "$build.setup.log" >&2; exit 1; }
fi
ninja -C "$build" src/amd/vulkan/libvulkan_radeon.so src/amd/drm-shim/libamdgpu_noop_drm_shim.so > "$build.log" 2>&1 ||
    { grep -E "error|FAILED" "$build.log" | head -20 >&2; exit 1; }
echo "$build"
