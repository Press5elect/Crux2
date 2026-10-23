#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - the recomp kit: what a player's PC (Windows or Linux) needs to make a game's modules for
# Crux2 Recomp and for Crux2 (native code), from the player's own copy of the game: a disc image
# (.chd, .iso, .cue/.bin, .cso) or its executable. No game code is in it.
#
#   ps5/tools/make-recomp-kit.sh <version> [out folder]     -> <out>/Crux2-recomp-kit-<version>.zip
#
# It takes ps5/recomp/ (the tools, the disc reader, Crux2's module runtime, the converter's patch),
# Crux2 Recomp's runner files (../PS5_Crux2Recomp: its runtime patch, its settings header, its
# module builder), the console's headers the modules are compiled against (the payload SDK's
# target/include: the platform's C library headers and LLVM's libc++), with their licences, and the
# Windows parts build-recomp-windows.sh makes (PS2Recomp's converter as .exe files, the patched
# PS2Recomp tree, the disc reader's helper); it runs that script when they are not there.
set -euo pipefail
version=${1:?version, e.g. v1.1.0}
ps5=$(cd "$(dirname "$0")/.." && pwd)
root=$(dirname "$ps5")
out=${2:-$root/dist}
recomp_title=${CRUX2_RECOMP_DIR:-$root/../PS5_Crux2Recomp}
sdk="$root/.deps/native/ps5-payload-sdk"
win=${CRUX2_RECOMP_WINDOWS:-$root/.deps/recomp-windows}
name="Crux2-recomp-kit-$version"
stage="$out/$name"

for file in "$recomp_title/ps5/patches/ps2recomp/runner.patch" "$recomp_title/ps5/patches/ps2recomp/def-jam-vendetta-recomp.patch" \
		"$recomp_title/ps5/runner/tools/vu1_recomp.py" "$recomp_title/ps5/runner/crux2_recomp_config.h" \
		"$recomp_title/ps5/runner/tools/build-runner-module.py" "$sdk/target/include/c++/v1" "$root/../PS5_PayloadSDK/LICENSE"; do
	[[ -e $file ]] || { echo "make-recomp-kit: missing $file" >&2; exit 1; }
done

# The Windows parts, made again when the converter's revision or a patch is newer than they are.
need_win=0
for f in "$win/bin/ps2_recomp.exe" "$win/bin/ps2_analyzer.exe" "$win/accel/crux2_disc_accel.dll" "$win/accel/libcrux2_disc_accel.so" "$win/SOURCES.txt"; do
	[[ -f $f ]] || need_win=1
done
if [[ $need_win == 0 ]]; then
	for f in "$ps5/recomp/tools/recomp-game.py" "$ps5/recomp/patches/ps2recomp-crux2.patch" "$ps5/recomp/tools/accel/crux2_disc_accel.c" \
			"$recomp_title/ps5/patches/ps2recomp/runner.patch" "$recomp_title/ps5/patches/ps2recomp/def-jam-vendetta-recomp.patch"; do
		[[ $f -nt "$win/SOURCES.txt" ]] && need_win=1
	done
fi
if [[ $need_win == 1 ]]; then
	"$ps5/tools/build-recomp-windows.sh" "$win"
fi

rm -rf "$stage"
mkdir -p "$stage/recomp"
cp -r "$ps5/recomp/tools" "$ps5/recomp/runtime" "$ps5/recomp/patches" "$stage/recomp/"
mkdir -p "$stage/recomp/tests"
cp -r "$ps5/recomp/tests/program" "$ps5/recomp/tests/native_test.cpp" "$ps5/recomp/tests/run-native-tests.sh" "$ps5/recomp/tests/disc" "$stage/recomp/tests/"
find "$stage" -name __pycache__ -prune -exec rm -rf {} +
rm -f "$stage/recomp/tools/accel/"*.so "$stage/recomp/tools/accel/"*.dll
cp "$win/accel/crux2_disc_accel.dll" "$win/accel/libcrux2_disc_accel.so" "$stage/recomp/tools/accel/"

runner="$stage/recomp/runner"
mkdir -p "$runner/sdk/target" "$runner/licenses"
cp "$recomp_title/ps5/patches/ps2recomp/runner.patch" "$recomp_title/ps5/patches/ps2recomp/def-jam-vendetta-recomp.patch" \
	"$recomp_title/ps5/runner/crux2_recomp_config.h" "$recomp_title/ps5/runner/tools/build-runner-module.py" \
	"$recomp_title/ps5/runner/tools/vu1_recomp.py" "$runner/"
cp -r "$sdk/target/include" "$runner/sdk/target/"
cp "$root/../PS5_PayloadSDK/LICENSE" "$runner/licenses/payload-sdk-GPL-3.0.txt"
# (libc++'s licence at the SDK's LLVM release, 18.1.3: ps5/recomp/licenses/.)
cp "$ps5/recomp/licenses/llvm-libcxx-LICENSE.txt" "$runner/licenses/llvm-LICENSE.txt"

# Windows: the converter, and PS2Recomp's patched tree (the runtime the modules are compiled with,
# and the source of the .exe files).
mkdir -p "$stage/recomp/bin/windows-x64"
cp "$win/bin/ps2_recomp.exe" "$win/bin/ps2_analyzer.exe" "$win/SOURCES.txt" "$stage/recomp/bin/windows-x64/"
cp -r "$win/licenses" "$stage/recomp/bin/windows-x64/licenses"
mkdir -p "$stage/recomp/ps2recomp"
(cd "$win/ps2recomp" && git ls-files -z && git ls-files -z --others --exclude-standard) |
	(cd "$win/ps2recomp" && xargs -0 cp --parents -t "$stage/recomp/ps2recomp/")
cp "$root/NOTICE" "$root/LICENSE" "$stage/"
chmod +x "$stage/recomp/tools/"*.sh "$stage/recomp/tools/"*.py "$runner/build-runner-module.py" "$runner/vu1_recomp.py"

echo "$version" > "$stage/recomp/KIT_VERSION"

# Windows: a game dropped on this file (or several, or a folder of disc images) becomes a folder
# for Crux2 Recomp in the kit's output folder.
printf '%s\r\n' \
	'@echo off' \
	'rem Crux2 recomp kit - by Press5elect. Drop a PS2 disc image (.chd, .iso, .cue, .cso), its executable,' \
	'rem or a folder of disc images on this file: each becomes a folder for Crux2 Recomp in "output".' \
	'setlocal' \
	'set "KIT=%~dp0"' \
	'cd /d "%KIT%"' \
	'set "PY="' \
	'where py >nul 2>nul && set "PY=py -3"' \
	'if not defined PY where python >nul 2>nul && set "PY=python"' \
	'if not defined PY (' \
	'  echo Python 3 is not installed. Get it from https://www.python.org/downloads/ and tick "Add python.exe to PATH".' \
	'  pause' \
	'  exit /b 1' \
	')' \
	'if "%~1"=="" (' \
	'  echo Drop a disc image ^(.chd, .iso, .cue, .cso^), a game'"'"'s executable or a folder of disc images on this file.' \
	'  echo Or from a command prompt: %PY% recomp\tools\recomp-game.py --help' \
	'  pause' \
	'  exit /b 1' \
	')' \
	'%PY% "%KIT%recomp\tools\recomp-game.py" --for recomp --out "%KIT%output" %*' \
	'echo.' \
	'if errorlevel 1 (echo Not everything was made: the lines above say why.) else (echo The folders are in %KIT%output)' \
	'pause' > "$stage/Crux2 Recomp.cmd"

cat > "$stage/README.txt" <<EOF
Crux2 recomp kit $version - by Press5elect
==========================================

Makes a PS2 game's modules on your PC from your own copy of the game: its disc image (.chd, .iso,
.cue with its .bin, .cso), a folder of the disc's files, or its executable (the file BOOT2 names in
the disc's SYSTEM.CNF). From a disc image the executable is read out by itself.

Windows (Crux2 Recomp)
----------------------
1. Install Python 3 (https://www.python.org/downloads/, tick "Add python.exe to PATH") and LLVM 18
   or newer (LLVM-<version>-win64.msi from https://github.com/llvm/llvm-project/releases).
2. Drop the game's .chd (or .iso, .cue, .cso; several, or a folder of them) on "Crux2 Recomp.cmd".
3. When it is done, output\\<executable>\\ holds the executable, its .crux2recomp module and the disc
   as an .iso. Copy that folder into /data/crux2recomp/ on the console (FTP or a USB drive) and
   start Crux2 Recomp: the game is listed by name.

From a command prompt, recomp\\tools\\recomp-game.py --help lists the options (--ghidra, --jobs,
--iso no, ...). The work folder is %LOCALAPPDATA%\\Crux2\\recomp; a second run only redoes what changed.
PS2Recomp's converter comes built for Windows in recomp\\bin\\windows-x64. Crux2's native modules
(--for crux2) are made on Linux for now.

Linux (Mint or Ubuntu)
----------------------
  sudo apt install git cmake ninja-build clang lld python3
  recomp/tools/recomp-game.sh GAME.chd --for recomp     a folder for Crux2 Recomp, as on Windows
  recomp/tools/recomp-game.sh GAME.chd --for both       and Crux2's native module

  SLUS_213.76.crux2native   for Crux2:        copy to /data/crux2/native/
  <executable>/             for Crux2 Recomp: copy into /data/crux2recomp/

Or let Crux2 hand you the executables (Crux2 v1.3.7 or newer): a game started without a module puts its
executable, read from the disc image, in Crux2-native/executables on its network share. With the share
mounted on the PC (at /mnt/ps2, say):

  recomp/tools/native-batch.sh /mnt/ps2/Crux2-native            every game waiting there, once
  recomp/tools/native-batch.sh /mnt/ps2/Crux2-native --watch    and again every 5 minutes

The modules go to Crux2-native/modules; the game's next start in Crux2 takes its new module by itself.
recomp/tools/native-batch.sh --help says how to mount the share.

On Linux the first run fetches PS2Recomp (ran-j/PS2Recomp, GPL-3.0) and builds its converter. A
large game takes a long while to compile (every function, for the console's processor); later runs
only redo what changed. --ghidra DIR uses Ghidra to find the functions (better on retail games).

Disc images
-----------
recomp/tools/ps2disc.py reads them with Python alone: CHD version 5 (chdman createdvd and createcd,
every codec they use; zstd, used only when asked for with -c zstd, needs Python 3.14 or
\`pip install zstandard\`), .iso, .cue/.bin, .cso. "ps2disc.py info GAME.chd" says what a disc is;
"ps2disc.py iso GAME.chd GAME.iso" writes the disc out as an .iso by itself.

The modules, the executables and the .iso files are the game: keep them to yourself, as you would
the disc. This kit holds no game code, BIOS, firmware or keys.

Licences
--------
The kit's tools are GPL-3.0-or-later with the attribution term in NOTICE (keep "Crux2 by
Press5elect"). recomp/runner/sdk/ holds the console's headers from the PS5 payload SDK
(GPL-3.0-or-later, licenses/payload-sdk-GPL-3.0.txt) with LLVM's libc++ headers (Apache-2.0 WITH
LLVM-exception, licenses/llvm-LICENSE.txt). recomp/runtime/crux2_ee_interp.h holds PCSX2's
interpreter code for the MMI, FPU and SA instructions (GPL-3.0-or-later, the PCSX2 Dev Team), made
by recomp/tools/make-ee-interp.py. recomp/ps2recomp/ is PS2Recomp (GPL-3.0) at the revision the kit
uses, with the patches below applied; recomp/bin/windows-x64/ is its converter built for Windows
(its SOURCES.txt and licenses/ say what else is in the .exe files and where their source is).
recomp/runner/def-jam-vendetta-recomp.patch (PS2Recomp's converter and runtime) and
recomp/runner/vu1_recomp.py (the VU1 recompiler) are def-jam-vendetta-recomp's work, by jcsix694
(https://github.com/jcsix694/def-jam-vendetta-recomp), GPL-3.0; vu1_recomp.py with Crux2's changes.
The CHD reader follows MAME's format as libchdr reads it (BSD-3-Clause); none of their code is in it.
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
