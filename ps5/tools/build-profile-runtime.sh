#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - LLVM's profile runtime (compiler-rt/lib/profile) built for the console, for the
# profiling build (PS5PX_PGO=generate ps5/tools/build.sh).
#
#   ps5/tools/build-profile-runtime.sh <sdk>     prints the archive's path; the same archive is
#                                                in .deps/pgo/lib under the name clang asks for
#
# The SDK has no compiler-rt for the console. The profile runtime is portable C over stdio; the
# console's target defines __FreeBSD__ and __ELF__, so it takes the ELF branch
# (InstrProfilingPlatformLinux.c: the counters between the linker's __start_/__stop_ symbols of
# the __llvm_prf_* sections). Its source must be the release of the SDK's clang: the raw profile
# format changes between releases, and llvm-profdata of the same release reads it.
# The title never returns from main, so the runtime's atexit write never happens: the title
# writes the profile itself (frontend/ps5_pgo.cpp).
set -euo pipefail

ps5=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
root=$(dirname "$ps5")
sdk=${1:?usage: build-profile-runtime.sh <sdk>}
clang_version=$("$sdk/bin/prospero-clang" --version | sed -n 's/.*clang version \([0-9][0-9.]*\).*/\1/p' | head -1)
[[ -n $clang_version ]] || { echo "the SDK's clang version is unknown" >&2; exit 1; }
src="$root/.deps/src/llvm-profile"
out="$root/.deps/pgo"
archive="$out/libclang_rt.profile-ps5-$clang_version-$(sha256sum "${BASH_SOURCE[0]}" | cut -c1-8).a"

if [[ ! -f $src/compiler-rt/lib/profile/InstrProfiling.c ]] ||
    [[ $(git -C "$src" describe --tags --exact-match 2>/dev/null) != "llvmorg-$clang_version" ]]; then
    rm -rf "$src"
    git clone --quiet --depth 1 --branch "llvmorg-$clang_version" --filter=blob:none --sparse \
        https://github.com/llvm/llvm-project.git "$src" >&2
    git -C "$src" sparse-checkout set compiler-rt/lib/profile compiler-rt/include/profile >&2
fi

if [[ ! -f $archive ]]; then
    mkdir -p "$out/obj"
    profile="$src/compiler-rt/lib/profile"
    flags=(-O2 -fPIC -fno-profile-instr-generate -fno-profile-generate -ffunction-sections -fdata-sections
        -I "$src/compiler-rt/include" -I "$profile" -DCOMPILER_RT_HAS_ATOMICS=1 -DCOMPILER_RT_HAS_FCNTL_LCK=1
        -Wno-unused-parameter)
    objects=()
    for file in InstrProfiling InstrProfilingBuffer InstrProfilingFile InstrProfilingInternal InstrProfilingMerge \
        InstrProfilingMergeFile InstrProfilingNameVar InstrProfilingPlatformLinux InstrProfilingUtil \
        InstrProfilingValue InstrProfilingVersionVar InstrProfilingWriter; do
        source_file="$profile/$file.c"
        if [[ $file == InstrProfilingPlatformLinux ]]; then
            # No build IDs in the profile: they are for matching a profile to its binary through
            # debuginfod, and they are read from the ELF header at __ehdr_start, which the
            # console's signed executable need not have mapped. The runtime's own fallback
            # (no NT_GNU_BUILD_ID) writes none.
            sed 's|^#ifdef NT_GNU_BUILD_ID$|#if 0 /* Crux2: no build IDs (build-profile-runtime.sh) */|' \
                "$source_file" > "$out/obj/$file.c"
            grep -q 'Crux2: no build IDs' "$out/obj/$file.c" || { echo "build IDs: the runtime's source changed" >&2; exit 1; }
            source_file="$out/obj/$file.c"
        fi
        "$sdk/bin/prospero-clang" "${flags[@]}" -std=gnu11 -c "$source_file" -o "$out/obj/$file.o"
        objects+=("$out/obj/$file.o")
    done
    # The host's name, for the %h pattern of a profile's file name (not one Crux2 uses): the
    # runtime asks uname() for it, which the console is not known to have.
    printf '%s\n' '/* Crux2: lprofGetHostName without uname (build-profile-runtime.sh). */' '#include <string.h>' \
        'int lprofGetHostName(char *Name, int Len) { if (Len <= 0) return -1; strncpy(Name, "ps5", (size_t)Len); Name[Len - 1] = 0; return 0; }' \
        '/* procctl: FreeBSD'"'"'s, for holding off a parent-death signal while the profile is written;' \
        '   "not supported" is the runtime'"'"'s path without one. Weak: the SDK'"'"'s libc has its own since' \
        '   b5efad5, and that one is taken where it is there. */' \
        '#include <errno.h>' '#include <sys/types.h>' \
        '__attribute__((weak)) int procctl(int idtype, id_t id, int cmd, void *data) { (void)idtype; (void)id; (void)cmd; (void)data; errno = EINVAL; return -1; }' \
        '/* getpagesize through sysconf, which the title imports already: no import of its own (weak, as procctl). */' \
        '#include <unistd.h>' '__attribute__((weak)) int getpagesize(void) { return (int)sysconf(_SC_PAGESIZE); }' \
        > "$out/obj/HostName.c"
    "$sdk/bin/prospero-clang" "${flags[@]}" -std=gnu11 -c "$out/obj/HostName.c" -o "$out/obj/HostName.o"
    objects+=("$out/obj/HostName.o")
    "$sdk/bin/prospero-clang++" "${flags[@]}" -std=c++17 -fno-exceptions -fno-rtti \
        -c "$profile/InstrProfilingRuntime.cpp" -o "$out/obj/InstrProfilingRuntime.o"
    objects+=("$out/obj/InstrProfilingRuntime.o")
    rm -f "$archive.new"
    "$sdk/bin/llvm-ar" rcs "$archive.new" "${objects[@]}"
    mv "$archive.new" "$archive"
fi
# Clang for this target marks each instrumented object with a dependent library of the SDK's
# own name for the runtime; this is that library, for the linker to find in the folder printed.
mkdir -p "$out/lib"
cp -f "$archive" "$out/lib/libclang_rt.profile-x86_64_nosubmission.a"
echo "$archive"
