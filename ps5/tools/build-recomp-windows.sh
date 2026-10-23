#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - the recomp kit's Windows parts, built on Linux: PS2Recomp's converter and analyzer at the
# revision and with the patches recomp-game.py uses (static .exe files: nothing else to install on
# Windows), the patched tree whose runtime the modules are compiled with, and the disc reader's
# native helper (a DLL, and the same for Linux).
#
#   ps5/tools/build-recomp-windows.sh [out folder]     (default .deps/recomp-windows)
#
# Needs: sudo apt install g++-mingw-w64-x86-64-posix gcc-mingw-w64-x86-64-posix cmake ninja-build git clang
# make-recomp-kit.sh takes what this writes:
#   <out>/bin/ps2_recomp.exe, ps2_analyzer.exe      the converter and the analyzer
#   <out>/ps2recomp/                                PS2Recomp's tree as built (its sources, patched)
#   <out>/accel/crux2_disc_accel.dll, libcrux2_disc_accel.so
#   <out>/licenses/                                 the licences of what the .exe files hold
#   <out>/SOURCES.txt                               where each part's source is
set -euo pipefail
ps5=$(cd "$(dirname "$0")/.." && pwd)
root=$(dirname "$ps5")
out=${1:-$root/.deps/recomp-windows}
recomp_title=${CRUX2_RECOMP_DIR:-$root/../PS5_Crux2Recomp}
jobs=$(nproc 2>/dev/null || echo 4)

revision=$(sed -n 's/^PS2RECOMP_REVISION = "\(.*\)"/\1/p' "$ps5/recomp/tools/recomp-game.py")
url=$(sed -n 's/^PS2RECOMP_URL = "\(.*\)"/\1/p' "$ps5/recomp/tools/recomp-game.py")
[[ -n $revision && -n $url ]] || { echo "build-recomp-windows: no PS2Recomp revision in recomp-game.py" >&2; exit 1; }
patches=("$recomp_title/ps5/patches/ps2recomp/def-jam-vendetta-recomp.patch" "$ps5/recomp/patches/ps2recomp-crux2.patch"
	"$recomp_title/ps5/patches/ps2recomp/runner.patch")
for file in "${patches[@]}"; do
	[[ -f $file ]] || { echo "build-recomp-windows: missing $file" >&2; exit 1; }
done
for tool in x86_64-w64-mingw32-gcc-posix x86_64-w64-mingw32-g++-posix cmake ninja git clang; do
	command -v "$tool" >/dev/null || { echo "build-recomp-windows: missing $tool (see the top of this script)" >&2; exit 1; }
done

mkdir -p "$out"
src="$out/ps2recomp"
echo "==> PS2Recomp ${revision:0:7} with Crux2's patches"
if [[ ! -d $src/.git ]]; then
	rm -rf "$src"
	git -c core.autocrlf=false clone --quiet "${PS2RECOMP_MIRROR:-$url}" "$src"
fi
git -C "$src" cat-file -e "$revision^{commit}" 2>/dev/null || git -C "$src" fetch --quiet origin
git -C "$src" checkout --quiet --force "$revision"
git -C "$src" clean -fdxq
for patch in "${patches[@]}"; do
	git -C "$src" apply "$patch"
done

build="$out/build"
toolchain="$out/mingw-toolchain.cmake"
cat > "$toolchain" <<'EOF'
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc-posix)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++-posix)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
# (PS2Recomp turns on link-time optimisation: its archives need GCC's ar, which knows LTO objects.)
set(CMAKE_AR /usr/bin/x86_64-w64-mingw32-gcc-ar-posix CACHE FILEPATH "")
set(CMAKE_RANLIB /usr/bin/x86_64-w64-mingw32-gcc-ranlib-posix CACHE FILEPATH "")
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++ -Wl,--no-insert-timestamp")
EOF
echo "==> its converter and analyzer for Windows"
cmake -S "$src" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
	-DPS2X_BUILD_RUNTIME=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF -DBUILD_SHARED_LIBS=OFF \
	> "$out/configure.log" 2>&1 || { tail -40 "$out/configure.log" >&2; exit 1; }
cmake --build "$build" --target ps2_recomp ps2_analyzer -j "$jobs" > "$out/build.log" 2>&1 || { grep -m20 -B2 -A8 "error" "$out/build.log" >&2; exit 1; }
mkdir -p "$out/bin"
for exe in ps2_recomp ps2_analyzer; do
	path=$(find "$build" -name "$exe.exe" -type f | head -1)
	[[ -n $path ]] || { echo "build-recomp-windows: $exe.exe did not build" >&2; exit 1; }
	x86_64-w64-mingw32-strip -o "$out/bin/$exe.exe" "$path"
done
# Only Windows' own DLLs may be imported (the .exe files must run with nothing installed).
for exe in "$out/bin/"*.exe; do
	bad=$(x86_64-w64-mingw32-objdump -p "$exe" | sed -n 's/^\s*DLL Name: //p' | grep -viE '^(kernel32|msvcrt|ucrtbase|api-ms-win-.*|advapi32|user32|shell32|ws2_32|bcrypt|ntdll|ole32|shlwapi)\.dll$' || true)
	[[ -z $bad ]] || { echo "build-recomp-windows: $(basename "$exe") needs $bad" >&2; exit 1; }
done

echo "==> the disc reader's helper"
mkdir -p "$out/accel"
accel="$ps5/recomp/tools/accel/crux2_disc_accel.c"
x86_64-w64-mingw32-gcc-posix -O2 -shared -nostdlib -ffreestanding -Wall -Wextra -Werror -e DllMainCRTStartup \
	-Wl,--no-insert-timestamp -s -o "$out/accel/crux2_disc_accel.dll" "$accel"
clang -O2 -shared -fPIC -nostdlib -ffreestanding -fvisibility=hidden -Wall -Wextra -Werror -Wl,-z,defs -s \
	-o "$out/accel/libcrux2_disc_accel.so" "$accel"

echo "==> licences"
lic="$out/licenses"
rm -rf "$lic"
mkdir -p "$lic"
cp "$src/LICENSE" "$lic/PS2Recomp-GPL-3.0.txt"
deps="$build/_deps"
copy_licence() { # name folder-under-_deps files...
	local name=$1 dir=$2
	shift 2
	for f in "$@"; do
		if [[ -f "$deps/$dir/$f" ]]; then
			cp "$deps/$dir/$f" "$lic/$name.txt"
			return 0
		fi
	done
	echo "build-recomp-windows: no licence for $name in $deps/$dir" >&2
	return 1
}
copy_licence ELFIO-MIT elfio-src LICENSE.txt LICENSE
copy_licence toml11-MIT toml11-src LICENSE
copy_licence fmt-MIT fmt-src LICENSE LICENSE.rst
copy_licence libdwarf-LGPL-2.1 libdwarf-src src/lib/libdwarf/COPYING COPYING
copy_licence rabbitizer-MIT rabbitizer-src LICENSE
copy_licence nlohmann-json-MIT nlohmann_json-src LICENSE.MIT LICENSE
mingw_doc=/usr/share/doc
for pkg in mingw-w64-common gcc-mingw-w64-base libstdc++6; do
	[[ -f $mingw_doc/$pkg/copyright ]] && cp "$mingw_doc/$pkg/copyright" "$lic/$pkg-copyright.txt"
done

cat > "$out/SOURCES.txt" <<EOF
The recomp kit's Windows programs (recomp/bin/windows-x64) and where their source is:

ps2_recomp.exe, ps2_analyzer.exe
  PS2Recomp (ran-j and contributors, GPL-3.0): $url at $revision, with
  runner/def-jam-vendetta-recomp.patch (jcsix694, GPL-3.0), patches/ps2recomp-crux2.patch and
  runner/runner.patch (Press5elect, GPL-3.0-or-later) applied; the patched tree is recomp/ps2recomp.
  Built with mingw-w64's GCC (static: libstdc++ and libgcc under the GCC Runtime Library Exception,
  winpthreads, the mingw-w64 runtime), and these libraries PS2Recomp's build fetches:
$(for d in "$deps"/*-src; do n=$(basename "$d" -src); r=$(git -C "$d" describe --tags --always 2>/dev/null || echo "?"); u=$(git -C "$d" remote get-url origin 2>/dev/null || echo "?"); echo "    $n $r ($u)"; done)
  Their licences: recomp/bin/windows-x64/licenses. libdwarf is LGPL-2.1: its source is at the URL
  above, and the whole program's is here, so it can be built again with a changed libdwarf.

recomp/tools/accel/crux2_disc_accel.dll, libcrux2_disc_accel.so
  recomp/tools/accel/crux2_disc_accel.c (Press5elect, GPL-3.0-or-later), no library linked.
EOF
echo "==> $out"
ls -la "$out/bin" "$out/accel"
