#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - build a game's native module from PS2Recomp's Crux2-mode output and the game's executable.
#
#   build-module.py --elf SLUS_213.76 --generated out/ --out SLUS_213.76.crux2native [--jobs N]
#
# Writes crux2_module_info.cpp next to the generated code: every function's range and the hash of
# its bytes in the executable (so that Crux2 runs a function's native code only while memory holds
# the same bytes), and for every address the converter can enter at, its function. Then compiles
# the generated code with Crux2's runtime (ps5/recomp/runtime) into one freestanding shared object
# for the console's processor (Zen 2), with no imports: Crux2's loader only places it.
#
# The module is the game's code, made from your own copy of the game: keep it to yourself.
import argparse
import io
import os
import re
import struct
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
RUNTIME = HERE.parent / "runtime"


def fnv1a64(data: bytes) -> int:
    h = 0xcbf29ce484222325
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


class Elf:
    """The loadable segments of a 32-bit little-endian MIPS executable (what the PS2 loads)."""

    def __init__(self, path: Path):
        data = path.read_bytes()
        if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
            raise SystemExit(f"{path}: not a 32-bit little-endian ELF file (a PS2 executable)")
        phoff, = struct.unpack_from("<I", data, 28)
        phentsize, phnum = struct.unpack_from("<HH", data, 42)
        self.segments = []
        for i in range(phnum):
            p_type, p_offset, p_vaddr, _, p_filesz, p_memsz, p_flags, _ = struct.unpack_from("<8I", data, phoff + i * phentsize)
            if p_type == 1:  # PT_LOAD
                image = data[p_offset:p_offset + p_filesz] + bytes(max(0, p_memsz - p_filesz))
                self.segments.append((p_vaddr, image))

    def read(self, start: int, end: int) -> bytes:
        out = bytearray()
        for vaddr, image in self.segments:
            lo, hi = max(start, vaddr), min(end, vaddr + len(image))
            if lo < hi:
                out[len(out):] = image[lo - vaddr:hi - vaddr]
        if len(out) != end - start:
            raise SystemExit(f"function 0x{start:x}-0x{end:x} is not inside the executable's loaded segments")
        return bytes(out)


EXPORTS = ("crux2_native_init", "crux2_native_has", "crux2_native_run", "crux2_native_invalidate", "crux2_native_stats")


def check_module(path: Path) -> str:
    """What is wrong with the linked module for Crux2's loader, or an empty string: it must export
    the five functions (in .dynsym) and import nothing."""
    data = path.read_bytes()
    if data[:4] != b"\x7fELF" or data[4] != 2:
        return "not a 64-bit ELF file"
    shoff, = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum, _ = struct.unpack_from("<HHH", data, 0x3A)
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize) for i in range(shnum)]
    dynsym = next((s for s in sections if s[1] == 11), None)  # SHT_DYNSYM
    if not dynsym:
        return "it has no dynamic symbols"
    strtab = sections[dynsym[6]]
    names, imports = set(), []
    for i in range(1, dynsym[5] // 24):
        st_name, st_info, _, st_shndx, _, _ = struct.unpack_from("<IBBHQQ", data, dynsym[4] + i * 24)
        end = data.index(b"\0", strtab[4] + st_name)
        name = data[strtab[4] + st_name:end].decode(errors="replace")
        if st_shndx == 0:
            imports.append(name)
        else:
            names.add(name)
    missing = [n for n in EXPORTS if n not in names]
    if missing:
        return "it does not export " + ", ".join(missing)
    if imports:
        return "it imports " + ", ".join(sorted(set(imports))[:10])
    return ""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--elf", required=True, type=Path)
    parser.add_argument("--generated", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--made-by", default="PS2Recomp, Crux2 mode")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "clang++"))
    parser.add_argument("--cc", default=os.environ.get("CC", "clang"))
    parser.add_argument("--opt", default="-O2")
    args = parser.parse_args()

    gen = args.generated
    elf = Elf(args.elf)

    # Each generated function: its name and range.
    functions = {}
    header = re.compile(r"// Function: .*\n// Address: 0x([0-9a-fA-F]+) - 0x([0-9a-fA-F]+)\nvoid (\w+)\(uint8_t\* rdram, R5900Context\* ctx, PS2Runtime \*runtime\)")
    sources = sorted(p for p in gen.glob("*.cpp") if p.name not in ("register_functions.cpp", "crux2_module_info.cpp"))
    for source in sources:
        for m in header.finditer(source.read_text(errors="replace")):
            functions[m.group(3)] = (int(m.group(1), 16), int(m.group(2), 16))
    if not functions:
        raise SystemExit(f"{gen}: no generated functions found (PS2Recomp's output, made with PS2RECOMP_CRUX2=1?)")

    # The address table: which function each enterable address belongs to.
    table = (gen / "register_functions.cpp").read_text()
    base = int(re.search(r"g_ps2RecompiledFunctionTableBase = 0x([0-9a-fA-F]+)u", table).group(1), 16)
    slots = int(re.search(r"g_ps2RecompiledFunctionTableSlotCount = (\d+)u", table).group(1))
    entries = [(int(m.group(1)), m.group(2)) for m in re.finditer(r"g_ps2RecompiledFunctionTable\[(\d+)\] = (\w+);", table)]

    ordered = sorted(functions.items(), key=lambda kv: kv[1][0])
    index = {name: i for i, (name, _) in enumerate(ordered)}
    slot_function = [0] * slots
    for slot, name in entries:
        if name in index and slot < slots:
            slot_function[slot] = index[name] + 1
    missing = sorted({name for _, name in entries if name not in index})

    info = gen / "crux2_module_info.cpp"
    with io.StringIO() as f:
        f.write("// Written by Crux2's build-module.py: the functions of the executable this module was made from.\n")
        f.write('#include "ps2_runtime.h"\n\n')
        f.write(f"extern const uint32_t g_crux2FunctionCount = {len(ordered)}u;\n")
        f.write(f"uint8_t g_crux2FunctionState[{len(ordered)}];\n")
        f.write("extern const Crux2NativeFunction g_crux2Functions[] = {\n")
        for name, (start, end) in ordered:
            f.write(f"\t{{0x{start:x}u, 0x{end:x}u, 0x{fnv1a64(elf.read(start, end)):016x}ull}}, // {name}\n")
        f.write("};\n")
        f.write(f"extern const uint32_t g_crux2SlotFunction[{slots}] = {{\n")
        for i in range(0, slots, 32):
            f.write("\t" + ",".join(str(v) for v in slot_function[i:i + 32]) + ",\n")
        f.write("};\n")
        executable = args.elf.name.replace("\\", "\\\\").replace('"', '\\"')
        f.write(f'extern const char g_crux2Executable[] = "{executable}";\n')
        f.write(f'extern const char g_crux2MadeBy[] = "{args.made_by}";\n')
        text = f.getvalue()
    # (Rewritten only when it changes: an unchanged one keeps its compiled object.)
    if not info.exists() or info.read_text() != text:
        info.write_text(text)
    print(f"build-module: {len(ordered)} functions, {sum(1 for v in slot_function if v)} entries, table at 0x{base:x} ({slots} slots)"
          + (f"; {len(missing)} table names without a generated function (not used)" if missing else ""))

    # Compile.
    objects_dir = gen / "obj"
    objects_dir.mkdir(exist_ok=True)
    # -ffp-contract=off: no multiply-add fused into one rounding. The PS2 rounds each, and PCSX2's
    # recompiler, which runs the code a module does not have, uses separate SSE operations.
    common = [args.opt, "-march=znver2", "-ffp-contract=off", "-fPIC", "-fvisibility=hidden", "-fno-exceptions", "-fno-asynchronous-unwind-tables",
              "-fno-stack-protector", "-ffunction-sections", "-I", str(RUNTIME), "-I", str(gen), "-w"]
    cxx = [args.cxx, "-std=c++20", "-fno-rtti", "-fno-threadsafe-statics"] + common

    # An object newer than its source and than Crux2's runtime headers is kept from the last build.
    headers = max(p.stat().st_mtime for p in list(RUNTIME.glob("*.h")) + list(RUNTIME.glob("upstream/*.h")))

    # The runtime's headers, parsed once for every generated file (a precompiled header): each
    # file includes them, and they are most of what a small file costs to compile.
    # (What a generated file starts with, in its order: <stdexcept> before the runtime's macros.)
    pch_source = objects_dir / "crux2_runtime_pch.h"
    pch_text = '#include <stdexcept>\n#include "ps2_runtime_macros.h"\n#include "ps2_runtime.h"\n'
    if not pch_source.exists() or pch_source.read_text() != pch_text:
        pch_source.write_text(pch_text)
    pch = objects_dir / "crux2_runtime.pch"
    if not pch.exists() or pch.stat().st_mtime <= max(headers, pch_source.stat().st_mtime):
        result = subprocess.run(cxx + ["-x", "c++-header", str(pch_source), "-o", str(pch)],
                                capture_output=True, text=True)
        if result.returncode != 0:
            print("build-module: no precompiled header (compiling without):\n" + result.stderr[-2000:], flush=True)
            pch.unlink(missing_ok=True)
    generated = cxx + (["-include-pch", str(pch)] if pch.exists() else [])
    jobs = [(src, objects_dir / (src.stem + ".o"), generated) for src in sources]
    jobs += [(src, objects_dir / (src.stem + ".o"), cxx) for src in [gen / "register_functions.cpp", info, RUNTIME / "crux2_module.cpp"]]
    jobs.append((RUNTIME / "crux2_module_libc.c", objects_dir / "crux2_module_libc.o",
                 [args.cc, "-std=c11", "-fno-builtin", "-ffreestanding"] + common))

    def compile_one(job):
        src, obj, command = job
        if obj.exists() and obj.stat().st_mtime > max(src.stat().st_mtime, headers):
            return None
        result = subprocess.run(command + ["-c", str(src), "-o", str(obj)], capture_output=True, text=True)
        return None if result.returncode == 0 else f"{src.name}:\n{result.stderr[-4000:]}"

    failures = []
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for i, failure in enumerate(pool.map(compile_one, jobs)):
            if failure:
                failures.append(failure)
            if (i + 1) % 200 == 0:
                print(f"build-module: compiled {i + 1} of {len(jobs)}", flush=True)
    if failures:
        sys.stderr.write("\n".join(failures[:5]) + f"\nbuild-module: {len(failures)} file(s) did not compile\n")
        return 1

    link = [args.cxx, "-shared", "-nostdlib", "-fuse-ld=lld", "-Wl,-z,defs", "-Wl,-Bsymbolic", "-Wl,--gc-sections",
            "-Wl,--hash-style=sysv", "-o", str(args.out)] + [str(obj) for _, obj, _ in jobs]
    result = subprocess.run(link, capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(result.stderr[-4000:])
        return 1
    problem = check_module(args.out)
    if problem:
        sys.stderr.write(f"build-module: {args.out} cannot be loaded by Crux2: {problem}\n")
        return 1
    print(f"build-module: {args.out} ({args.out.stat().st_size // 1024} KiB): exports Crux2's five functions, imports nothing")
    return 0


if __name__ == "__main__":
    sys.exit(main())
