#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - make the native modules for every game Crux2 has put on the share.
#
#   native-batch.sh <folder>                 each executable in <folder>/executables without an
#                                            up-to-date module gets one in <folder>/modules
#   native-batch.sh <folder> --watch         the same, then again every few minutes (Ctrl+C stops)
#
# <folder> is the share's Crux2-native folder as your PC sees it: Crux2 makes it on the share the
# games are on (or the first share network.cfg names), so with the share mounted at /mnt/ps2 it is
# /mnt/ps2/Crux2-native. On Linux Mint, mount the share once with:
#   sudo apt install cifs-utils
#   sudo mkdir -p /mnt/ps2
#   sudo mount -t cifs //192.168.1.10/Games/ps2 /mnt/ps2 -o guest,uid=$(id -u),gid=$(id -g)
# (or open it in the file manager and use the path under /run/user/<you>/gvfs/).
#
# Options:
#   --watch [SECONDS]  look again every SECONDS (default 300) after a pass
#   --ghidra DIR       Ghidra finds the functions (recomp-game.sh's --ghidra)
#   --jobs N           compilers at once (default: every processor)
#   --work DIR         recomp-game.sh's work folder (default ~/.cache/crux2-recomp)
#   --force            make every module again, up to date or not
#
# Crux2 writes an executable and then its note (<name>.txt: name, serial, CRC, title); one without
# its note is still being written and waits for the next pass. One that failed is not tried again
# until its executable or the kit changes (or with --force). A module is made when there is none,
# when the executable is newer than it, or when it was made by another kit (modules/<name>.kit).
# It is built here and moved into modules/ whole, so Crux2 never takes half of one. Each game's
# output goes to logs/<name>.log on the share. Crux2 copies a new module into the console's data
# folder the next time the game starts.
set -uo pipefail

here=$(cd "$(dirname "$0")" && pwd)
kit_version=$(cat "$here/../KIT_VERSION" 2>/dev/null || echo dev)

usage() { sed -n '5,30p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

folder=""
watch=0
interval=300
force=0
pass_args=()
while [[ $# -gt 0 ]]; do
	case "$1" in
		--watch)
			watch=1
			if [[ ${2:-} =~ ^[0-9]+$ ]]; then interval=$2; shift; fi
			shift ;;
		--ghidra|--jobs|--work) pass_args+=("$1" "$2"); shift 2 ;;
		--force) force=1; shift ;;
		-h|--help) usage 0 ;;
		-*) echo "native-batch: unknown option $1" >&2; usage 1 ;;
		*) folder=$1; shift ;;
	esac
done
[[ -n "$folder" ]] || usage 1
[[ -d "$folder" ]] || { echo "native-batch: $folder is not a folder (is the share mounted?)" >&2; exit 1; }
folder=$(cd "$folder" && pwd)
mkdir -p "$folder/executables" "$folder/modules" "$folder/logs"

needs_module() { # name
	local name=$1 exe="$folder/executables/$1" module="$folder/modules/$1.crux2native" stamp="$folder/modules/$1.kit"
	local failed="$folder/logs/$1.failed"
	[[ $force == 1 ]] && return 0
	# (Failed with this kit and this executable: not tried again every pass; --force does.)
	if [[ -f "$failed" && ! "$exe" -nt "$failed" && "$(cat "$failed" 2>/dev/null)" == "$kit_version" ]]; then
		skipped=$((skipped + 1))
		return 1
	fi
	[[ -f "$module" ]] || return 0
	[[ "$exe" -nt "$module" ]] && return 0
	[[ "$(cat "$stamp" 2>/dev/null)" == "$kit_version" ]] || return 0
	return 1
}

one_pass() {
	local made=0 failed=0 waiting=0 name exe title out log
	skipped=0
	shopt -s nullglob
	for exe in "$folder/executables"/*; do
		name=$(basename "$exe")
		case "$name" in *.txt|*.partial|.*) continue ;; esac
		[[ -f "$exe" ]] || continue
		if [[ ! -f "$exe.txt" ]]; then
			waiting=$((waiting + 1))
			continue
		fi
		needs_module "$name" || continue
		title=$(cut -f4 "$exe.txt" 2>/dev/null | head -1)
		log="$folder/logs/$name.log"
		out=$(mktemp -d "${TMPDIR:-/tmp}/crux2-native.XXXXXX")
		echo "==> $name${title:+ ($title)}: making its module (log: $log)"
		if "$here/recomp-game.sh" "$exe" --for crux2 --out "$out" "${pass_args[@]}" > "$log" 2>&1; then
			# Into modules/ whole: copied beside it under another name, then renamed.
			if cp "$out/$name.crux2native" "$folder/modules/.$name.crux2native.partial" &&
				mv -f "$folder/modules/.$name.crux2native.partial" "$folder/modules/$name.crux2native"; then
				echo "$kit_version" > "$folder/modules/$name.kit"
				rm -f "$folder/logs/$name.failed"
				echo "    done: modules/$name.crux2native ($(du -h "$folder/modules/$name.crux2native" | cut -f1))"
				made=$((made + 1))
			else
				echo "    the module could not be written to the share" >&2
				failed=$((failed + 1))
			fi
		else
			echo "    failed; the end of its log:" >&2
			tail -15 "$log" | sed 's/^/      /' >&2
			echo "$kit_version" > "$folder/logs/$name.failed"
			failed=$((failed + 1))
		fi
		rm -rf "$out"
	done
	echo "native-batch ($kit_version): $made made, $failed failed$([[ $skipped -gt 0 ]] && echo ", $skipped failed before (--force tries again)")$([[ $waiting -gt 0 ]] && echo ", $waiting still being written by Crux2")"
	[[ $failed == 0 ]]
}

if [[ $watch == 0 ]]; then
	one_pass
	exit $?
fi
echo "native-batch: watching $folder/executables every ${interval}s (Ctrl+C stops)"
while true; do
	one_pass
	sleep "$interval"
done
