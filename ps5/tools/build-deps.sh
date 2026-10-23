#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - the third-party libraries the title links, for the console.
#
#   ps5/tools/build-deps.sh          fetch and build what is missing into .deps/ports
#
# Where they come from:
#   - the ps5-payload-dev "pacbrew" ports release (prebuilt static libraries for
#     the console): zlib, zstd, libjpeg-turbo, libwebp, curl with OpenSSL,
#     libsmb2. Only what the title uses is copied out of it, so its other
#     headers (fmt, imgui...) cannot shadow PCSX2's own.
#   - built here with the SDK's compilers, at the versions PCSX2's own
#     dependency script pins (.github/workflows/scripts/linux): libpng with the
#     APNG patch, LZ4, FreeType (with Brotli), PlutoVG, PlutoSVG, RapidYAML, and
#     shaderc with glslang and SPIRV-Tools. libzip, SoundTouch, cpuinfo's
#     header and the rest of 3rdparty/ are compiled by the PCSX2 build itself.
#   - built here as well, at its newest release and the fixes after it: libnfs,
#     for the NFS shares of the virtual file layer (the prebuilt ports carry an
#     older libnfs, 5.0.3).
#   - libarchive, at its newest release, reading only (RAR, RAR5, ZIP and 7z): the HD texture
#     packs are RAR archives, unpacked as they download (ps5/frontend/ps5_texture_packs.cpp).
#
# Everything lands in .deps/ (ignored): src/ the sources, ports/ the prefix the
# build reads, build/ the build trees and their logs.
set -euo pipefail

ps5=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
root=$(dirname "$ps5")
sdk=$(bash "$ps5/tools/setup-sdk.sh")
deps="$root/.deps"
src="$deps/src"
sys="$deps/ports"
bld="$deps/build"
patches="$ps5/patches/deps"
mkdir -p "$src" "$sys/include" "$sys/lib" "$bld"

step() { echo "==> [deps] $1"; }
have() { [[ -f $bld/$1.done ]]; }
mark() { touch "$bld/$1.done"; }

# ---------------------------------------------------------------- prebuilt ports
pacbrew_version=v0.40.2
pacbrew_hash=a85f65de418a8e6a898c6c3e3c870d50fff7618a200e4dd59ea9692af6ecec4d
pacbrew="$deps/pacbrew/user/homebrew"
if [[ ! -d $pacbrew/lib ]]; then
    step "prebuilt ports $pacbrew_version (about 346 MB)"
    archive="$deps/pacbrew-$pacbrew_version.tar.gz"
    [[ -f $archive ]] || wget -q "https://github.com/ps5-payload-dev/pacbrew-repo/releases/download/$pacbrew_version/ps5-payload-dev.tar.gz" -O "$archive"
    printf '%s  %s\n' "$pacbrew_hash" "$archive" | sha256sum --check --strict >/dev/null
    mkdir -p "$deps/pacbrew"
    tar -xzf "$archive" -C "$deps/pacbrew" --strip-components=3 opt/ps5-payload-sdk/target/user/homebrew
fi
if ! have ports-copy; then
    step "copying the ports the title uses"
    for header in zlib.h zconf.h zstd.h zstd_errors.h zdict.h jconfig.h jerror.h jmorecfg.h jpeglib.h turbojpeg.h \
            libpsl.h bzlib.h; do
        cp "$pacbrew/include/$header" "$sys/include/"
    done
    for folder in webp curl openssl smb2; do
        rm -rf "$sys/include/$folder"
        cp -r "$pacbrew/include/$folder" "$sys/include/"
    done
    for library in libz.a libzstd.a libwebp.a libwebpdemux.a libwebpmux.a libsharpyuv.a libjpeg.a libturbojpeg.a \
            libcurl.a libssl.a libcrypto.a libpsl.a libsmb2.a libbz2.a; do
        cp "$pacbrew/lib/$library" "$sys/lib/"
    done
    mark ports-copy
fi

# --------------------------------------------------------------------- sources
# name, repository, revision: PCSX2's versions; shaderc is the fork with the debug-information
# option PCSX2's patch adds (the same change, already applied).
fetch() {
    local name=$1 url=$2 revision=$3
    [[ -d $src/$name/.git ]] && return 0
    step "fetching $name"
    rm -rf "$src/$name"
    mkdir -p "$src/$name"
    git -C "$src/$name" init -q
    git -C "$src/$name" remote add origin "$url"
    git -C "$src/$name" fetch -q --depth 1 origin "$revision"
    git -C "$src/$name" checkout -q FETCH_HEAD
}
fetch shaderc https://github.com/stenzek/shaderc da25addad407d2ec377ab3c113d202f10adcf062
fetch libpng https://github.com/pnggroup/libpng refs/tags/v1.6.58
fetch sdl3 https://github.com/libsdl-org/SDL refs/tags/release-3.4.16
fetch brotli https://github.com/google/brotli refs/tags/v1.2.0
fetch freetype https://github.com/freetype/freetype refs/tags/VER-2-14-3
fetch lz4 https://github.com/lz4/lz4 refs/tags/v1.10.0
fetch plutovg https://github.com/sammycage/plutovg refs/tags/v1.3.3
fetch plutosvg-upstream https://github.com/sammycage/plutosvg refs/tags/v0.0.8
fetch rapidyaml https://github.com/biojppm/rapidyaml refs/tags/v0.12.1
# libnfs: its 8.0.0 release (2026-09-20) and the four commits after it on master up to 7613236
# (2026-10-08), among them 27d0cd0, without which NFS 3 opens report every refusal (a file that is
# not there, a folder that may not be written in) as ERANGE, and a bound on the sizes ZDR decodes.
fetch libnfs https://github.com/sahlberg/libnfs 7613236464317bee2d9636f087f995148de4db27
fetch libarchive https://github.com/libarchive/libarchive refs/tags/v3.8.10

# ---------------------------------------------------------------- cross builds
# The SDK's CMake toolchain links real executables for its feature checks, so a
# check for a function answers what the SDK's stubs have. (The stubs have more
# than the console does: the overrides below turn off what libzip would use.)
common=(-G Ninja -DCMAKE_TOOLCHAIN_FILE="$sdk/toolchain/prospero.cmake" -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_INSTALL_PREFIX="$sys" -DCMAKE_PREFIX_PATH="$sys" -DCMAKE_FIND_ROOT_PATH="$sys"
        -DBUILD_SHARED_LIBS=OFF -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_VERBOSE_MAKEFILE=OFF
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5
        "-DCMAKE_C_FLAGS=-ffunction-sections -fdata-sections -fno-omit-frame-pointer"
        "-DCMAKE_CXX_FLAGS=-ffunction-sections -fdata-sections -fno-omit-frame-pointer")
build() {
    local name=$1
    shift
    cmake "${common[@]}" -B "$bld/$name" "$@" >"$bld/$name.log" 2>&1 &&
        cmake --build "$bld/$name" >>"$bld/$name.log" 2>&1 &&
        cmake --install "$bld/$name" >>"$bld/$name.log" 2>&1 ||
        { tail -30 "$bld/$name.log" >&2; exit 1; }
    mark "$name"
}

if ! have libpng; then
    step libpng
    rm -rf "$bld/libpng-src"
    cp -r "$src/libpng" "$bld/libpng-src"
    (cd "$bld/libpng-src" && patch -s -p1 < "$patches/libpng-1.6.56-apng.patch")
    build libpng -S "$bld/libpng-src" -DPNG_TESTS=OFF -DPNG_STATIC=ON -DPNG_SHARED=OFF -DPNG_TOOLS=OFF \
        -DPNG_FRAMEWORK=OFF -DZLIB_ROOT="$sys" -DZLIB_INCLUDE_DIR="$sys/include" -DZLIB_LIBRARY="$sys/lib/libz.a"
fi

# FreeType with Brotli, as PCSX2's own dependency builds have it: icon fonts in WOFF2 files
# need it, and the prebuilt port's FreeType has none.
if ! have brotli; then
    step brotli
    build brotli -S "$src/brotli" -DBROTLI_BUILD_TOOLS=OFF -DBROTLI_DISABLE_TESTS=ON
fi

if ! have freetype; then
    step "FreeType (zlib, libpng, Brotli)"
    rm -rf "$sys/include/freetype2" "$sys/lib/libfreetype.a" "$bld/plutosvg.done"
    build freetype -S "$src/freetype" -DFT_REQUIRE_ZLIB=ON -DFT_REQUIRE_PNG=ON -DFT_DISABLE_BZIP2=ON \
        -DFT_REQUIRE_BROTLI=ON -DFT_DISABLE_HARFBUZZ=ON \
        -DZLIB_INCLUDE_DIR="$sys/include" -DZLIB_LIBRARY="$sys/lib/libz.a" \
        -DPNG_PNG_INCLUDE_DIR="$sys/include" -DPNG_LIBRARY="$sys/lib/libpng16.a" \
        -DBROTLIDEC_INCLUDE_DIRS="$sys/include" \
        "-DBROTLIDEC_LIBRARIES=$sys/lib/libbrotlidec.a;$sys/lib/libbrotlicommon.a"
fi
# Its package file names ZLIB::ZLIB and PNG::PNG, which its users here have not looked up: they
# find FreeType by its header and library, as with the prebuilt port.
rm -rf "$sys/lib/cmake/freetype"

if ! have lz4; then
    step LZ4
    build lz4 -S "$src/lz4/build/cmake" -DLZ4_BUILD_CLI=OFF -DLZ4_BUILD_LEGACY_LZ4C=OFF
fi

if ! have plutovg; then
    step PlutoVG
    rm -rf "$sys/include/plutosvg" "$sys/lib/libplutosvg.a" "$sys/lib/cmake/plutosvg" "$sys/lib/pkgconfig/plutosvg.pc"
    build plutovg -S "$src/plutovg" -DPLUTOVG_BUILD_EXAMPLES=OFF
fi

if ! have plutosvg-upstream; then
    step PlutoSVG
    build plutosvg-upstream -S "$src/plutosvg-upstream" -DPLUTOSVG_ENABLE_FREETYPE=OFF -DPLUTOSVG_BUILD_EXAMPLES=OFF
fi

if ! have rapidyaml; then
    step RapidYAML
    git -C "$src/rapidyaml" submodule update -q --init --recursive --depth 1
    build rapidyaml -S "$src/rapidyaml" -DRYML_BUILD_TOOLS=OFF -DRYML_BUILD_API=OFF -DRYML_DEFAULT_CALLBACK_USES_EXCEPTIONS=OFF
fi

# libnfs: NFS versions 3 and 4 with AUTH_SYS, for ps5/frontend/vfs/ps5_vfs_nfs.cpp. No TLS, no
# Kerberos, no threads of its own (the virtual file layer serialises the calls on a context).
#   - The SDK's FreeBSD headers name the coarse monotonic clock CLOCK_MONOTONIC_FAST, so libnfs's
#     check for clock_gettime fails and it falls back to time(), whose one-second steps make every
#     timeout up to a second late: it is given CLOCK_MONOTONIC, the clock the title uses already.
#   - NO_SRV_AUTOSCAN leaves out the search for servers on the local network (nfs_find_local_servers,
#     which lists the interfaces through ioctl); nothing asks for it.
#   - No system module exports getservbyport, which libnfs asks before it binds a reserved source
#     port, and getuid/getgid only give the default account, which the layer always replaces: the
#     three go to the layer's own (ps5vfs_nfs_* in ps5_vfs_nfs.cpp), linked in the same group.
if ! have libnfs; then
    step "libnfs 8.0.0+ (7613236)"
    rm -rf "$sys/include/nfsc" "$sys/lib/libnfs.a" "$sys/lib/cmake/libnfs"
    nfs_flags="-ffunction-sections -fdata-sections -fno-omit-frame-pointer -DCLOCK_MONOTONIC_COARSE=CLOCK_MONOTONIC"
    nfs_flags+=" -DNO_SRV_AUTOSCAN -Dgetservbyport=ps5vfs_nfs_getservbyport -Dgetuid=ps5vfs_nfs_getuid"
    nfs_flags+=" -Dgetgid=ps5vfs_nfs_getgid"
    build libnfs -S "$src/libnfs" -DENABLE_TLS=OFF -DENABLE_MULTITHREADING=OFF -DENABLE_UTILS=OFF \
        -DENABLE_EXAMPLES=OFF -DENABLE_TESTS=OFF -DENABLE_DOCUMENTATION=OFF "-DCMAKE_C_FLAGS=$nfs_flags"
    # Its package files are not used: the layer finds libnfs by its header and library.
    rm -rf "$sys/lib/cmake/libnfs" "$sys/lib/pkgconfig/libnfs.pc"
fi

# libarchive: reading only, with zlib for ZIP; no tools, no tests, nothing it would ask the
# console's libc for that it lacks (iconv, ACLs, extended attributes, the crypto libraries:
# encrypted archives are refused, as nothing here needs them). RAR and RAR5 need nothing outside.
if ! have libarchive; then
    step "libarchive 3.8.10 (reading)"
    rm -rf "$sys/include/archive.h" "$sys/include/archive_entry.h" "$sys/lib/libarchive.a"
    build libarchive -S "$src/libarchive" -DENABLE_TAR=OFF -DENABLE_CPIO=OFF -DENABLE_CAT=OFF -DENABLE_UNZIP=OFF \
        -DENABLE_TEST=OFF -DENABLE_INSTALL=ON -DENABLE_OPENSSL=OFF -DENABLE_MBEDTLS=OFF -DENABLE_NETTLE=OFF -DENABLE_CNG=OFF \
        -DENABLE_LIBB2=OFF -DENABLE_LZ4=OFF -DENABLE_LZO=OFF -DENABLE_LZMA=OFF -DENABLE_ZSTD=OFF -DENABLE_BZip2=OFF \
        -DENABLE_LIBXML2=OFF -DENABLE_EXPAT=OFF -DENABLE_PCREPOSIX=OFF -DENABLE_PCRE2POSIX=OFF -DENABLE_ICONV=OFF \
        -DENABLE_ACL=OFF -DENABLE_XATTR=OFF -DENABLE_WIN32_XMLLITE=OFF -DENABLE_ZLIB=ON -DBUILD_SHARED_LIBS=OFF \
        -DZLIB_INCLUDE_DIR="$sys/include" -DZLIB_LIBRARY="$sys/lib/libz.a"
    rm -f "$sys/lib/"libarchive.so* "$sys/lib/pkgconfig/libarchive.pc"
    rm -rf "$sys/lib/cmake/libarchive"
fi

if ! have shaderc; then
    step "shaderc (glslang and SPIRV-Tools with it: a long build)"
    build shaderc -S "$src/shaderc" -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON -DSHADERC_SKIP_EXECUTABLES=ON \
        -DSHADERC_SKIP_COPYRIGHT_CHECK=ON -DSHADERC_ENABLE_HLSL=OFF -DSPIRV_TOOLS_BUILD_STATIC=ON \
        -DSPIRV_SKIP_EXECUTABLES=ON -DSPIRV_SKIP_TESTS=ON -DENABLE_GLSLANG_BINARIES=OFF \
        -DSKIP_GLSLANG_INSTALL=OFF -DSKIP_SPIRV_TOOLS_INSTALL=OFF
    # The fork builds its C API as a shared library only, which a title cannot
    # load: the same objects go into an archive, beside the archives they need.
    rm -f "$sys/lib/"libshaderc_shared.so* "$sys/lib/libshaderc.a"
    "$sdk/bin/prospero-ar" rcs "$sys/lib/libshaderc.a" "$bld/shaderc/libshaderc/CMakeFiles/shaderc_shared.dir/src/"*.o
    cp "$bld/shaderc/libshaderc_util/libshaderc_util.a" "$bld/shaderc/third_party/glslang/glslang/libglslang.a" \
        "$bld/shaderc/third_party/glslang/SPIRV/libSPIRV.a" "$bld/shaderc/third_party/spirv-tools/source/libSPIRV-Tools.a" \
        "$bld/shaderc/third_party/spirv-tools/source/opt/libSPIRV-Tools-opt.a" "$sys/lib/"
fi
echo "==> [deps] ready in $sys"
