#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - the names of the program's functions, for the title's own profiler.
#
#   ps5/tools/make-symbols.py <linked ELF> <link map> <symbols.txt> [--nm llvm-nm] [--cxxfilt llvm-cxxfilt]
#
# tools/link-title.sh runs it after the link. A test run with a "profile" line (README, "Test
# runs") samples where the graphics thread is and writes the busiest functions to the boot log
# by name; the names come from this file, which the title reads from its folder. One line a
# function, in address order:
#
#     <offset from the program's first byte, hex> <where it comes from> <name>
#
# where it comes from, by the archive the link took the code from (the link map says):
#     e   the emulator and this port (libpcsx2, libcommon, the port layer)
#     d   the graphics driver (RADV, ACO, NIR and Mesa's runtime)
#     p   the platform layer and the C runtime start-up
#     l   every other library
# as a capital letter when the function begins by setting up a frame ("push rbp; mov rbp, rsp"):
# the profiler reads the stack of such a function by its frame, and of the others by looking
# (frontend/ps5_profile_fold.h). And a last line "<offset> - end": where the program's code ends.
#
# Names are demangled and their parameter lists cut, so a line stays short; two functions that
# differ only in parameters then read the same, which a profile can live with.
import argparse
import re
import struct
import subprocess
import sys
from bisect import bisect_right
from pathlib import Path

NAME_LIMIT = 110
EMULATOR = ("libpcsx2-ps5.a", "libpcsx2.a", "libPCSX2_LTO.a", "libcommon.a", "libps5vfs.a")
DRIVER = ("libvulkan_radeon",)
PLATFORM = ("libps5platform", "app_crt.o", "crt1", "crti", "crtn", "_stub.o")


def code_sections(elf: Path):
    """The executable sections: {name: (address, end)}."""
    data = elf.read_bytes()
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        raise SystemExit(f"{elf}: not a 64-bit little-endian ELF")
    shoff, = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    headers = [struct.unpack_from("<IIQQQQ", data, shoff + index * shentsize) for index in range(shnum)]
    names_at = headers[shstrndx][4]
    sections = {}
    for name_offset, kind, flags, address, _, size in headers:
        if kind == 1 and flags & 0x4 and flags & 0x2 and size:  # PROGBITS, EXECINSTR, ALLOC
            name = data[names_at + name_offset:data.index(b"\0", names_at + name_offset)].decode()
            sections[name] = (address, address + size)
    if not sections:
        raise SystemExit(f"{elf}: no executable section")
    return sections


def frame_test(elf: Path):
    """A function of an address: whether the code there begins by setting up a frame.

    "push rbp; mov rbp, rsp", in either of the two ways the move is written, after an "endbr64"
    if there is one. A function the compiler gives a frame only on some of its paths begins
    otherwise and counts as without."""
    data = elf.read_bytes()
    shoff, = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum, _ = struct.unpack_from("<HHH", data, 0x3A)
    spans = []
    for index in range(shnum):
        _, kind, flags, address, offset, size = struct.unpack_from("<IIQQQQ", data, shoff + index * shentsize)
        if kind == 1 and flags & 0x4 and flags & 0x2 and size:
            spans.append((address, address + size, offset))

    def framed(address: int) -> bool:
        for start, end, offset in spans:
            if start <= address < end:
                code = data[offset + address - start:offset + min(address + 8, end) - start]
                if code.startswith(b"\xf3\x0f\x1e\xfa"):
                    code = code[4:]
                return code.startswith((b"\x55\x48\x89\xe5", b"\x55\x48\x8b\xec"))
        return False

    return framed


def origins(link_map: Path, sections):
    """Sorted (start, end, tag) of the input sections of the executable output sections.

    lld's map has one line for each output section, input section and symbol, told apart by how
    far the last column is indented: none, 8 and 16 spaces. Only the input sections' lines are
    taken, and only under an executable output section: a symbol's line can look like one (lld
    demangles, and "Outer::(anonymous namespace)::f()" has the same ":(" in it), and the debug
    sections are listed with addresses that start again at 0."""
    spans = []
    line_pattern = re.compile(r"^\s*([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+(\d+) ( *)(\S.*)$")
    executable = False
    with link_map.open(errors="replace") as file:
        for line in file:
            match = line_pattern.match(line)
            if not match:
                continue
            indent, what = len(match.group(5)), match.group(6)
            if indent == 0:
                executable = what in sections
                continue
            if indent != 8 or not executable or ":(" not in what:
                continue
            start, size = int(match.group(1), 16), int(match.group(3), 16)
            if size == 0:
                continue
            source = what.rsplit(":(", 1)[0]
            if any(name in source for name in EMULATOR):
                tag = "e"
            elif any(name in source for name in DRIVER):
                tag = "d"
            elif any(name in source for name in PLATFORM):
                tag = "p"
            else:
                tag = "l"
            spans.append((start, start + size, tag))
    spans.sort()
    return spans


OPERATOR = re.compile(r"operator\s*(<=>|<<=|>>=|->\*|<<|>>|<=|>=|->|<|>|\(\))")


def collapse_templates(name: str) -> str:
    """Template arguments replaced by nothing: "Foo<Bar<int>>::baz" reads "Foo<>::baz"."""
    out = []
    depth = 0
    for char in name:
        if char == "<":
            if depth == 0:
                out.append("<")
            depth += 1
        elif char == ">" and depth:
            depth -= 1
            if depth == 0:
                out.append(">")
        elif depth == 0:
            out.append(char)
    return "".join(out)


def short_name(demangled: str) -> str:
    """The name without its return type and parameter list, short enough for a log line."""
    text = demangled.replace("(anonymous namespace)", "{anon}")
    # Operators whose names hold brackets are set aside while brackets are counted.
    operators = []

    def aside(match):
        operators.append(match.group(0).replace(" ", ""))
        return f"\x01{len(operators) - 1}\x02"

    text = OPERATOR.sub(aside, text)
    # A "decltype(...)" return type has a parameter list of its own: skip it.
    start = 0
    if text.startswith("decltype("):
        depth = 0
        for index, char in enumerate(text):
            if char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
                if depth == 0:
                    start = index + 1
                    break
    depth = 0
    cut = len(text)
    for index in range(start, len(text)):
        char = text[index]
        if char == "<":
            depth += 1
        elif char == ">" and depth:
            depth -= 1
        elif char == "(" and depth == 0:
            cut = index
            break
    name = text[start:cut].strip()
    rest = text[cut:]
    # A return type in front (templates' instantiations carry one): the last word is the name.
    # (Not for a conversion operator, "Foo::operator bool", whose last word is its type.)
    depth = 0
    for at in (range(len(name) - 1, -1, -1) if not re.search(r"operator [A-Za-z_]", name) else ()):
        if name[at] == ">":
            depth += 1
        elif name[at] == "<" and depth:
            depth -= 1
        elif name[at] == " " and depth == 0:
            name = name[at + 1:]
            break
    if "$_" in rest or "lambda" in rest:
        name += " [a lambda in it]"
    if len(name) > NAME_LIMIT:
        name = collapse_templates(name)
    if len(name) > NAME_LIMIT:
        name = "..." + name[-(NAME_LIMIT - 3):]  # the end is the function's own name
    for index, operator in enumerate(operators):
        name = name.replace(f"\x01{index}\x02", operator)
    return name if name.strip("\x01\x02 ") else demangled[:NAME_LIMIT]


def main() -> int:
    parser = argparse.ArgumentParser(description="Write the title's symbols.txt.")
    parser.add_argument("elf", type=Path)
    parser.add_argument("map", type=Path)
    parser.add_argument("out", type=Path)
    parser.add_argument("--nm", default="llvm-nm")
    parser.add_argument("--cxxfilt", default=None, help="the demangler (default: llvm-cxxfilt or c++filt, as found)")
    args = parser.parse_args()

    sections = code_sections(args.elf)
    end = max(section[1] for section in sections.values())
    spans = origins(args.map, sections)
    framed = frame_test(args.elf)
    if not spans:
        raise SystemExit(f"{args.map}: no input sections found (is it lld's -Map output?)")
    starts = [span[0] for span in spans]

    listing = subprocess.run([args.nm, "-n", "--defined-only", str(args.elf)], check=True, capture_output=True,
                             text=True).stdout
    base = None
    functions = {}
    for line in listing.splitlines():
        parts = line.split(" ", 2)
        if len(parts) != 3:
            continue
        address, kind, name = int(parts[0], 16), parts[1], parts[2]
        if name == "__executable_start":
            base = address
        # (Not the assembler's local labels, which would cut a function in two, nor the marker
        # the C runtime puts at the program's first byte.)
        if kind in "tTwW" and address < end and not name.startswith((".L", "$", "..@")) and name != "__dso_handle":
            functions.setdefault(address, name)
    if base is None:
        raise SystemExit(f"{args.elf}: no __executable_start (the link script defines it)")

    addresses = sorted(address for address in functions if address >= base)
    mangled = [functions[address] for address in addresses]
    demangled = None
    for tool in ([args.cxxfilt] if args.cxxfilt else ["llvm-cxxfilt", "c++filt"]):
        try:
            answer = subprocess.run([tool], input="\n".join(mangled) + "\n", check=True, capture_output=True,
                                    text=True).stdout.splitlines()
        except (OSError, subprocess.CalledProcessError):
            continue
        if len(answer) == len(mangled):
            demangled = answer
            break
    if demangled is None:
        # The names as the linker has them are still names; the title folder is not held up for this.
        print("symbols: no demangler found (llvm-cxxfilt, c++filt): the names stay as the linker has them",
              file=sys.stderr)
        demangled = mangled

    counts = {"e": 0, "d": 0, "p": 0, "l": 0}
    with_frame = {"e": 0, "d": 0, "p": 0, "l": 0}
    with args.out.open("w") as out:
        for address, name in zip(addresses, demangled):
            at = bisect_right(starts, address) - 1
            tag = spans[at][2] if at >= 0 and address < spans[at][1] else "l"
            counts[tag] += 1
            has_frame = framed(address)
            with_frame[tag] += has_frame
            out.write(f"{address - base:x} {tag.upper() if has_frame else tag} {short_name(name)}\n")
        out.write(f"{end - base:x} - end\n")
    print(f"symbols: {len(addresses)} functions (emulator {counts['e']}, driver {counts['d']}, platform {counts['p']}, "
          f"other {counts['l']}), code ends at 0x{end - base:x}: {args.out}")
    print(f"symbols: begin with a frame: emulator {with_frame['e']} of {counts['e']}, driver {with_frame['d']} of "
          f"{counts['d']}")
    if counts["e"] == 0 or counts["d"] == 0:
        raise SystemExit("no function of the emulator, or none of the driver, was recognised in the link map")
    return 0


if __name__ == "__main__":
    sys.exit(main())
