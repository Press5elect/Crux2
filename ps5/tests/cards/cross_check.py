#!/usr/bin/env python3
# Crux2 - the memory card code held against mymc+, another program for the same cards.
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0+
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
#
#   cross_check.py <mc_test> <python with mymc+ installed> <work folder>
#
# mymc+ (Florian Märkl's continuation of Ross Ridge's mymc, GPL-3.0-or-later) is run as a program
# and asked the same things as the code here; nothing of it is in the title. What is compared:
#
#   * an empty card made here is, byte for byte, the card mymc+ makes;
#   * a save written here is found sound, listed and read out the same by mymc+, and the other way;
#   * a save taken off here leaves a card mymc+ finds sound, with the room it says;
#   * .psu and .max files written by mymc+ are read here, and .psu files written here by mymc+;
#   * .cbs, .sps and .psv files made here are read the same by both.
#
# Without mymc+ (pip install mymcplus) nothing of this can run: the script says so and passes
# nothing off as checked.

import hashlib
import os
import random
import shutil
import struct
import subprocess
import sys
import zlib

checks = 0
failed = 0


def check(what, ok, detail=""):
    global checks, failed
    checks += 1
    if not ok:
        failed += 1
        print("FAILED: %s %s" % (what, detail))


def main():
    if len(sys.argv) != 4:
        print(__doc__ or "usage: cross_check.py <mc_test> <python> <work>")
        return 2
    tool, python, work = sys.argv[1], sys.argv[2], os.path.abspath(sys.argv[3])
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    rng = random.Random(7)
    stamp = "0038220c070aea07"  # 2026-10-07 12:34:56 as a card keeps it

    def mine(*args, ok=(0,)):
        r = subprocess.run([tool] + list(args), capture_output=True, text=True)
        if r.returncode not in ok:
            raise RuntimeError("mc_test %s: %d\n%s%s" % (" ".join(args), r.returncode, r.stdout, r.stderr))
        return r.stdout

    def theirs(card, *args, ok=(0,), cwd=None):
        r = subprocess.run([python, "-m", "mymcplus", card] + list(args), capture_output=True, text=True, cwd=cwd or work)
        if r.returncode not in ok:
            raise RuntimeError("mymc+ %s: %d\n%s%s" % (" ".join(args), r.returncode, r.stdout, r.stderr))
        return r.stdout + r.stderr

    def path(name):
        return os.path.join(work, name)

    def data(name):
        with open(path(name), "rb") as f:
            return f.read()

    def icon_sys(title, first):
        out = bytearray(964)
        out[0:4] = b"PS2D"
        struct.pack_into("<H", out, 6, first * 2)
        wide = "".join(chr(0xFF21 + ord(c) - 65) if "A" <= c <= "Z" else chr(0xFF41 + ord(c) - 97) if "a" <= c <= "z"
                       else chr(0xFF10 + ord(c) - 48) if "0" <= c <= "9" else "　" for c in title).encode("shift_jis")
        out[0xC0:0xC0 + len(wide)] = wide
        return bytes(out)

    def make_files(title, sizes):
        files = [("icon.sys", icon_sys(title, title.find(" ") if " " in title else len(title)))]
        for n, size in enumerate(sizes):
            half = size // 2
            files.append(("DATA%d.BIN" % n, rng.randbytes(half) + bytes((i // 7) & 0xFF for i in range(size - half))))
        return files

    def their_add(card, name, files):
        folder = path("src-" + name)
        os.makedirs(folder, exist_ok=True)
        theirs(card, "mkdir", name)
        for file_name, content in files:
            with open(os.path.join(folder, file_name), "wb") as f:
                f.write(content)
        theirs(card, "add", "-d", name, *[file_name for file_name, _ in files], cwd=folder)

    def their_files(card, name, files):
        """Whether mymc+ reads each file of a save as it should be."""
        for file_name, content in files:
            out = path("x-" + hashlib.md5((card + name + file_name).encode()).hexdigest())
            theirs(card, "extract", "-d", name, "-o", out, file_name)
            with open(out, "rb") as f:
                if f.read() != content:
                    return False
            os.remove(out)
        return True

    def fnv(content):
        h = 2166136261
        for b in content:
            h = ((h ^ b) * 16777619) & 0xFFFFFFFF
        return "%08x" % h

    def my_files(dump, name, files):
        """Whether the listing mc_test printed of a save is that save."""
        lines = [line.split("\t") for line in dump.splitlines()]
        saves = [l for l in lines if l[0] == "save"]
        got = [(l[1], int(l[3]), l[4]) for l in lines if l[0] == "file"]
        want = [(file_name, len(content), fnv(content)) for file_name, content in files]
        return len(saves) == 1 and saves[0][1] == name and got == want

    # ---------------------------------------------------------------- an empty card, byte for byte
    theirs(path("theirs.ps2"), "format")
    card = data("theirs.ps2")
    # (The time mymc+ wrote into the top directory: the card here is made at the same moment.)
    root = 41 * 2 * 528
    their_stamp = card[root + 8:root + 16].hex()
    mine("format", path("mine.ps2"), "ecc", their_stamp)
    made = data("mine.ps2")
    check("an empty card is mymc+'s, byte for byte", made == card,
          "" if made == card else "(first difference at %d)" % next((i for i in range(min(len(card), len(made))) if card[i] != made[i]), -1))
    theirs(path("theirs-plain.ps2"), "format", "-e")
    card = data("theirs-plain.ps2")
    mine("format", path("mine-plain.ps2"), "plain", card[41 * 2 * 512 + 8:41 * 2 * 512 + 16].hex())
    made = data("mine-plain.ps2")
    # (mymc+ erases the block the card keeps for itself where it would be on a card with the
    # error-correction bytes, which is past the end of one without: its file is longer by that,
    # and the block's true place is left as zeros. Everything else has to be the same.)
    block = 1022 * 16 * 512
    check("a card without error-correction bytes is as long as it says", len(made) == 16384 * 512 and len(card) == 1022 * 16 * 528 + 16 * 528,
          "%d, %d" % (len(made), len(card)))
    check("and is mymc+'s outside the block the card keeps for itself",
          made[:block] == card[:block] and made[block + 16 * 512:] == card[block + 16 * 512:len(made)]
          and made[block:block + 16 * 512] == b"\xff" * (16 * 512))
    check("mymc+ finds the card made here sound", "No errors found" in theirs(path("mine.ps2"), "check"))
    # (The larger cards PCSX2 makes: 16, 32 and 64 MB.)
    for megabytes in (16, 32, 64):
        theirs(path("theirs-%d.ps2" % megabytes), "format", "-c", str(megabytes * 1024))
        card = data("theirs-%d.ps2" % megabytes)
        offset = struct.unpack_from("<L", card, 0x34)[0] * 2 * 528
        mine("format", path("mine-%d.ps2" % megabytes), "ecc%d" % megabytes, card[offset + 8:offset + 16].hex())
        check("an empty card of %d MB is mymc+'s, byte for byte" % megabytes, data("mine-%d.ps2" % megabytes) == card)

    # -------------------------------------------- saves written by mymc+, read here; .psu and .max
    shapes = [[], [0], [1], [1023], [1024], [1025], [2048, 0, 5], [100000, 3, 70000], [4096] * 4, [300000]]
    saves = []
    for n, sizes in enumerate(shapes):
        name = "BESLES-5%04dSAVE" % n
        files = make_files("Save Number %d" % n, sizes)
        their_add(path("theirs.ps2"), name, files)
        saves.append((name, files))
    listing = mine("ls", path("theirs.ps2")).splitlines()
    check("the saves mymc+ wrote are all listed here", [l.split("\t")[0] for l in listing[:-1]] == [name for name, _ in saves], listing)
    for n, line in enumerate(listing[:-1]):
        fields = line.split("\t")
        check("with their titles", fields[1] == "Save Number %d" % n, fields[1])
        check("and their numbers of files", int(fields[3]) == len(saves[n][1]))
    their_free = int(theirs(path("theirs.ps2"), "df").split(":")[1].split()[0])
    check("the room left is what mymc+ says", listing[-1].split("\t")[1] == str(their_free), "%s against %d" % (listing[-1], their_free))
    for name, files in saves:
        dump = mine("export", path("theirs.ps2"), name, path(name + ".mine.psu"))
        check("a save mymc+ wrote reads the same here (%s)" % name, my_files(dump, name, files))
        theirs(path("theirs.ps2"), "export", "-o", path(name + ".psu"), name)
        check("and its .psu is mymc+'s, byte for byte", data(name + ".mine.psu") == data(name + ".psu"))
        check("a .psu written here says of itself what it is", "format\tEMS (.psu)" in mine("dump", path(name + ".mine.psu"), stamp))
        theirs(path("theirs.ps2"), "export", "-m", "-o", path(name + ".max"), name)
        dump = mine("dump", path(name + ".max"), stamp)
        check("mymc+'s .max of it reads here", "format\tMax Drive (.max)" in dump and my_files(dump, name, files), dump[:200])
        dump = mine("dump", path(name + ".psu"), stamp)
        check("mymc+'s .psu of it reads here", "format\tEMS (.psu)" in dump and my_files(dump, name, files))

    # ------------------------------------------- saves written here, read by mymc+; every format
    for n, (name, files) in enumerate(saves):
        kind = ["psu", "max"][n % 2]
        mine("import", path("mine.ps2"), path(name + "." + kind), stamp)
        check("mymc+ finds the card sound after a save was written here (%s, from .%s)" % (name, kind),
              "No errors found" in theirs(path("mine.ps2"), "check"))
        check("and reads its files as they were", their_files(path("mine.ps2"), name, files))
    check("the error-correction bytes are right", "ecc-mismatches\t0" in mine("check", path("mine.ps2")))
    check("mymc+ lists them as it lists its own", theirs(path("mine.ps2"), "dir") == theirs(path("theirs.ps2"), "dir"),
          "\n" + theirs(path("mine.ps2"), "dir") + "\n--\n" + theirs(path("theirs.ps2"), "dir"))
    check("a save of the same name is refused, the card untouched",
          "already" in mine("import", path("mine.ps2"), path(saves[0][0] + ".psu"), stamp, ok=(3,)))
    for n, (name, files) in enumerate(saves):
        if n % 2 == 0:
            theirs(path("mine.ps2"), "export", "-o", path(name + ".back.psu"), name)
            check("its .psu out of mymc+ is the one that went in", plain_psu(data(name + ".back.psu")) == plain_psu(data(name + ".psu")))

    # (The plain card: written here, read there.)
    for name, files in saves[:4]:
        mine("import", path("mine-plain.ps2"), path(name + ".psu"), stamp)
        check("mymc+ reads a save written to a card without error-correction bytes", their_files(path("mine-plain.ps2"), name, files))
    check("and finds that card sound", "No errors found" in theirs(path("mine-plain.ps2"), "check"))
    for megabytes in (16, 64):
        for name, files in saves[6:9]:
            mine("import", path("mine-%d.ps2" % megabytes), path(name + ".psu"), stamp)
            check("mymc+ reads a save written to a card of %d MB" % megabytes, their_files(path("mine-%d.ps2" % megabytes), name, files))
        check("and finds that card sound", "No errors found" in theirs(path("mine-%d.ps2" % megabytes), "check"))

    # ------------------------------------------------- the other files: made here, read by both
    def tod(year, month, day, hour, minute, second):
        return struct.pack("<BBBBBBH", 0, second, minute, hour, day, month, year)

    dir_mode, file_mode = 0x8427, 0x8497
    created, modified = tod(2004, 3, 2, 1, 2, 3), tod(2005, 6, 7, 8, 9, 10)
    key = data_of_key(tool)

    def rc4(body):
        s = bytearray(key)
        j = 0
        out = bytearray(body)
        for n in range(len(out)):
            i = (n + 1) & 0xFF
            j = (j + s[i]) & 0xFF
            s[i], s[j] = s[j], s[i]
            out[n] ^= s[(s[i] + s[j]) & 0xFF]
        return bytes(out)

    def make_cbs(name, files, whole):
        plain = b"".join(struct.pack("<8s8sLHHLL32s", created, modified, len(content), file_mode, 0, 0, 0, file_name.encode()) + content
                         for file_name, content in files)
        body = rc4(zlib.compress(plain, 9))
        title = b"A title".ljust(32, b"\0")
        header_length = 92 + len(title)
        return (b"CFU\0" + struct.pack("<LL", 0x1F40, header_length)
                + struct.pack("<LL32s8s8sLLLLLL", len(plain), len(body) + (header_length if whole else 0), name.encode(), created, modified,
                              0, 0, dir_mode, 0, 0, 0) + title + body)

    def make_sps(name, files):
        def long_string(s):
            return struct.pack("<L", len(s)) + s

        def entry(entry_name, length, mode):
            return struct.pack("<H64sL8xH2x8s8s", 98, entry_name.encode(), length, ((mode & 0xFF) << 8) | (mode >> 8), created, modified)

        body = entry(name, len(files) + 2, dir_mode) + b"".join(entry(file_name, len(content), file_mode) + content for file_name, content in files)
        return (b"\x0d\0\0\0SharkPortSave" + struct.pack("<L", 0) + long_string(name.encode()) + long_string(b"07/10/2026") + long_string(b"a comment")
                + struct.pack("<L", len(body)) + body + struct.pack("<L", 0))

    others = []
    for n, sizes in enumerate([[700, 5000], [0], [123456, 1024]]):
        files = make_files("Other Save %d" % n, sizes)
        for kind in ("cbs", "cbsw", "sps"):
            name = "BASLUS-2%04d%s" % (n, kind.upper())
            content = make_sps(name, files) if kind == "sps" else make_cbs(name, files, kind == "cbsw")
            with open(path(name + "." + kind[:3]), "wb") as f:
                f.write(content)
            others.append((name, files, name + "." + kind[:3]))
    mine("format", path("mine2.ps2"), "ecc", stamp)
    theirs(path("theirs2.ps2"), "format")
    for name, files, file_name in others:
        dump = mine("dump", path(file_name), stamp)
        check("a %s file reads here (%s)" % (file_name[-3:], name), my_files(dump, name, files), dump[:300])
        theirs(path("theirs2.ps2"), "import", path(file_name))
        check("and mymc+ reads the same out of it", their_files(path("theirs2.ps2"), name, files))
        mine("import", path("mine2.ps2"), path(file_name), stamp)
        check("and written to a card here, mymc+ reads it as it was", their_files(path("mine2.ps2"), name, files))
        # (A .psv is what mymc+ writes from a card, under a name of its own choosing.)
    # (.psv: mymc+ reads them and does not write them. Made here as its reader takes them.)
    def make_psv(name, files):
        def info(entry_name, size, mode):
            return struct.pack("<8s8sII32s", created, modified, size, mode, entry_name.encode())

        head = struct.pack("<4sI40s8xII", b"\0VSP", 0, b"\0" * 40, 0x2C, 2) + struct.pack("<IIIIIIIIII", 0, 0, 964, 0, 0, 0, 0, 0, 0, len(files))
        head += info(name, len(files) + 2, dir_mode)
        offset = len(head) + 60 * len(files)
        body = b""
        for file_name, content in files:
            head += info(file_name, len(content), file_mode) + struct.pack("<I", offset + len(body))
            body += content
        return head + body

    for n, sizes in enumerate([[700, 5000], [0], [123456, 1024]]):
        name = "BISLPS-2%04dPSV" % n
        files = make_files("Third Save %d" % n, sizes)
        with open(path(name + ".psv"), "wb") as f:
            f.write(make_psv(name, files))
        dump = mine("dump", path(name + ".psv"), stamp)
        check("a psv file reads here (%s)" % name, "format\tPlayStation 3 (.psv)" in dump and my_files(dump, name, files), dump[:300])
        theirs(path("theirs2.ps2"), "import", path(name + ".psv"))
        check("and mymc+ reads the same out of it", their_files(path("theirs2.ps2"), name, files))
        mine("import", path("mine2.ps2"), path(name + ".psv"), stamp)
        check("and written to a card here, mymc+ reads it as it was", their_files(path("mine2.ps2"), name, files))
    check("mymc+ finds that card sound", "No errors found" in theirs(path("mine2.ps2"), "check"))
    check("and lists it as it lists its own", theirs(path("mine2.ps2"), "dir") == theirs(path("theirs2.ps2"), "dir"),
          "\n" + theirs(path("mine2.ps2"), "dir") + "\n--\n" + theirs(path("theirs2.ps2"), "dir"))

    # ------------------------------------------------------------------- taken off, and filled
    for n, (name, files) in enumerate(saves):
        if n % 3 == 0:
            mine("remove", path("mine.ps2"), name, stamp)
            theirs(path("theirs.ps2"), "delete", name)
    check("mymc+ finds the card sound after saves were taken off here", "No errors found" in theirs(path("mine.ps2"), "check"))
    check("and lists what is left as on its own card", theirs(path("mine.ps2"), "dir") == theirs(path("theirs.ps2"), "dir"),
          "\n" + theirs(path("mine.ps2"), "dir") + "\n--\n" + theirs(path("theirs.ps2"), "dir"))
    check("with the room it has there", theirs(path("mine.ps2"), "df").split(":")[1] == theirs(path("theirs.ps2"), "df").split(":")[1])
    for n, (name, files) in enumerate(saves):
        if n % 3 != 0:
            check("the saves that stayed read as they were", their_files(path("mine.ps2"), name, files))
    # (Taken off by mymc+, written again here into the place that came free; and the other way.)
    for n, (name, files) in enumerate(saves):
        if n % 3 == 0:
            mine("import", path("theirs.ps2"), path(name + ".psu"), stamp)
            theirs(path("mine.ps2"), "import", path(name + ".psu"))
    for card_name in ("mine.ps2", "theirs.ps2"):
        check("%s is sound after both programs wrote to it" % card_name, "No errors found" in theirs(path(card_name), "check"))
        check("and here too", "ecc-mismatches\t0" in mine("check", path(card_name)))
        for name, files in saves:
            dump = mine("export", path(card_name), name, path("tmp.psu"))
            check("with every save as it was (%s on %s)" % (name, card_name), my_files(dump, name, files) and their_files(path(card_name), name, files))
    # (Without the times: a .max carries none, and each program wrote its own "now" for those.)
    def listed(card_name):
        return sorted("\t".join(line.split("\t")[:4]) for line in mine("ls", path(card_name)).splitlines())

    check("and both list the same", listed("mine.ps2") == listed("theirs.ps2"), "\n%s\n%s" % (listed("mine.ps2"), listed("theirs.ps2")))

    # Filled to its end here; mymc+ agrees it is full and sound.
    count = 0
    while count < 200:
        name = "FILL%03d" % count
        their_add(path("theirs2.ps2"), name, make_files("Fill", [rng.randrange(60000, 150000)]))
        theirs(path("theirs2.ps2"), "export", "-o", path("fill.psu"), "-f", name)
        theirs(path("theirs2.ps2"), "delete", name)
        out = subprocess.run([tool, "import", path("mine.ps2"), path("fill.psu"), stamp], capture_output=True, text=True)
        if out.returncode != 0:
            check("the save that does not fit is refused with the card untouched", out.returncode == 3 and "not enough room" in out.stdout, out.stdout)
            break
        count += 1
    check("the card took saves until it was full", 20 < count < 200, "%d" % count)
    check("mymc+ finds the full card sound", "No errors found" in theirs(path("mine.ps2"), "check"))
    my_free = int(mine("ls", path("mine.ps2")).splitlines()[-1].split("\t")[1])
    their_free = int(theirs(path("mine.ps2"), "df").split(":")[1].split()[0])
    check("and counts the room left as here", my_free == their_free, "%d against %d" % (my_free, their_free))
    for name, files in saves:
        check("the first saves are still as they were on the full card", their_files(path("mine.ps2"), name, files))

    print("memory cards against mymc+: %d checks, %d failed" % (checks, failed))
    return 0 if failed == 0 else 1


def plain_psu(content):
    """A .psu without the places its entries had on the card they came from, which differ from
    card to card and mean nothing in the file."""
    out = bytearray(content)
    at = 0
    entries = struct.unpack_from("<L", out, 4)[0] + 1
    for n in range(entries):
        if at + 512 > len(out):
            break
        mode, _, length = struct.unpack_from("<HHL", out, at)
        out[at + 16:at + 24] = bytes(8)
        at += 512
        if n >= 3 and mode & 0x10:
            at += (length + 1023) // 1024 * 1024
    return bytes(out)


def data_of_key(tool):
    """The key of CodeBreaker files, read out of the header it is written in (not kept twice)."""
    header = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "frontend", "ps5_mc.h")
    with open(header, "r", encoding="utf-8") as f:
        text = f.read()
    at = text.index("static const uint8_t key[256] = {")
    numbers = text[text.index("{", at) + 1:text.index("}", at)]
    key = bytes(int(n, 16) for n in numbers.replace("\n", " ").split(",") if n.strip())
    assert len(key) == 256 and tool
    return key


if __name__ == "__main__":
    sys.exit(main())
