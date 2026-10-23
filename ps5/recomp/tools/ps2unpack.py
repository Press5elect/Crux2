#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - a packed PS2 executable, unpacked: the game's code as it is once its own decompressor has
# run. Some games ship their executable compressed: a small decompressor, the compressed game, and
# an empty segment the game is unpacked into at start. A recompiler given such a file only sees the
# decompressor, and the game then jumps into code the module does not have (a black screen). This
# runs the decompressor in an emulator (Unicorn's MIPS64) until it jumps into the empty segment, and
# writes the memory there out as a new executable that starts where the decompressor jumped.
#
#   ps2unpack.py check   SLUS_123.45             is it packed?
#   ps2unpack.py unpack  SLUS_123.45 OUT          the unpacked executable as OUT
#
# Needs the unicorn package (pip install unicorn) for unpacking; the check needs nothing.
import argparse
import struct
import sys
from pathlib import Path

PT_LOAD = 1


class UnpackError(Exception):
    pass


def segments(data: bytes):
    """(vaddr, offset, filesz, memsz, flags) of the loadable segments, and the entry point."""
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        raise UnpackError("not a 32-bit little-endian ELF file (a PS2 executable)")
    entry, phoff = struct.unpack_from("<II", data, 24)
    phentsize, phnum = struct.unpack_from("<HH", data, 42)
    out = []
    for i in range(phnum):
        p_type, offset, vaddr, _, filesz, memsz, flags, _ = struct.unpack_from("<8I", data, phoff + i * phentsize)
        if p_type == PT_LOAD and memsz:
            out.append((vaddr, offset, filesz, memsz, flags))
    return out, entry


def packed_target(data: bytes):
    """The empty segment a packed executable unpacks into (vaddr, memsz), or None.
    Packed: a segment with no bytes in the file, large (64 KiB or more), below 32 MiB, and the entry
    point in another segment that has bytes."""
    segs, entry = segments(data)
    home = next((s for s in segs if s[0] <= entry < s[0] + s[3]), None)
    if home is None or home[2] == 0:
        return None
    for vaddr, _, filesz, memsz, _ in segs:
        if filesz == 0 and memsz >= 0x10000 and vaddr + memsz <= 0x2000000 and not (vaddr <= entry < vaddr + memsz):
            return vaddr, memsz
    return None


def _stub_patches(image: bytearray, base: int, entry: int):
    """The decompressor's instructions the emulator lacks, made ones it has (only from the entry
    point to the first long run of zero words, where the decompressor's code ends):
    LQ/SQ (the R5900's 128-bit moves) as LD/SD, PREF (a hint) as nothing, SYSCALL (FlushCache and
    the like: the emulator has no kernel and no caches) as v0 = 0, and LUI 0x7000 (the scratchpad,
    past the 512 MiB the emulator maps) as LUI 0x01F0 (RAM no game is unpacked into)."""
    pos = entry - base
    zeros = 0
    while 0 <= pos < len(image) - 3 and zeros < 16:
        w = struct.unpack_from("<I", image, pos)[0]
        zeros = zeros + 1 if w == 0 else 0
        op = w >> 26
        if op == 0x1E:
            w = (w & 0x03FFFFFF) | (0x37 << 26)
        elif op == 0x1F:
            w = (w & 0x03FFFFFF) | (0x3F << 26)
        elif op == 0x33:
            w = 0
        elif (w & 0xFC00003F) == 0x0000000C:
            w = 0x24020000  # addiu v0, zero, 0
        elif op == 0x0F and (w & 0xFFFF) == 0x7000:
            w = (w & 0xFFFF0000) | 0x01F0
        struct.pack_into("<I", image, pos, w)
        pos += 4


def unpack(data: bytes, log=print) -> bytes:
    target = packed_target(data)
    if target is None:
        raise UnpackError("not packed (no empty segment the game unpacks into)")
    try:
        from unicorn import (Uc, UcError, UC_ARCH_MIPS, UC_MODE_MIPS64, UC_MODE_LITTLE_ENDIAN, UC_HOOK_BLOCK, UC_HOOK_INTR,
                             UC_HOOK_MEM_WRITE)
        from unicorn.mips_const import UC_MIPS_REG_CP0_STATUS, UC_MIPS_REG_PC, UC_MIPS_REG_SP
    except ImportError:
        raise UnpackError(f"unpacking needs the unicorn package: {Path(sys.executable).name} -m pip install unicorn") from None
    segs, entry = segments(data)
    lo, size = target
    hi = lo + size

    mu = Uc(UC_ARCH_MIPS, UC_MODE_MIPS64 | UC_MODE_LITTLE_ENDIAN)
    # Kernel mode with 64-bit operations allowed (the R5900 has them everywhere).
    mu.reg_write(UC_MIPS_REG_CP0_STATUS, mu.reg_read(UC_MIPS_REG_CP0_STATUS) | 0xE0)
    mu.mem_map(0, 32 << 20)
    for vaddr, offset, filesz, memsz, _ in segs:
        if vaddr + memsz > (32 << 20):
            continue
        image = bytearray(data[offset:offset + filesz])
        if vaddr <= entry < vaddr + filesz:
            _stub_patches(image, vaddr, entry)
        mu.mem_write(vaddr, bytes(image))
    mu.reg_write(UC_MIPS_REG_SP, 0x01FF0000)

    state = {"jump": None, "error": None}

    def block(uc, address, size, user):
        if lo <= address < hi:
            state["jump"] = address
            uc.emu_stop()

    def interrupt(uc, number, user):
        pc = uc.reg_read(UC_MIPS_REG_PC)
        state["error"] = f"the decompressor stopped at 0x{pc:x} (exception {number}: an instruction the emulator does not have?)"
        uc.emu_stop()

    mu.hook_add(UC_HOOK_BLOCK, block)
    mu.hook_add(UC_HOOK_INTR, interrupt)

    # Where the target overlaps the packed file's own bytes (its compressed data, its decompressor),
    # what the decompressor did not write is left over, not the game's: the writes there are watched.
    overlap = min((v for v, _, fs, _, _ in segs if fs and lo < v < hi), default=hi)
    state["top"] = overlap

    def wrote(uc, access, address, size, value, user):
        state["top"] = max(state["top"], address + size)

    if overlap < hi:
        mu.hook_add(UC_HOOK_MEM_WRITE, wrote, begin=overlap, end=hi - 1)
    log(f"    unpacking: running its decompressor (entry 0x{entry:x}) until it jumps into 0x{lo:x}-0x{hi:x}")
    try:
        mu.emu_start(entry, 0xFFFFFFFF, count=4_000_000_000)
    except UcError as e:
        if state["jump"] is None:
            raise UnpackError(f"the decompressor failed at 0x{mu.reg_read(UC_MIPS_REG_PC):x}: {e}") from None
    if state["jump"] is None:
        raise UnpackError(state["error"] or "the decompressor never jumped into the game")
    memory = bytes(mu.mem_read(lo, state["top"] - lo)) + bytes(hi - state["top"])
    filesz = len(memory.rstrip(b"\x00"))
    filesz = (filesz + 15) & ~15
    log(f"    unpacked: {filesz // 1024} KiB of code and data at 0x{lo:x}, starting at 0x{state['jump']:x}")
    return make_elf(state["jump"], lo, memory[:filesz], size, struct.unpack_from("<I", data, 36)[0])


def make_elf(entry: int, vaddr: int, image: bytes, memsz: int, flags: int) -> bytes:
    """A PS2 executable of one segment (read, write, execute), as the packed one's header describes."""
    ehsize, phentsize = 52, 32
    offset = 0x1000
    header = bytearray(ehsize)
    header[0:16] = b"\x7fELF\x01\x01\x01" + bytes(9)
    struct.pack_into("<HHIIIIIHHHHHH", header, 16,
                     2, 8, 1, entry, ehsize, 0, flags, ehsize, phentsize, 1, 40, 0, 0)
    ph = struct.pack("<8I", PT_LOAD, offset, vaddr, vaddr, len(image), memsz, 7, 0x10)
    out = bytearray(offset)
    out[0:ehsize] = header
    out[ehsize:ehsize + phentsize] = ph
    return bytes(out) + image


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="A packed PS2 executable, unpacked.")
    sub = parser.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("check")
    p.add_argument("elf", type=Path)
    p = sub.add_parser("unpack")
    p.add_argument("elf", type=Path)
    p.add_argument("out", type=Path)
    args = parser.parse_args(argv)
    try:
        data = args.elf.read_bytes()
        target = packed_target(data)
        if args.cmd == "check":
            print(f"{args.elf.name}: " + (f"packed (unpacks into 0x{target[0]:x}-0x{target[0] + target[1]:x})" if target else "not packed"))
            return 0
        args.out.write_bytes(unpack(data))
        print(args.out)
    except (UnpackError, OSError) as e:
        print(f"ps2unpack: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
