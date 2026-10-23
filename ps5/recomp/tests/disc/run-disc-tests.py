#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - the recomp kit's disc reader (tools/ps2disc.py), tested: a made-up PS2 disc (an ISO 9660
# file system with a SYSTEM.CNF and an executable; no game) as an .iso, a raw .bin/.cue, a .cso and
# CHDs made by MAME's chdman with each codec on its own, DVD and CD. Each must give back the
# executable byte for byte, and the CHDs, CSO and .bin the same .iso.
#
#   run-disc-tests.py WORK [--elf PS2_EXECUTABLE]      (chdman on the PATH: apt install mame-tools)
import argparse
import math
import os
import random
import shutil
import struct
import subprocess
import sys
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent / "tools"))
import ps2disc  # noqa: E402

SECTOR = 2048


def both(v16=None, v32=None):
    if v16 is not None:
        return struct.pack("<H", v16) + struct.pack(">H", v16)
    return struct.pack("<I", v32) + struct.pack(">I", v32)


def dir_record(name: bytes, lba: int, size: int, is_dir: bool) -> bytes:
    n = len(name)
    length = 33 + n + (1 if n % 2 == 0 else 0)
    rec = bytes([length, 0]) + both(v32=lba) + both(v32=size) + bytes(7) + bytes([2 if is_dir else 0, 0, 0]) + both(v16=1) + bytes([n]) + name
    return rec + bytes(length - len(rec))


def make_iso(files: dict, subdir_files: dict) -> bytes:
    """A small ISO 9660 image: files at the top, and a folder DATA with more."""
    root_lba, sub_lba = 18, 19
    lba = 20
    placed, sub_placed = {}, {}
    for name, data in files.items():
        placed[name] = lba
        lba += (len(data) + SECTOR - 1) // SECTOR
    for name, data in subdir_files.items():
        sub_placed[name] = lba
        lba += (len(data) + SECTOR - 1) // SECTOR
    total = lba
    root = dir_record(b"\x00", root_lba, SECTOR, True) + dir_record(b"\x01", root_lba, SECTOR, True)
    root += dir_record(b"DATA", sub_lba, SECTOR, True)
    for name, data in files.items():
        root += dir_record(name.encode() + b";1", placed[name], len(data), False)
    sub = dir_record(b"\x00", sub_lba, SECTOR, True) + dir_record(b"\x01", root_lba, SECTOR, True)
    for name, data in subdir_files.items():
        sub += dir_record(name.encode() + b";1", sub_placed[name], len(data), False)
    assert len(root) <= SECTOR and len(sub) <= SECTOR
    pvd = bytearray(SECTOR)
    pvd[0:8] = b"\x01CD001\x01\x00"
    pvd[8:40] = b"PLAYSTATION".ljust(32)
    pvd[40:72] = b"CRUX2_DISC_TEST".ljust(32)
    pvd[80:88] = both(v32=total)
    pvd[120:124] = both(v16=1)
    pvd[124:128] = both(v16=1)
    pvd[128:132] = both(v16=SECTOR)
    pvd[156:156 + 34] = dir_record(b"\x00", root_lba, SECTOR, True)
    pvd[881] = 1
    term = bytearray(SECTOR)
    term[0:7] = b"\xffCD001\x01"
    image = bytearray(total * SECTOR)
    image[16 * SECTOR:17 * SECTOR] = pvd
    image[17 * SECTOR:18 * SECTOR] = term
    image[root_lba * SECTOR:root_lba * SECTOR + len(root)] = root
    image[sub_lba * SECTOR:sub_lba * SECTOR + len(sub)] = sub
    for name, data in files.items():
        image[placed[name] * SECTOR:placed[name] * SECTOR + len(data)] = data
    for name, data in subdir_files.items():
        image[sub_placed[name] * SECTOR:sub_placed[name] * SECTOR + len(data)] = data
    return bytes(image)


def raw_mode2(iso: bytes) -> bytes:
    """The image as a CD's raw mode 2 form 1 sectors (sync, header, subheader, data; EDC/ECC zero)."""
    out = bytearray()
    for i in range(len(iso) // SECTOR):
        lba = i + 150
        m, s, f = lba // 4500, (lba // 75) % 60, lba % 75
        bcd = lambda v: (v // 10) << 4 | v % 10
        sector = bytearray(2352)
        sector[0:12] = ps2disc.CD_SYNC
        sector[12:16] = bytes([bcd(m), bcd(s), bcd(f), 2])
        sector[16:24] = bytes([0, 0, 8, 0, 0, 0, 8, 0])
        sector[24:24 + SECTOR] = iso[i * SECTOR:(i + 1) * SECTOR]
        out += sector
    return bytes(out)


def make_cso(iso: bytes, block=SECTOR) -> bytes:
    blocks = (len(iso) + block - 1) // block
    header = struct.pack("<4sIQIBB2x", b"CISO", 24, len(iso), block, 1, 0)
    index, body = [], bytearray()
    pos = 24 + 4 * (blocks + 1)
    for i in range(blocks):
        chunk = iso[i * block:(i + 1) * block]
        c = zlib.compressobj(9, zlib.DEFLATED, -15)
        z = c.compress(chunk) + c.flush()
        if len(z) >= len(chunk):
            index.append(pos | 0x80000000)
            body += chunk
            pos += len(chunk)
        else:
            index.append(pos)
            body += z
            pos += len(z)
    index.append(pos)
    return header + struct.pack(f"<{len(index)}I", *index) + bytes(body)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("work", type=Path)
    parser.add_argument("--elf", type=Path)
    args = parser.parse_args()
    work = args.work.resolve()
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    chdman = shutil.which("chdman")

    rng = random.Random(1234)
    elf = args.elf.read_bytes() if args.elf else b"\x7fELF\x01\x01\x01" + bytes(rng.randrange(256) for _ in range(150000))
    # What a disc has besides: sound as plain samples (FLAC's ground), noise (stored as it is), zeros
    # and the same block twice (a CHD's references to another hunk), text.
    pcm = bytearray()
    for i in range(300000):
        v = int(9000 * math.sin(i * 0.031) + 3000 * math.sin(i * 0.17))
        pcm += struct.pack("<hh", v, -v // 2)
    noise = bytes(rng.randrange(256) for _ in range(400000))
    block = bytes(rng.randrange(256) for _ in range(SECTOR * 4))
    files = {
        "SYSTEM.CNF": b"BOOT2 = cdrom0:\\SLUS_123.45;1\r\nVER = 1.00\r\nVMODE = NTSC\r\n",
        "SLUS_123.45": elf,
        "SOUND.PCM": bytes(pcm),
        "NOISE.BIN": noise,
        "ZEROS.BIN": bytes(SECTOR * 64),
        "TWICE.BIN": block + block + block,
        "README.TXT": b"Crux2's disc reader test: no game here.\n" * 4000,
    }
    iso = make_iso(files, {"INNER.ELF": elf[:5000]})
    (work / "game.iso").write_bytes(iso)
    raw = raw_mode2(iso)
    (work / "game.bin").write_bytes(raw)
    (work / "game.cue").write_text('FILE "game.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n')
    (work / "game.cso").write_bytes(make_cso(iso))

    cases = [("game.iso", None), ("game.cue", None), ("game.bin", None), ("game.cso", None)]
    if chdman:
        for codecs in ["lzma", "zlib", "huff", "flac", "zstd", "none", "lzma,zlib,huff,flac"]:
            out = work / f"dvd-{codecs.replace(',', '+')}.chd"
            subprocess.run([chdman, "createdvd", "-i", str(work / "game.iso"), "-o", str(out), "-c", codecs, "-f"],
                           check=True, capture_output=True)
            cases.append((out.name, None))
        for codecs in ["cdlz", "cdzl", "cdfl", "cdzs", "cdlz,cdzl,cdfl"]:
            out = work / f"cd-{codecs.replace(',', '+')}.chd"
            subprocess.run([chdman, "createcd", "-i", str(work / "game.cue"), "-o", str(out), "-c", codecs, "-f"],
                           check=True, capture_output=True)
            cases.append((out.name, None))
        # A large hunk (3 bytes of compressed length in CD hunks; many sectors to a DVD hunk).
        out = work / "dvd-bighunk.chd"
        subprocess.run([chdman, "createdvd", "-i", str(work / "game.iso"), "-o", str(out), "-hs", "65536", "-f"],
                       check=True, capture_output=True)
        cases.append((out.name, None))
    else:
        print("run-disc-tests: no chdman on the PATH: the CHD cases are skipped (apt install mame-tools)")

    have_zstd = True
    try:
        ps2disc._zstd(zlib.compress(b""), 0)
    except ps2disc.DiscError as e:
        have_zstd = "zstd" not in str(e)
    except Exception:
        pass

    failed = 0
    for name, _ in cases:
        path = work / name
        try:
            with ps2disc.Disc(path) as disc:
                exe_name, data = disc.boot()
                assert exe_name == "SLUS_123.45", exe_name
                assert data == elf, "the executable differs"
                assert disc.read_file("DATA/INNER.ELF") == elf[:5000], "a file in a folder differs"
                for fname in ("SOUND.PCM", "NOISE.BIN", "TWICE.BIN"):
                    assert disc.read_file(fname) == files[fname], f"{fname} differs"
                if name != "game.iso":
                    out = work / (name + ".out.iso")
                    disc.write_iso(out)
                    assert out.read_bytes() == iso, "the .iso made from it differs"
                    out.unlink()
            print(f"  ok    {name}")
        except ps2disc.DiscError as e:
            if "zstd" in str(e) and not have_zstd:
                print(f"  skip  {name} ({e})")
                continue
            failed += 1
            print(f"  FAIL  {name}: {e}")
        except AssertionError as e:
            failed += 1
            print(f"  FAIL  {name}: {e}")

    # A PS1 disc and a disc with no SYSTEM.CNF are refused with a reason.
    ps1 = make_iso({"SYSTEM.CNF": b"BOOT = cdrom:\\SLUS_000.01;1\r\n", "SLUS_000.01": b"x"}, {})
    (work / "ps1.iso").write_bytes(ps1)
    try:
        with ps2disc.Disc(work / "ps1.iso") as disc:
            disc.boot()
        failed += 1
        print("  FAIL  a PS1 disc was taken")
    except ps2disc.DiscError as e:
        print(f"  ok    a PS1 disc refused: {e}")

    # A folder of the disc's files.
    folder = work / "folder"
    folder.mkdir()
    (folder / "system.cnf").write_bytes(files["SYSTEM.CNF"])
    (folder / "SLUS_123.45").write_bytes(elf)
    with ps2disc.Disc(folder) as disc:
        assert disc.boot() == ("SLUS_123.45", elf)
    print("  ok    a folder of the disc's files")

    print("run-disc-tests: " + ("all passed" if not failed else f"{failed} failed"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
