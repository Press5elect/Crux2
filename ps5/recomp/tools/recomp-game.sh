#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - make a game's modules on your PC, from your own copy of the game's executable.
#
#   recomp-game.sh SLUS_213.76                    a native module for Crux2 (SLUS_213.76.crux2native)
#   recomp-game.sh SLUS_213.76 --for recomp       a module for Crux2 Recomp (SLUS_213.76.crux2recomp)
#   recomp-game.sh SLUS_213.76 --for both         both
#
# Options:
#   --for WHAT     crux2 (default), recomp or both
#   --ghidra DIR   Ghidra (installed in DIR) finds the functions: slow, better on retail games. Without
#                  it PS2Recomp's analyzer finds them (quick; fine for a try). Set GHIDRA_PROCESSOR to
#                  the language to import with (an Emotion Engine extension's, say) if you have one.
#   --work DIR     where everything is made (default ~/.cache/crux2-recomp); kept between runs, so a
#                  second run only redoes what changed
#   --out DIR      where the modules go (default: here)
#   --jobs N       compilers at once (default: every processor)
#
# It needs, on Linux Mint or Ubuntu:
#   sudo apt install git cmake ninja-build clang lld python3
# (clang 18 or newer for --for recomp; for --ghidra, a Java runtime Ghidra accepts: openjdk-21-jdk).
#
# What it does: fetches PS2Recomp (ran-j/PS2Recomp, GPL-3.0) at the revision Crux2 was tested with,
# applies def-jam-vendetta-recomp's work on PS2Recomp (jcsix694, GPL-3.0: the converter's EE and VU0
# fixes, and the runtime Crux2 Recomp runs: ../runner/def-jam-vendetta-recomp.patch) and Crux2's patches
# (../patches/ps2recomp-crux2.patch: its Crux2 mode; ../runner/runner.patch: its runtime as Crux2
# Recomp builds it), builds its converter, finds the executable's functions,
# converts them to C++ and builds the modules. Then, on the console:
#   Crux2:        /data/crux2/native/SLUS_213.76.crux2native
#   Crux2 Recomp: /data/crux2recomp/<a folder>/ with SLUS_213.76, SLUS_213.76.crux2recomp and the
#                 disc (its .iso, or its files in a folder named disc)
#
# The executable is on the game's disc (the name in its SYSTEM.CNF, BOOT2 = cdrom0:\SLUS_213.76;1).
# The modules are the game's code: keep them to yourself, as you would the disc image.
set -euo pipefail

PS2RECOMP_URL=https://github.com/ran-j/PS2Recomp.git
PS2RECOMP_REVISION=eb8688b2fd191129f103a39543002c005b33409c

here=$(cd "$(dirname "$0")" && pwd)
recomp_dir=$(cd "$here/.." && pwd)

usage() { sed -n '5,22p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

elf=""
what=crux2
work="${HOME}/.cache/crux2-recomp"
out_dir="$PWD"
jobs=$(nproc 2>/dev/null || echo 4)
ghidra=""
while [[ $# -gt 0 ]]; do
	case "$1" in
		--for) what=$2; shift 2 ;;
		--work) work=$2; shift 2 ;;
		--out) out_dir=$2; shift 2 ;;
		--jobs) jobs=$2; shift 2 ;;
		--ghidra) ghidra=$2; shift 2 ;;
		-h|--help) usage 0 ;;
		-*) echo "unknown option $1" >&2; usage 1 ;;
		*) elf=$1; shift ;;
	esac
done
[[ -n "$elf" ]] || usage 1
case "$what" in crux2|recomp|both) ;; *) echo "recomp-game: --for crux2, recomp or both" >&2; exit 1 ;; esac
[[ -f "$elf" ]] || { echo "recomp-game: $elf is not a file" >&2; exit 1; }
if [[ "$(head -c 4 "$elf" | od -An -tx1 | tr -d ' \n')" != "7f454c46" ]]; then
	echo "recomp-game: $elf is not an ELF executable (copy the file BOOT2 names in the disc's SYSTEM.CNF)" >&2
	exit 1
fi
elf=$(cd "$(dirname "$elf")" && pwd)/$(basename "$elf")
name=$(basename "$elf")
mkdir -p "$out_dir"
out_dir=$(cd "$out_dir" && pwd)

# ---- the tools
missing=()
for tool in git cmake ninja clang clang++ ld.lld python3; do
	command -v "$tool" >/dev/null || missing+=("$tool")
done
if [[ ${#missing[@]} -gt 0 ]]; then
	echo "recomp-game: missing ${missing[*]}. On Linux Mint or Ubuntu: sudo apt install git cmake ninja-build clang lld python3" >&2
	exit 1
fi
if [[ -n "$ghidra" && ! -x "$ghidra/support/analyzeHeadless" ]]; then
	echo "recomp-game: no Ghidra at $ghidra (looked for $ghidra/support/analyzeHeadless)" >&2
	exit 1
fi
runner="$recomp_dir/runner"
if [[ $what != crux2 ]]; then
	for file in runner.patch def-jam-vendetta-recomp.patch crux2_recomp_config.h build-runner-module.py sdk/target/include/c++/v1; do
		[[ -e "$runner/$file" ]] || { echo "recomp-game: $runner/$file is missing (Crux2's recomp kit has it)" >&2; exit 1; }
	done
	major=$(clang --version | sed -n 's/.*clang version \([0-9]*\).*/\1/p' | head -1)
	if [[ -z "$major" || "$major" -lt 18 ]]; then
		echo "recomp-game: --for recomp needs clang 18 or newer (this one is ${major:-unknown}): sudo apt install clang-18 lld-18" >&2
		exit 1
	fi
fi

mkdir -p "$work"
echo "==> work folder: $work"

# ---- PS2Recomp with Crux2's patches, built once
src="$work/PS2Recomp"
patches=()
[[ -f "$runner/def-jam-vendetta-recomp.patch" ]] && patches+=("$runner/def-jam-vendetta-recomp.patch")
patches+=("$recomp_dir/patches/ps2recomp-crux2.patch")
[[ -f "$runner/runner.patch" ]] && patches+=("$runner/runner.patch")
stamp="$work/.converter-$PS2RECOMP_REVISION-$(cat "${patches[@]}" | sha256sum | cut -c1-12)"
if [[ ! -f "$stamp" ]]; then
	echo "==> PS2Recomp $PS2RECOMP_REVISION, with Crux2's patches"
	if [[ ! -d "$src/.git" ]]; then
		git clone --quiet "${PS2RECOMP_MIRROR:-$PS2RECOMP_URL}" "$src"
		git -C "$src" remote set-url origin "$PS2RECOMP_URL"
	fi
	git -C "$src" cat-file -e "$PS2RECOMP_REVISION^{commit}" 2>/dev/null || git -C "$src" fetch --quiet origin
	git -C "$src" checkout --quiet --force "$PS2RECOMP_REVISION"
	git -C "$src" clean -fdq -e out
	for patch in "${patches[@]}"; do
		git -C "$src" apply "$patch"
	done
	echo "==> building its converter (a few minutes, once)"
	cmake -S "$src" -B "$src/out" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
		-DPS2X_BUILD_RUNTIME=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF >/dev/null
	cmake --build "$src/out" --target ps2_recomp ps2_analyzer -j "$jobs"
	rm -f "$work"/.converter-*
	touch "$stamp"
fi
converter=$(find "$src/out" -type f -name ps2_recomp -perm -u+x | head -1)
analyzer=$(find "$src/out" -type f -name ps2_analyzer -perm -u+x | head -1)
[[ -x "$converter" && -x "$analyzer" ]] || { echo "recomp-game: the converter did not build" >&2; exit 1; }

# ---- the game's functions
game="$work/$name"
mkdir -p "$game"
found="$game/functions.toml"
if [[ -n "$ghidra" ]]; then
	echo "==> Ghidra: analysing $name (this can take an hour on a large game)"
	processor=()
	[[ -n "${GHIDRA_PROCESSOR:-}" ]] && processor=(-processor "$GHIDRA_PROCESSOR")
	rm -rf "$game/ghidra"
	mkdir -p "$game/ghidra"
	"$ghidra/support/analyzeHeadless" "$game/ghidra" crux2 -import "$elf" "${processor[@]}" \
		-scriptPath "$src/ps2xRecomp/tools/ghidra" -postScript ExportPS2Functions.java "$found" "$game/functions.csv" \
		-deleteProject > "$game/ghidra.log" 2>&1 || { tail -30 "$game/ghidra.log" >&2; echo "recomp-game: Ghidra failed (its log: $game/ghidra.log)" >&2; exit 1; }
	[[ -f "$found" ]] || { echo "recomp-game: Ghidra wrote no function list (its log: $game/ghidra.log)" >&2; exit 1; }
else
	echo "==> the analyzer: finding $name's functions"
	"$analyzer" "$elf" "$found" > "$game/analyzer.log" 2>&1 || { tail -30 "$game/analyzer.log" >&2; exit 1; }
fi

# A configuration for one kind of output: the executable from here, the C++ into <dir>.new/; in
# Crux2 mode every function is kept (no stubs, nothing skipped: Crux2 runs the rest itself).
configure() { # configure TOML OUTPUT_DIR crux2|recomp
	python3 - "$found" "$1" "$elf" "$2.new" "$3" <<'EOF'
import re, sys
found, path, elf, out, mode = sys.argv[1:6]
text = open(found, encoding="utf-8").read()
def setting(text, key, value):
    pattern = re.compile(rf'^{key}\s*=\s*"[^"]*"', re.M)
    line = f'{key} = "{value}"'
    return pattern.sub(line, text, count=1) if pattern.search(text) else text.replace("[general]", f"[general]\n{line}", 1)
text = setting(text, "input", elf.replace("\\", "/"))
text = setting(text, "output", out.replace("\\", "/") + "/")
if mode == "crux2":
    for key in ("stubs", "skip"):
        text = re.sub(rf'^{key}\s*=\s*\[.*?\]', f'{key} = []', text, flags=re.M | re.S)
open(path, "w", encoding="utf-8").write(text)
EOF
}

# Converts into DIR, keeping files that came out the same (and so their compiled objects).
convert() { # convert TOML DIR LABEL ENV...
	local toml=$1 dir=$2 label=$3
	shift 3
	echo "==> converting $name to C++ ($label)"
	rm -rf "$dir.new"
	(cd "$game" && env "$@" "$converter" "$toml" > "$dir.log" 2>&1) ||
		{ tail -30 "$dir.log" >&2; echo "recomp-game: the converter failed (its log: $dir.log)" >&2; exit 1; }
	mkdir -p "$dir"
	for f in "$dir.new"/*; do
		cmp -s "$f" "$dir/$(basename "$f")" || cp "$f" "$dir/$(basename "$f")"
	done
	for f in "$dir"/*.cpp "$dir"/*.h; do
		[[ -e "$f" ]] || continue
		local base
		base=$(basename "$f")
		case "$base" in crux2_module_info.cpp|crux2_recomp_module.cpp|vu1_compiled.cpp) continue ;; esac
		[[ -e "$dir.new/$base" ]] || rm -f "$f"
	done
	rm -rf "$dir.new"
	echo "    $(ls "$dir"/*.cpp | wc -l) files"
}

made=()
if [[ $what == crux2 || $what == both ]]; then
	configure "$game/crux2.toml" "$game/generated" crux2
	convert "$game/crux2.toml" "$game/generated" "Crux2 mode" PS2RECOMP_CRUX2=1
	echo "==> building the native module for Crux2 (the longest part: every function is compiled for the console's processor)"
	python3 "$here/build-module.py" --elf "$elf" --generated "$game/generated" --out "$out_dir/$name.crux2native" --jobs "$jobs" \
		--made-by "PS2Recomp ${PS2RECOMP_REVISION:0:7}, Crux2 mode"
	made+=("$out_dir/$name.crux2native  ->  Crux2: /data/crux2/native/$name.crux2native")
fi
if [[ $what == recomp || $what == both ]]; then
	configure "$game/recomp.toml" "$game/generated-recomp" recomp
	convert "$game/recomp.toml" "$game/generated-recomp" "PS2Recomp's own mode, for Crux2 Recomp" PS2RECOMP_CRUX2=
	if [[ -f "$runner/vu1_recomp.py" ]]; then
		echo "==> the game's VU1 microprograms to C++ (def-jam-vendetta-recomp's VU1 recompiler)"
		python3 "$runner/vu1_recomp.py" "$elf" "$game/generated-recomp/vu1_compiled.cpp"
	fi
	echo "==> building the module for Crux2 Recomp"
	python3 "$runner/build-runner-module.py" --generated "$game/generated-recomp" --out "$out_dir/$name.crux2recomp" \
		--sdk "$runner/sdk" --runtime "$src" --config "$runner/crux2_recomp_config.h" --jobs "$jobs"
	made+=("$out_dir/$name.crux2recomp  ->  Crux2 Recomp: /data/crux2recomp/<a folder>/, with $name and the disc")
fi

echo
echo "Done:"
for line in "${made[@]}"; do
	echo "  $line"
done
echo "Copy them to the console (FTP, or a USB drive). Crux2's log says 'native: $name.crux2native loaded' when the"
echo "game starts; Crux2 Recomp lists the game by name."
