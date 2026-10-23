#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - the recomp kit: what a player's PC needs to make a game's modules for Crux2 (native code)
# and for Crux2 Recomp, from the player's own copy of the game. No game code is in it.
#
#   ps5/tools/make-recomp-kit.sh <version> [out folder]     -> <out>/Crux2-recomp-kit-<version>.zip
#
# It takes ps5/recomp/ (the tools, Crux2's module runtime, the converter's patch), Crux2 Recomp's
# runner files (../PS5_Crux2Recomp: its runtime patch, its settings header, its module builder) and
# the console's headers the modules are compiled against (the payload SDK's target/include: the
# platform's C library headers and LLVM's libc++), with their licences.
set -euo pipefail
version=${1:?version, e.g. v1.3.0}
ps5=$(cd "$(dirname "$0")/.." && pwd)
root=$(dirname "$ps5")
out=${2:-$root/dist}
recomp_title=${CRUX2_RECOMP_DIR:-$root/../PS5_Crux2Recomp}
sdk="$root/.deps/native/ps5-payload-sdk"
name="Crux2-recomp-kit-$version"
stage="$out/$name"

for file in "$recomp_title/ps5/patches/ps2recomp/runner.patch" "$recomp_title/ps5/patches/ps2recomp/def-jam-vendetta-recomp.patch" \
		"$recomp_title/ps5/runner/tools/vu1_recomp.py" "$recomp_title/ps5/runner/crux2_recomp_config.h" \
		"$recomp_title/ps5/runner/tools/build-runner-module.py" "$sdk/target/include/c++/v1" "$root/../PS5_PayloadSDK/LICENSE"; do
	[[ -e $file ]] || { echo "make-recomp-kit: missing $file" >&2; exit 1; }
done

rm -rf "$stage"
mkdir -p "$stage/recomp"
cp -r "$ps5/recomp/tools" "$ps5/recomp/runtime" "$ps5/recomp/patches" "$stage/recomp/"
mkdir -p "$stage/recomp/tests"
cp -r "$ps5/recomp/tests/program" "$ps5/recomp/tests/native_test.cpp" "$ps5/recomp/tests/run-native-tests.sh" "$stage/recomp/tests/"
find "$stage" -name __pycache__ -prune -exec rm -rf {} +

runner="$stage/recomp/runner"
mkdir -p "$runner/sdk/target" "$runner/licenses"
cp "$recomp_title/ps5/patches/ps2recomp/runner.patch" "$recomp_title/ps5/patches/ps2recomp/def-jam-vendetta-recomp.patch" \
	"$recomp_title/ps5/runner/crux2_recomp_config.h" "$recomp_title/ps5/runner/tools/build-runner-module.py" \
	"$recomp_title/ps5/runner/tools/vu1_recomp.py" "$runner/"
cp -r "$sdk/target/include" "$runner/sdk/target/"
cp "$root/../PS5_PayloadSDK/LICENSE" "$runner/licenses/payload-sdk-GPL-3.0.txt"
# (libc++'s licence at the SDK's LLVM release, 18.1.3: ps5/recomp/licenses/.)
cp "$ps5/recomp/licenses/llvm-libcxx-LICENSE.txt" "$runner/licenses/llvm-LICENSE.txt"
cp "$root/NOTICE" "$root/LICENSE" "$stage/"
chmod +x "$stage/recomp/tools/"*.sh "$stage/recomp/tools/"*.py "$runner/build-runner-module.py" "$runner/vu1_recomp.py"

echo "$version" > "$stage/recomp/KIT_VERSION"

cat > "$stage/README.txt" <<EOF
Crux2 recomp kit $version - by Press5elect
==========================================

Makes a PS2 game's modules on your PC (Linux Mint or Ubuntu), from your own copy of the game's
executable (the file BOOT2 names in the disc's SYSTEM.CNF, SLUS_213.76 say):

  sudo apt install git cmake ninja-build clang lld python3
  recomp/tools/recomp-game.sh SLUS_213.76 --for both

  SLUS_213.76.crux2native   for Crux2:        copy to /data/crux2/native/
  SLUS_213.76.crux2recomp   for Crux2 Recomp: copy to /data/crux2recomp/<a folder>/ with the
                            executable and the disc (.iso, or its files in a folder named disc)

Or let Crux2 hand you the executables (Crux2 v1.3.7 or newer): a game started without a module puts its
executable, read from the disc image, in Crux2-native/executables on its network share. With the share
mounted on the PC (at /mnt/ps2, say):

  recomp/tools/native-batch.sh /mnt/ps2/Crux2-native            every game waiting there, once
  recomp/tools/native-batch.sh /mnt/ps2/Crux2-native --watch    and again every 5 minutes

The modules go to Crux2-native/modules; the game's next start in Crux2 takes its new module by itself.
recomp/tools/native-batch.sh --help says how to mount the share.

The first run fetches PS2Recomp (ran-j/PS2Recomp, GPL-3.0) and builds its converter; a large game
takes a long while to compile (every function, for the console's processor). Later runs only
redo what changed. --ghidra DIR uses Ghidra to find the functions (better on retail games).

The modules are the game's code: keep them to yourself, as you would the disc image. This kit
holds no game code, BIOS, firmware or keys.

Licences: the kit's tools are GPL-3.0-or-later with the attribution term in NOTICE (keep "Crux2
by Press5elect"). recomp/runner/sdk/ holds the console's headers from the PS5 payload SDK
(GPL-3.0-or-later, licenses/payload-sdk-GPL-3.0.txt) with LLVM's libc++ headers (Apache-2.0 WITH
LLVM-exception, licenses/llvm-LICENSE.txt). recomp/runtime/crux2_ee_interp.h holds PCSX2's
interpreter code for the MMI, FPU and SA instructions (GPL-3.0-or-later, the PCSX2 Dev Team), made
by recomp/tools/make-ee-interp.py. PS2Recomp is fetched from its own repository.
recomp/runner/def-jam-vendetta-recomp.patch (PS2Recomp's converter and runtime) and
recomp/runner/vu1_recomp.py (the VU1 recompiler) are def-jam-vendetta-recomp's work, by jcsix694
(https://github.com/jcsix694/def-jam-vendetta-recomp), GPL-3.0; vu1_recomp.py with Crux2's changes.
EOF

rm -f "$out/$name.zip"
python3 - "$stage" "$out/$name.zip" <<'EOF'
import os, sys, zipfile
stage, target = sys.argv[1:3]
base = os.path.dirname(stage)
with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for folder, dirs, files in os.walk(stage):
        dirs.sort()
        rel = os.path.relpath(folder, base)
        z.write(folder, rel + "/")
        for f in sorted(files):
            path = os.path.join(folder, f)
            info = zipfile.ZipInfo.from_file(path, os.path.relpath(path, base))
            info.compress_type = zipfile.ZIP_DEFLATED
            with open(path, "rb") as fh:
                z.writestr(info, fh.read())
EOF
echo "==> $out/$name.zip ($(du -h "$out/$name.zip" | cut -f1))"
