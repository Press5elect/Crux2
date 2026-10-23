#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - the recomp kit's unpacker (tools/ps2unpack.py), tested: a PS2 executable is "packed" the
# way games pack theirs (an empty segment for the game, the game's bytes and a small copier in a
# segment of their own, the entry point in the copier), with the instructions the unpacker has to
# stand in for (SQ, LQ, PREF, a scratchpad address, a SYSCALL), and must come back as it was.
#
#   run-unpack-test.py PS2_EXECUTABLE         (needs the unicorn package)
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent / "tools"))
import ps2unpack  # noqa: E402


def i_type(op, rs, rt, imm):
    return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


def main() -> int:
    original = Path(sys.argv[1]).read_bytes()
    segs, entry = ps2unpack.segments(original)
    vaddr, offset, filesz, memsz, _ = segs[0]
    game = original[offset:offset + filesz]
    stub_base = (vaddr + memsz + 0xFFFF) & ~0xFFFF
    data_addr = stub_base
    code_addr = stub_base + ((len(game) + 0xFFF) & ~0xFFF)
    T0, T1, T2, T3, T9, SP, GP = 8, 9, 10, 11, 25, 29, 28
    code = [
        i_type(0x0F, 0, GP, 0x7000),                       # lui gp, 0x7000  (the scratchpad)
        i_type(0x0F, 0, SP, 0x01F0),                       # lui sp, 0x01f0
        i_type(0x1F, SP, GP, -16),                         # sq gp, -16(sp)
        i_type(0x1E, SP, GP, -16),                         # lq gp, -16(sp)
        i_type(0x0F, 0, T0, data_addr >> 16), i_type(0x0D, T0, T0, data_addr),
        i_type(0x0F, 0, T1, vaddr >> 16), i_type(0x0D, T1, T1, vaddr),
        i_type(0x0F, 0, T2, len(game) >> 16), i_type(0x0D, T2, T2, len(game)),
        # loop:
        i_type(0x33, T0, 0, 64),                           # pref 0, 64(t0)
        i_type(0x23, T0, T3, 0),                           # lw t3, 0(t0)
        i_type(0x2B, T1, T3, 0),                           # sw t3, 0(t1)
        i_type(0x09, T0, T0, 4), i_type(0x09, T1, T1, 4),
        i_type(0x09, T2, T2, -4),
        i_type(0x05, T2, 0, -7),                           # bne t2, zero, loop
        0,
        i_type(0x09, 0, 3, 100),                           # li v1, 100 (FlushCache)
        0x0000000C,                                        # syscall
        i_type(0x0F, 0, T9, entry >> 16), i_type(0x0D, T9, T9, entry),
        (T9 << 21) | 0x08,                                 # jr t9
        0,
    ]
    stub = game + bytes((code_addr - data_addr) - len(game)) + b"".join(struct.pack("<I", w) for w in code) + bytes(64)
    ehsize, phentsize = 52, 32
    header = bytearray(ehsize)
    header[0:16] = b"\x7fELF\x01\x01\x01" + bytes(9)
    struct.pack_into("<HHIIIIIHHHHHH", header, 16, 2, 8, 1, code_addr, ehsize, 0, 0x20924000, ehsize, phentsize, 2, 40, 0, 0)
    body_off = 0x1000
    phs = struct.pack("<8I", 1, 0, vaddr, vaddr, 0, memsz, 7, 0x10) + struct.pack("<8I", 1, body_off, stub_base, stub_base, len(stub), len(stub), 7, 0x10)
    packed = bytes(header) + phs + bytes(body_off - ehsize - len(phs)) + stub

    assert ps2unpack.packed_target(original) is None, "the original was taken as packed"
    assert ps2unpack.packed_target(packed) == (vaddr, memsz), "the packed one was not found packed"
    out = ps2unpack.unpack(packed, log=lambda *_: None)
    segs2, entry2 = ps2unpack.segments(out)
    assert entry2 == entry, f"entry 0x{entry2:x}, not 0x{entry:x}"
    v2, o2, f2, m2, _ = segs2[0]
    assert (v2, m2) == (vaddr, memsz), "the segment differs"
    assert out[o2:o2 + f2] == game.rstrip(b"\x00").ljust(f2, b"\x00")[:f2], "the game's bytes differ"
    print("run-unpack-test: passed (unpacked back to the original's code, entry and size)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
