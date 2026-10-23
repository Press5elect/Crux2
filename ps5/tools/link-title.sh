#!/usr/bin/env bash
# Crux2 - link the title and package its folder.
#
#   link-title.sh BUILD_DIR PS5_VULKAN_DIR SDK
#
# Run by tools/build.sh once the libraries are compiled. The link is PS5_Vulkan's: its RADV
# release archive (RADV, ACO, NIR and Mesa's runtime, built from PS5_Mesa), its link recipe
# (tools/radv-link.sh: the platform layer of this title's SDK pin, the heap and thread wraps, the
# libc names bound to ps5_*), its CRT, its native tool (ELF -> the console's fake SELF) and its
# libc.prx, as its CTS title links them.
#
# On top of the recipe, this title binds:
#   - getaddrinfo/freeaddrinfo to its own (frontend/ps5_libc.c: numeric addresses and the console's
#     resolver; the platform layer's refuses every lookup, and libsmb2 and curl both go through it);
#   - the functions no system module exports (pipe2, lockf, timegm...), and a few a title may be
#     refused or would get from a module it should not depend on, to frontend/ps5_libc.c;
#   - every other libc name the title references that the platform layer has a ps5_ replacement
#     for (functions the console lacks, refuses to a title, or faults in).
#
# The title folder, dist/<TITLE_ID>/, gets the signed eboot.bin, sce_sys/, sce_module/libc.prx,
# the emulator's resources (with a certificate bundle for HTTPS), the sandbox elevation helper for
# USB drives, and the folders a user fills.
#
# Made from PS5 Vulkan Template's ps5/tools/link-title.sh (Copyright (C) 2026 Mihawk, MIT).
# SPDX-FileCopyrightText: 2026 Press5elect (the changes for Crux2)
# SPDX-License-Identifier: MIT

set -euo pipefail
[[ -n ${LINK_TRACE:-} ]] && set -x

work=$1 vulkan=$2 sdk=$3
ps5=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
root=$(dirname "$ps5")
ports="$root/.deps/ports"
archive=${RADV_ARCHIVE:-$vulkan/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a}
export PS5_CLANG=${PS5_CLANG:-$(command -v clang || true)}
tool="$vulkan/build/host/ps5-native-tool"
native="$vulkan/tooling/native"
param="$ps5/sce_sys/param.json"
nm="$sdk/bin/llvm-nm"

# The emulator's core: one archive, or, in a build with PCSX2's LTO_PCSX2_CORE option, the
# archive of the sources that option optimises at link time (the rest of the core is then inside
# libpcsx2-ps5.a, where CMake puts an object library's files).
pcsx2_archive="$work/pcsx2/libpcsx2.a"
lto_flags=()
if [[ -f $work/pcsx2/libPCSX2_LTO.a ]]; then
    # (-O3 as the files are compiled with; the linker's own default for this step is -O2.)
    pcsx2_archive="$work/pcsx2/libPCSX2_LTO.a"
    lto_flags=(--lto-O3)
    # The core's code is generated here, so what build.sh asks of the compiler for PGO is asked
    # of the linker's code generation too (ps5/tools/build.sh, PS5PX_PGO):
    case ${PS5PX_PGO:-} in
        # the second round counts after inlining, which in this core happens here;
        cs-generate) lto_flags+=(--lto-cs-profile-generate) ;;
        # the profile's second-round counts are applied after inlining, and the cold blocks of
        # hot functions are split away from them.
        # (PS5PX_PGO_SPLIT=1 only: off by default, ps5/tools/build.sh says why.)
        use) [[ ${PS5PX_PGO_SPLIT:-} == 1 ]] && lto_flags+=("--lto-cs-profile-file=$PS5PX_PGO_PROFILE_PATH" -mllvm -enable-split-machine-functions) ;;
    esac
fi
# With PS5PX_PGO_SPLIT=1, the functions are laid out by cache-directed sort (LLD's newer
# heuristic) rather than its default.
[[ ${PS5PX_PGO:-} == use && ${PS5PX_PGO_SPLIT:-} == 1 ]] && lto_flags+=(--call-graph-profile-sort=cdsort)

for file in "$archive" "$tool" "$param" "$sdk/bin/prospero-lld" "$vulkan/tools/radv-link.sh" \
        "$vulkan/runtime/libc.prx" "$work/pcsx2-ps5/libpcsx2-ps5.a" "$pcsx2_archive"; do
    [[ -e $file ]] || { echo "missing: $file" >&2; exit 2; }
done
mkdir -p "$work/link/obj" "$work/link/stubs"
cc() { PS5_PAYLOAD_SDK="$sdk" sh "$vulkan/tooling/prospero-clang18" "$@"; }

# The CRT, as in PS5_Vulkan's CTS title.
cc -std=c++20 -O2 -fno-exceptions -fno-rtti -c "$native/app_crt.cpp" -o "$work/link/obj/app_crt.o"
# RADV calls AGC, which the SDK has no stubs for: these name its imports.
stub() {
    local library=$1 source=$2
    cc -std=c11 -O2 -fPIC -c "$vulkan/$source" -o "$work/link/obj/${library}_stub.o"
    "$sdk/bin/prospero-lld" --shared -soname "${library}.prx" \
        -o "$work/link/stubs/${library}.so" "$work/link/obj/${library}_stub.o"
}
stub libSceAgc vendor/ps5/sdk/stubs/agc_canary_link_stub.c
stub libSceAgcDriver vendor/ps5/sdk/stubs/agc_driver_canary_link_stub.c

# ---- what is linked ------------------------------------------------------------------------
# The emulator and the port layer; then what they are built on. All in one group: the libraries
# refer to each other in both directions (core <-> util, the host in the port layer).
title_archives=(
    "$work/pcsx2-ps5/libpcsx2-ps5.a"
    "$pcsx2_archive"
    "$work/common/libcommon.a"
    "$work/pcsx2-ps5/libps5vfs.a"
    "$work/libcpuinfo.a"
)
while IFS= read -r library; do
    title_archives+=("$library")
done < <(find "$work/3rdparty" -name '*.a' | sort)
# paraLLEl-GS, the second GS renderer (ps5/tools/build-parallel-gs.sh): one object of its own.
[[ -n ${PS5PX_PGS_OBJECT:-} ]] && title_archives+=("$PS5PX_PGS_OBJECT")

# (zlib is not in the list: the RADV archive carries zlib 1.3.1 whole, and a second copy would
# define every function twice.)
port_archives=()
for name in archive smb2 nfs curl ssl crypto psl plutosvg plutovg freetype brotlidec brotlicommon png16 webpdemux webpmux webp \
        sharpyuv jpeg shaderc shaderc_util glslang SPIRV SPIRV-Tools-opt SPIRV-Tools ryml c4core lz4 zstd bz2; do
    [[ -f $ports/lib/lib$name.a ]] || { echo "missing: $ports/lib/lib$name.a (ps5/tools/build-deps.sh)" >&2; exit 2; }
    port_archives+=("$ports/lib/lib$name.a")
done

# A profiling build (PS5PX_PGO=generate or cs-generate, ps5/tools/build.sh): LLVM's profile
# runtime, built for the console from the source of the SDK's clang release
# (tools/build-profile-runtime.sh).
profiling=
[[ ${PS5PX_PGO:-} == generate || ${PS5PX_PGO:-} == cs-generate ]] && profiling=1
if [[ -n $profiling ]]; then
    port_archives+=("$(bash "$(dirname "${BASH_SOURCE[0]}")/build-profile-runtime.sh" "$sdk")")
    # (the same archive under the name clang writes into each instrumented object)
    extra_flags_pgo=(-L "$(dirname "${port_archives[-1]}")/lib")
fi

# ---- the libc names this title binds ---------------------------------------------------------
# shellcheck source=/dev/null
source "$vulkan/tools/radv-link.sh"
radv_link_recipe "$vulkan" "$sdk" "$archive" || exit 2

declare -A bound=()
for flag in "${radv_link_flags[@]}"; do
    case $flag in
        --defsym=*) name=${flag#--defsym=}; bound[${name%%=*}]=1 ;;
        --wrap=*) bound[${flag#--wrap=}]=1 ;;
    esac
done

extra_flags=()
extra_names=()
bind() { # bind NAME TARGET
    # A name PS5_Vulkan's recipe binds already (it binds more with each SDK: strcasestr, mkstemp,
    # readlink, link and symlink since its f3cbf87) is not bound twice: the recipe's flag is kept
    # when it names the same target, and made this title's when it names another.
    local index
    for index in "${!radv_link_flags[@]}"; do
        case ${radv_link_flags[$index]} in
            "--defsym=$1=$2") return 0 ;;
            "--defsym=$1="*) radv_link_flags[index]="--defsym=$1=$2"; return 0 ;;
        esac
    done
    extra_flags+=("--defsym=$1=$2")
    extra_names+=("$1")
    bound[$1]=1
}

# Name lookups: this title's, in place of the platform layer's refusal.
for index in "${!radv_link_flags[@]}"; do
    case ${radv_link_flags[$index]} in
        --defsym=getaddrinfo=ps5_getaddrinfo) radv_link_flags[index]=--defsym=getaddrinfo=ps5px_getaddrinfo ;;
        --defsym=freeaddrinfo=ps5_freeaddrinfo) radv_link_flags[index]=--defsym=freeaddrinfo=ps5px_freeaddrinfo ;;
    esac
done
[[ " ${radv_link_flags[*]} " == *" --defsym=getaddrinfo=ps5px_getaddrinfo "* ]] || bind getaddrinfo ps5px_getaddrinfo
[[ " ${radv_link_flags[*]} " == *" --defsym=freeaddrinfo=ps5px_freeaddrinfo "* ]] || bind freeaddrinfo ps5px_freeaddrinfo

# No system module exports these (frontend/ps5_libc.c says what each is for).
for name in pipe2 lockf gethostid timegm getprotobyname sendmmsg recvmmsg _setjmp _longjmp; do
    bind "$name" "ps5px_$name"
done
# Exported, but a system call a title may be refused, or by a module a title should not depend
# on (libkernel_sys, libScePosixForWebKit): the title's own, with the answer the console would give.
for name in getlogin_r gethostname random srandom fdatasync issetugid pathconf memrchr basename fnmatch; do
    bind "$name" "ps5px_$name"
done
# abort(), exit() and _exit() end the title with no signal and so no crash report: the title's
# calls go through frontend/ps5_crash.cpp, which records the caller in the boot log first.
for name in abort exit _exit _Exit; do
    extra_flags+=("--wrap=$name")
done
# clock_gettime is a system call of about 20 us on the console: CLOCK_MONOTONIC comes from the
# processor's counter instead, for the whole title and the graphics driver (frontend/ps5_clock.c).
extra_flags+=("--wrap=clock_gettime")
# lstat fails inside the sandbox where stat answers: frontend/ps5_libc.c tries it, then stat.
extra_flags+=("--wrap=lstat")
# poll ends with EINTR when a signal (the sampling profiler's) reaches the waiting thread, and is not
# restarted: frontend/ps5_libc.c waits again for what is left, so a network share's wait never fails
# for it.
extra_flags+=("--wrap=poll")
# std::filesystem (libc++) names these; the platform layer's refuse as a file system without links,
# and give the title's folder as the working directory.
for name in symlink link getcwd; do
    bind "$name" "ps5_$name"
done
# One datagram into one buffer, for the emulated network adapter's ICMP session: through recvfrom.
bind recvmsg ps5px_recvmsg

# localeconv in the C locale, '.' the decimal point (the console's gives none): the platform
# layer's since SDK fork 6b63a2a, bound by PS5_Vulkan's recipe since its 6a6dfa6.
platform="$sdk/target/lib/libps5platform.a"
"$nm" --defined-only "$platform" 2>/dev/null | awk '$2 == "T" && $3 ~ /^ps5_/ { print substr($3, 5) }' \
    | sort -u > "$work/link/platform-names.txt"
"$nm" -u "${title_archives[@]}" "${port_archives[@]}" 2>/dev/null | awk '$1 == "U" { print $2 }' \
    | sort -u > "$work/link/referenced-names.txt"
if grep -qx localeconv "$work/link/platform-names.txt" && [[ -z ${bound[localeconv]:-} ]]; then
    bind localeconv ps5_localeconv
fi
# Every other libc function the title calls that the platform layer replaces: the console lacks
# it, refuses it to a title, or faults in it (ps5platform/libc.h says which).
#
# Not the ones that adjust an answer and hand the rest to the console's own function (sysconf,
# pthread_exit): bound by name, their call of the real function would be a call of themselves, and
# the first sysconf(_SC_PAGESIZE) ran the stack out (inside the driver's device properties, on the
# first console runs). Those are found here as the names an object of the platform layer both
# defines as ps5_NAME and leaves undefined as NAME; the title calls the console's own.
"$nm" "$platform" 2>/dev/null | awk '
    /:$/ { member = $1; next }
    $1 == "U" { undefined[member] = undefined[member] " " $2 " " }
    $2 == "T" && $3 ~ /^ps5_/ { defined[member] = defined[member] " " substr($3, 5) }
    END {
        for (member in defined) {
            count = split(defined[member], names, " ")
            for (i = 1; i <= count; i++)
                if (index(undefined[member], " " names[i] " "))
                    print names[i]
        }
    }' | sort -u > "$work/link/platform-pass-through.txt"
while IFS= read -r name; do
    [[ -n ${bound[$name]:-} ]] && continue
    if grep -qx -- "$name" "$work/link/platform-pass-through.txt"; then
        echo "not bound (the platform layer's calls the console's own): $name"
        continue
    fi
    bind "$name" "ps5_$name"
done < <(comm -12 "$work/link/platform-names.txt" "$work/link/referenced-names.txt")
printf '%s\n' "${extra_names[@]}" > "$work/link/bound-by-title.txt"

# A bound name the SDK's stub libraries also define would be exported from the title to override
# theirs, and the title converter refuses exports: every bound name stays local.
{
    printf '{\n    local:\n'
    printf '        %s;\n' "${extra_names[@]}"
    printf '};\n'
} > "$work/link/ps5px-local.map"

# The program's first byte gets a name, so the crash report can give addresses relative to it
# (frontend/ps5_crash.cpp). The layout itself is PS5_Vulkan's.
cat > "$work/link/pcsx2-pie.ld" <<'EOF'
INCLUDE ps5-pie-unwind.ld

PROVIDE(__executable_start = ADDR(.text));
EOF
# A profiling build: the counters and the records that point at them are written while the title
# runs, so they go with the writable data; left to the linker they followed .got.plt into the
# region the native tool keeps for the process parameters.
if [[ -n $profiling ]]; then
    cat >> "$work/link/pcsx2-pie.ld" <<'EOF'

SECTIONS {
    __llvm_prf_cnts : { KEEP(*(__llvm_prf_cnts)) } : data
    __llvm_prf_bits : { KEEP(*(__llvm_prf_bits)) } : data
    __llvm_prf_data : { KEEP(*(__llvm_prf_data)) } : data
    __llvm_prf_vals : { KEEP(*(__llvm_prf_vals)) } : data
    __llvm_prf_vnds : { KEEP(*(__llvm_prf_vnds)) } : data
} INSERT AFTER .data;
EOF
fi

# ---- the link ----------------------------------------------------------------------------------
# Mesa's dispatch tables name entry points through weak references; with LLD 18 an undefined weak
# symbol would otherwise become an import the native tool cannot resolve.
"$sdk/bin/prospero-lld" -T "$work/link/pcsx2-pie.ld" -L "$vulkan/tooling/psbc" -L "$native" \
    --eh-frame-hdr "${radv_link_flags[@]}" "${extra_flags[@]}" ${extra_flags_pgo[@]+"${extra_flags_pgo[@]}"} \
    --no-dynamic-linker -z nodynamic-undefined-weak --error-limit=0 \
    --version-script "$native/app-symbols.map" --version-script "$work/link/ps5px-local.map" \
    --exclude-libs=ALL -Map="$work/link/llvm-pie.map" "${lto_flags[@]}" ${PS5PX_LLD_FLAGS:-} \
    -e _start -o "$work/link/llvm-pie.elf" \
    "$work/link/obj/app_crt.o" \
    --start-group "${title_archives[@]}" "${port_archives[@]}" --end-group \
    "$work/link/stubs/libSceAgc.so" "$work/link/stubs/libSceAgcDriver.so" \
    "${radv_link_inputs[@]}" \
    --as-needed "$sdk"/target/lib/*.so
# LINK_ONLY=1 stops here: the ELF and its map, to look at, with the title folder left as it is.
# A title loads neither libkernel_sys's exports nor libScePosixForWebKit's: an import that only
# their stubs define links, and is null at run time, so its first call jumps to address 0. A title
# that imports one is refused here rather than on the console. (PS5 Vulkan Template's link-title.sh
# has this check since its 0864913, Copyright (C) 2026 Mihawk, MIT.)
null_imports=$(comm -23 \
    <("$sdk/bin/llvm-nm" -D --undefined-only "$work/link/llvm-pie.elf" |
        awk '$1 == "U" { sub(/@.*/, "", $2); print $2 }' | sort -u) \
    <(for library in "$sdk"/target/lib/*.so "$work/link/stubs/libSceAgc.so" "$work/link/stubs/libSceAgcDriver.so"; do
        case ${library##*/} in libkernel_sys.so | libScePosixForWebKit.so) continue ;; esac
        "$sdk/bin/llvm-nm" -D --defined-only "$library" 2>/dev/null | awk '{ print $NF }'
    done | sort -u))
if [[ -n $null_imports ]]; then
    echo "link-title.sh: imports that no module a title loads exports (null at run time): ${null_imports//$'\n'/ }" >&2
    echo "link-title.sh: bind them to the platform layer, or to a function of the title's own" >&2
    exit 1
fi

[[ ${LINK_ONLY:-0} == 1 ]] && { echo "linked: $work/link/llvm-pie.elf"; exit 0; }
"$tool" link --in "$work/link/llvm-pie.elf" --out "$work/eboot.elf.new" \
    --stub-dir "$sdk/target/lib" --stub "$work/link/stubs/libSceAgc.so" \
    --stub "$work/link/stubs/libSceAgcDriver.so" --module-sdk 0x02000009 \
    --companion-sdk 0x08050001 --file-name eboot.elf

# ---- the title folder --------------------------------------------------------------------------
title_id=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$param")
app="$root/dist/$title_id"
mkdir -p "$app/sce_sys" "$app/sce_module"
"$tool" self --sign --in "$work/eboot.elf.new" --out "$app/eboot.bin" --magic 0x1D3D154F
"$tool" self --inspect --file "$app/eboot.bin" > /dev/null
cp "$param" "$app/sce_sys/param.json"
for asset in icon0.png pic0.dds pic1.dds snd0.at9; do
    [[ -f $ps5/sce_sys/$asset ]] && cp "$ps5/sce_sys/$asset" "$app/sce_sys/$asset"
done
(cd "$vulkan/runtime" && sha256sum --check --strict --quiet libc.prx.sha256)
cp "$vulkan/runtime/libc.prx" "$app/sce_module/libc.prx"

# The emulator's resources (fonts, the game database, its icon and banner, sounds, shaders), what
# the port adds to them (the interface's bold font), and the certificates HTTPS downloads are
# verified with.
rm -rf "$app/resources"
cp -r "$root/vendor/pcsx2/bin/resources" "$app/resources"
cp -r "$ps5/package/resources/." "$app/resources/"
rm -rf "$app/resources/shaders/dx11" "$app/resources/shaders/opengl"
# PCSX2's collection of game patches (widescreen, no-interlacing) is not packaged: its
# repository states no licence, so a package that may be passed on leaves it out (README, "Game
# patches": anyone adds it to resources/ themselves). WITH_PATCHES=1 packages it, for a folder
# that goes to one's own console and no further; the release check refuses such a folder.
if [[ ${WITH_PATCHES:-0} == 1 ]]; then
    cp "$(bash "$ps5/tools/fetch-patches.sh")" "$app/resources/patches.zip"
fi
# The certificates: Mozilla's list as the certifi package carries it, not the build machine's own
# store, which may hold certificates of that machine's network (a proxy's, a company's) that a
# console has no business trusting. CA_BUNDLE names another file.
ca_bundle=${CA_BUNDLE:-$(python3 -c 'import certifi; print(certifi.where())' 2>/dev/null || true)}
[[ -n $ca_bundle && -f $ca_bundle ]] ||
    { echo "no certificate bundle: pip install certifi, or set CA_BUNDLE to a cacert.pem" >&2; exit 2; }
cp "$ca_bundle" "$app/resources/cacert.pem"

# USB drives: the helper the title sends to the payload loader (tools/build-elevator.sh).
cp "$(bash "$ps5/tools/build-elevator.sh")" "$app/sandbox-elevator.elf"

# What a user fills. (network.cfg is not among them: the title writes its template on the first
# run, so copying a newer build over the folder never replaces the shares a user entered.)
mkdir -p "$app/games" "$app/bios"
# The music played while no game is loaded is the user's own: the folder comes empty (.wav, .mp3,
# .ogg or .flac files put in it are played, in name order or shuffled).
mkdir -p "$app/music"
rm -f "$app/network.cfg"
cp "$ps5/package/games.txt" "$app/games/README.txt"
cp "$ps5/package/bios.txt" "$app/bios/README.txt"
[[ -f $root/README.md ]] && cp "$root/README.md" "$app/README.md"

# The names of the program's functions, for a test run's profiler (frontend/ps5_profiler.cpp):
# from the ELF just linked and its link map, so they are this eboot.bin's and no other's.
python3 "$ps5/tools/make-symbols.py" "$work/link/llvm-pie.elf" "$work/link/llvm-pie.map" "$app/symbols.txt" --nm "$nm"

# The notices: LEGAL.txt, and licenses/ with every part's licence, source and the files it is in.
PS5PX_BUILD_DIR="$work" python3 "$ps5/tools/stage-notices.py" "$app"

mv "$work/eboot.elf.new" "$work/eboot.elf"
printf '==> %s: %s (eboot.bin %s bytes; %d libc names bound by the title)\n' "$title_id" "$app" \
    "$(stat -c %s "$app/eboot.bin")" "${#extra_names[@]}"
printf '    build identity: %s\n' "$(cat "$work/build-identity.txt" 2>/dev/null || echo unknown)"
