#!/usr/bin/env python3
# Crux2 - the settings' words in one file a person can edit (ps5/frontend/bigpicture/settings-text.yaml).
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
#
#   ps5/tools/settings-text.py make [--inventory FILE | --dump FILE]
#       Writes settings-text.yaml from the settings inventory (settings-inventory.json, see
#       settings-inventory.py): a row for every row the interface shows, under the page and heading
#       it is shown in today. A row already in the file keeps what was written there by hand (its
#       label, desc, choices, area, section and level); new rows are added with the words the code
#       has; rows no longer in the inventory are dropped, and named.
#
#   ps5/tools/settings-text.py check [--inventory FILE | --dump FILE]
#       Fails when a row of the inventory has no entry, an entry is of no row (a stale id), a field
#       is not one of its values (area, level, origin), the baked header is not what bake would
#       write, or (until the interface takes its words from here: phase 3 of the revamp) a label,
#       desc or choice name is not the one the code shows.
#
#   ps5/tools/settings-text.py bake [--check]
#       Writes bp_settings_text.h, the file's words as a table the interface can compile in.
#
#   ps5/tools/settings-text.py doubtful
#       The rows whose area or section is a guess (the `doubtful:` notes), for the owner to decide.
#
#   ps5/tools/settings-text.py review --out FILE
#       Writes the review sheet (one HTML page): every row's words now and as they will be, grouped
#       by the revamp's areas, with the kind and PCSX2's source of each.
#
#   ps5/tools/settings-text.py apply
#       Makes the new words the row's words: new_label, new_desc and new_choices become label, desc
#       and choices (a field marked exact-but-wrong is left as it is, and named), then bakes. For
#       phase 3: after it, `check`'s comparison with the code's words fails until the interface
#       takes its words from the baked table.
#
# An entry (one line per field, values in double quotes or as a [list]):
#
#   - id: "EmuCore/GS.Renderer"           # fixed: the setting's section.key (an action's or a line's
#                                         # kind:words; "@page/heading" after it where one is in two places)
#     label: "Renderer"                   # the words shown
#     desc: "..."
#     choices: ["Automatic", ...]         # a list's names, in its order
#     kind: "stringlist"                  # for reference (from the code)
#     area: "Picture"                     # the revamp's eight: Picture, Sound, Controls, Games and saves,
#                                         # Emulation, Network, Interface, System
#     section: "Graphics"                 # the part of the area it goes in
#     level: "Essentials"                 # Essentials, More or Everything (today: Normal, Advanced, Expert)
#     origin: "pcsx2"                     # whose setting it is: pcsx2 or crux2
#     pcsx2_title: "Renderer"             # PCSX2's own label and description, word for word, and where
#     pcsx2_description: "..."            # they are (PCSX2 at the pin: FullscreenUI_Settings.cpp, Big
#     pcsx2_source: "pcsx2/ImGui/..:2890" # Picture's; else pcsx2-qt's tooltips; else its own tables)
#     pcsx2_tooltip: "..."                # the Qt interface's, when it has one as well
#     pcsx2_tooltip_source: "pcsx2-qt/..."
#     dynamic: true                       # its words come from data (PCSX2's controller tables, a game's
#                                         # patches, a drive's name): the interface cannot take them from here
#     doubtful: "..."                     # why the area or section is a guess
#
# The words of the revamp (phase 2), next to today's until they are applied (docs/dev/settings-inventory.md):
#
#     new_label: "Graphics API"           # the label, description and choice names to be
#     new_desc: "..."
#     new_choices: ["Vulkan", ...]
#     text: "exact"                       # the description's kind: exact (PCSX2's, byte for byte),
#                                         # written (in PCSX2's voice), exact-but-wrong (PCSX2's, but
#                                         # untrue of this row: the owner decides; apply leaves it)
#     text_source: "pcsx2/ImGui/..:3114"  # where PCSX2's text is (exact, exact-but-wrong)
#     text_note: "..."                    # why exact-but-wrong, or what else to know
#     label_text: "exact"                 # the label's kind (exact or written), and where PCSX2's is
#     label_source: "pcsx2/ImGui/..:3114"
#     choices_text: "exact"               # the choice names' kind: exact (all PCSX2's), mixed (some),
#     choices_source: "pcsx2/ImGui/..:2857" # written, exact-but-wrong; and the PCSX2 list they are from
#
# Never change an id by hand: the code finds the row by it.

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
FRONT = ROOT / "ps5" / "frontend" / "bigpicture"
INVENTORY = FRONT / "settings-inventory.json"
YAML = FRONT / "settings-text.yaml"
HEADER = FRONT / "bp_settings_text.h"
VENDOR = ROOT / "vendor" / "pcsx2"

AREAS = ["Picture", "Sound", "Controls", "Games and saves", "Emulation", "Network", "Interface", "System"]
LEVELS = {"Normal": "Essentials", "Advanced": "More", "Expert": "Everything"}
SETTING_KINDS = {"toggle", "intlist", "floatlist", "stringlist", "intrange", "floatrange", "combo", "bind"}
# The words the fake console of the inventory has (ps5/tests/bigpicture/inventory/host_fakes.cpp): a
# row with them in it is made from data.
FAKE_WORDS = ("USB drive 1", "/mnt/usb0", "A patch", "A cheat", "EmuCore/GS / upscale_multiplier")


def slug(text):
    return re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")[:56] or "top"


# ------------------------------------------------------------------------------ PCSX2's own words

def pristine(path):
    """A file of PCSX2 as it is at the pin, without ps5.patch (vendor/pcsx2 is a git checkout of it)."""
    try:
        return subprocess.run(["git", "-C", str(VENDOR), "show", f"HEAD:{path}"], capture_output=True, text=True,
                              check=True).stdout
    except (subprocess.CalledProcessError, FileNotFoundError):
        p = VENDOR / path
        return p.read_text(errors="replace") if p.exists() else ""


def pcsx2_files():
    try:
        listed = subprocess.run(["git", "-C", str(VENDOR), "ls-tree", "-r", "--name-only", "HEAD", "pcsx2", "pcsx2-qt"],
                                capture_output=True, text=True, check=True).stdout.split()
    except (subprocess.CalledProcessError, FileNotFoundError):
        listed = [str(p.relative_to(VENDOR)) for p in (VENDOR / "pcsx2").rglob("*.cpp")] + \
                 [str(p.relative_to(VENDOR)) for p in (VENDOR / "pcsx2-qt").rglob("*.cpp")]
    return [f for f in listed if f.endswith(".cpp")]


def strip_comments(src):
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if c == '"':
            j = i + 1
            while j < n and src[j] != '"':
                j += 2 if src[j] == "\\" else 1
            out.append(src[i:j + 1])
            i = j + 1
        elif src.startswith("//", i):
            j = src.find("\n", i)
            i = n if j < 0 else j
        elif src.startswith("/*", i):
            j = src.find("*/", i)
            i = n if j < 0 else j + 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def unescape(s):
    out = bytearray()
    i = 0
    while i < len(s):
        c = s[i]
        if c != "\\":
            out += c.encode()
            i += 1
            continue
        e = s[i + 1]
        if e == "x":
            m = re.match(r"[0-9a-fA-F]{1,2}", s[i + 2:])
            out.append(int(m.group(0), 16))
            i += 2 + len(m.group(0))
            continue
        out += {"n": b"\n", "t": b"\t", "\\": b"\\", '"': b'"', "'": b"'"}.get(e, e.encode())
        i += 2
    return out.decode("utf-8", "replace")


def literals(arg):
    """The text of the string literals in a C++ expression, one after the other."""
    return "".join(unescape(m) for m in re.findall(r'"((?:[^"\\]|\\.)*)"', strip_comments(arg)))


def call_args(src, at):
    """The arguments of the call whose '(' is just before `at`."""
    args, depth, start, i = [], 0, at, at
    while i < len(src):
        c = src[i]
        if c == '"':
            j = i + 1
            while j < len(src) and src[j] != '"':
                j += 2 if src[j] == "\\" else 1
            i = j + 1
            continue
        if src.startswith("//", i):
            i = src.find("\n", i)
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            if depth == 0:
                args.append(src[start:i])
                return args
            depth -= 1
        elif c == "," and depth == 0:
            args.append(src[start:i])
            start = i + 1
        i += 1
    return args


# ------------------------------------------------------------------------------ PCSX2's words, by place

TEXT_MACROS = re.compile(r"\b(FSUI_ICONSTR_S|FSUI_ICONSTR|FSUI_CSTR|FSUI_NSTR|FSUI_STR|FSUI_FSTR|FSUI_VSTR|QT_TRANSLATE_NOOP|"
                         r"TRANSLATE_NOOP|TRANSLATE_SV|TRANSLATE|tr)\s*\(")
ONLY_LITERALS = re.compile(r'\s*(?:"(?:[^"\\]|\\.)*"\s*)+')


def line_offset(src, line):
    pos = 0
    for _ in range(line - 1):
        pos = src.index("\n", pos) + 1
    return pos


def statement_at(src, line):
    """The text of the statement, call or table entry that starts on `line` (1-based)."""
    start = i = line_offset(src, line)
    depth, opened, n = 0, False, len(src)
    while i < n:
        c = src[i]
        if c == '"':
            j = i + 1
            while j < n and src[j] != '"':
                j += 2 if src[j] == "\\" else 1
            i = j + 1
            continue
        if src.startswith("//", i):
            i = src.find("\n", i)
            i = n if i < 0 else i
            continue
        if c in "([{":
            depth += 1
            opened = True
        elif c in ")]}":
            depth -= 1
            if depth < 0:
                break
            if depth == 0 and opened and c == "}" and src[i + 1:i + 2] in (",", ";", "\n"):
                i += 1
                break
            if depth == 0 and c == ")" and (src[i + 1:].lstrip().startswith("{") or  # if (...) { : the condition only
                                            not src[i + 1:src.find("\n", i) if "\n" in src[i:] else n].strip()):  # a macro: no ;
                i += 1
                break
        elif c == ";" and depth == 0:
            break
        elif c == "," and depth == 0 and opened:
            break
        i += 1
    return src[start:i]


def texts_of(code):
    """The translatable texts of a piece of C++, in order: the text argument of each FSUI_*STR, TRANSLATE_NOOP, tr()."""
    out = []
    for m in TEXT_MACROS.finditer(code):
        args = call_args(code, m.end())
        if not args:
            continue
        macro = m.group(1)
        arg = args[1] if macro in ("FSUI_ICONSTR", "FSUI_ICONSTR_S", "TRANSLATE_NOOP", "QT_TRANSLATE_NOOP", "TRANSLATE_SV", "TRANSLATE") \
            and len(args) > 1 else args[0]
        if ONLY_LITERALS.fullmatch(strip_comments(arg)):
            out.append(literals(arg))
    return out


def texts_at(where):
    """The translatable texts of the statement at `where` ("path:line") in PCSX2 at the pin, without ps5.patch; of a Qt
    form (.ui), the <string>s on that line, a label's colon left off."""
    path, _, line = where.rpartition(":")
    src = pristine(path)
    if not src or not line.isdigit():
        return []
    if path.endswith(".ui"):
        import html
        text = src.splitlines()[int(line) - 1]
        return [html.unescape(t).removesuffix(":") for t in re.findall(r"<string[^>]*>(.*?)</string>", text)]
    # The statement that starts on the line, and every text written whole on the line itself (a table's row of several).
    texts = texts_of(statement_at(src, int(line)))
    return texts + [t for t in texts_of(src.splitlines()[int(line) - 1]) if t not in texts]


def array_at(where):
    """The entries of the brace-initialised array whose definition is at `where`: each a translatable text, a string, a number."""
    path, _, line = where.rpartition(":")
    src = pristine(path)
    start = line_offset(src, int(line))
    open_at = src.index("{", start)
    body = src[open_at + 1:]
    depth, i, entries, cur = 0, 0, [], ""
    while i < len(body):
        c = body[i]
        if c == '"':
            j = i + 1
            while j < len(body) and body[j] != '"':
                j += 2 if body[j] == "\\" else 1
            cur += body[i:j + 1]
            i = j + 1
            continue
        if body.startswith("//", i):
            i = body.find("\n", i)
            continue
        if c == "#":  # a preprocessor line (#ifdef): every platform's entries are read
            i = body.find("\n", i)
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            if depth == 0:
                entries.append(cur)
                break
            depth -= 1
        elif c == "," and depth == 0:
            entries.append(cur)
            cur = ""
            i += 1
            continue
        cur += c
        i += 1
    out = []
    for e in entries:
        e = strip_comments(e).strip()
        if not e or e == "nullptr":
            continue
        t = texts_of(e)
        if t:
            out.append(t[0])
        elif ONLY_LITERALS.fullmatch(e):
            out.append(literals(e))
        else:
            out.append(e.rstrip("f"))
    return out


def find_array(src, name, before):
    """The line of the definition of the array `name` nearest before offset `before` (or anywhere in the file)."""
    best = None
    for m in re.finditer(r"\b" + re.escape(name) + r"\s*\[[^\]]*\]\s*=\s*\{", src):
        if best is None or m.start() < before:
            best = m
    return None if best is None else src.count("\n", 0, best.start()) + 1


DRAW = re.compile(r"\bDraw(Toggle|IntList|IntRange|IntSpinBox|FloatRange|FloatSpinBox|StringList|FloatList|Enum|IPAddress|IntRect|"
                  r"ClampingMode)Setting\(\s*bsi\s*,")
CLAMP_KEYS = {"-1": "fpuOverflow", "0": "vu0Overflow", "1": "vu1Overflow"}

# PCSX2 settings whose FullscreenUI row is not a Draw*Setting call with the key written out (a key made with fmt, a
# button of its own): where the row is, read as the others are.
FSUI = "pcsx2/ImGui/FullscreenUI_Settings.cpp"
# {key: (title's place, which text of it, description's place or None, which text of it)}
MANUAL_ROWS = {
    ("Filenames", "BIOS"): (f"{FSUI}:2552", 0, f"{FSUI}:2552", 1),
    ("EmuCore", "InputProfileName"): (f"{FSUI}:2203", 0, f"{FSUI}:2203", 1),
    ("MemoryCards", "Slot1_Enable"): (f"{FSUI}:3878", 0, f"{FSUI}:3878", 1),
    ("MemoryCards", "Slot2_Enable"): (f"{FSUI}:3878", 0, f"{FSUI}:3878", 1),
    ("MemoryCards", "Slot1_Filename"): (f"{FSUI}:3887", 0, f"{FSUI}:3887", 1),
    ("MemoryCards", "Slot2_Filename"): (f"{FSUI}:3887", 0, f"{FSUI}:3887", 1),
    ("Pad", "Type"): (f"{FSUI}:5345", 0, None, 0),
    ("EmuCore", "ShowCheatsForAllCRCs"): (f"{FSUI}:5905", 0, f"{FSUI}:5907", 0),
    ("EmuCore", "ShowPatchesForAllCRCs"): (f"{FSUI}:5905", 1, f"{FSUI}:5907", 1),
    ("EmuCore/GS", "FramerateNTSC"): ("pcsx2-qt/Settings/GraphicsAdvancedSettingsTab.ui:247", 0, None, 0),
    ("EmuCore/GS", "FrameratePAL"): ("pcsx2-qt/Settings/GraphicsAdvancedSettingsTab.ui:273", 0, None, 0),
    ("SPU2/Output", "StretchSequenceLengthMS"): ("pcsx2-qt/Settings/AudioStretchSettingsDialog.ui:20", 0, None, 0),
    ("SPU2/Output", "StretchSeekWindowMS"): ("pcsx2-qt/Settings/AudioStretchSettingsDialog.ui:63", 0, None, 0),
    ("SPU2/Output", "StretchOverlapMS"): ("pcsx2-qt/Settings/AudioStretchSettingsDialog.ui:106", 0, None, 0),
    ("SPU2/Output", "StretchUseQuickSeek"): ("pcsx2-qt/Settings/AudioStretchSettingsDialog.ui:198", 0, None, 0),
    ("SPU2/Output", "StretchUseAAFilter"): ("pcsx2-qt/Settings/AudioStretchSettingsDialog.ui:205", 0, None, 0),
    ("SPU2/Output", "ExpandBlockSize"): ("pcsx2-qt/Settings/AudioExpansionSettingsDialog.ui:456", 0, None, 0),
    ("SPU2/Output", "ExpandCircularWrap"): ("pcsx2-qt/Settings/AudioExpansionSettingsDialog.ui:20", 0, None, 0),
    ("SPU2/Output", "ExpandShift"): ("pcsx2-qt/Settings/AudioExpansionSettingsDialog.ui:63", 0, None, 0),
    ("SPU2/Output", "ExpandDepth"): ("pcsx2-qt/Settings/AudioExpansionSettingsDialog.ui:106", 0, None, 0),
    ("SPU2/Output", "ExpandFocus"): ("pcsx2-qt/Settings/AudioExpansionSettingsDialog.ui:149", 0, None, 0),
    ("SPU2/Output", "ExpandCenterImage"): ("pcsx2-qt/Settings/AudioExpansionSettingsDialog.ui:192", 0, None, 0),
    ("SPU2/Output", "ExpandFrontSeparation"): ("pcsx2-qt/Settings/AudioExpansionSettingsDialog.ui:235", 0, None, 0),
    ("SPU2/Output", "ExpandRearSeparation"): ("pcsx2-qt/Settings/AudioExpansionSettingsDialog.ui:278", 0, None, 0),
    ("SPU2/Output", "ExpandLowCutoff"): ("pcsx2-qt/Settings/AudioExpansionSettingsDialog.ui:321", 0, None, 0),
    ("SPU2/Output", "ExpandHighCutoff"): ("pcsx2-qt/Settings/AudioExpansionSettingsDialog.ui:364", 0, None, 0),
}
# Choice names PCSX2 keeps outside FullscreenUI's own arrays: {key: (display names' array, values' array or an offset)}.
MANUAL_CHOICES = {
    ("EmuCore/GS", "upscale_multiplier"): (f"{FSUI}:2910", f"{FSUI}:2937"),
    ("EmuCore/GS", "AspectRatio"): ("pcsx2/Pcsx2Config.cpp:639", "pcsx2/Pcsx2Config.cpp:639"),
    ("EmuCore/GS", "FMVAspectRatioSwitch"): ("pcsx2/Pcsx2Config.cpp:647", "pcsx2/Pcsx2Config.cpp:647"),
    ("SPU2/Output", "SyncMode"): ("pcsx2/Pcsx2Config.cpp:1214", "pcsx2/Pcsx2Config.cpp:1210"),
    # FullscreenUI lists the positions from Top Left as 0, but the setting is an OsdOverlayPos, whose 0 is None.
    ("Achievements", "NotificationPosition"): (f"{FSUI}:5037", 1),
    ("SPU2/Output", "ExpansionMode"): ("pcsx2/Host/AudioStream.cpp:191", "pcsx2/Host/AudioStream.cpp:183"),
    ("Pad", "InvertL"): ("pcsx2/SIO/Pad/PadDualshock2.cpp:51", 0),
    ("Pad", "InvertR"): ("pcsx2/SIO/Pad/PadDualshock2.cpp:51", 0),
}


def pcsx2_text():
    """PCSX2's words for each setting at the pin: {(section, key): [{"ui", "title", "desc", "where", "choices"}...]}, FullscreenUI's
    rows first (in file order), then the Qt interface's tooltips, then the controller tables. "choices" is [(name, value)] where
    PCSX2 names them; a "desc" of None is a row that has none."""
    found = {}

    def add(key, entry):
        found.setdefault(key, []).append(entry)

    for path in (FSUI, "pcsx2/ImGui/FullscreenUI.cpp"):
        src = pristine(path)
        for m in DRAW.finditer(src):
            args = call_args(src, m.end())
            kind = m.group(1)
            if len(args) < 3:
                continue
            line = src.count("\n", 0, m.start()) + 1
            where = f"{path}:{line}"
            t = texts_of(args[0])
            title = t[0] if t else None
            s = args[1].strip()
            desc = texts_of(s)[0] if re.fullmatch(r"FSUI_CSTR\(" + ONLY_LITERALS.pattern + r"\)", s, re.S) else None
            if kind == "ClampingMode":
                keys = [("EmuCore/CPU/Recompiler", CLAMP_KEYS.get(args[2].strip(), ""))]
            elif kind == "IntRect":
                keys = [(literals(args[2]), literals(a)) for a in (args[3], args[5], args[7], args[9])]
            elif len(args) >= 4 and ONLY_LITERALS.fullmatch(args[2]) and ONLY_LITERALS.fullmatch(args[3]):
                keys = [(literals(args[2]), literals(args[3]))]
            else:
                continue
            choices = None
            if kind in ("IntList", "StringList", "FloatList") and len(args) > 6:
                names_line = find_array(src, args[5].strip(), m.start()) if re.fullmatch(r"\s*\w+\s*", args[5]) else None
                if names_line:
                    names = array_at(f"{path}:{names_line}")
                    if kind == "IntList":
                        offset = int(args[8]) if len(args) > 8 and re.fullmatch(r"\s*-?\d+\s*", args[8]) else 0
                        values = [i + offset for i in range(len(names))]
                    else:
                        values_line = find_array(src, args[6].strip(), m.start()) if re.fullmatch(r"\s*\w+\s*", args[6]) else None
                        values = array_at(f"{path}:{values_line}") if values_line else None
                    if values and len(values) == len(names):
                        choices = {"where": f"{path}:{names_line}", "list": list(zip(names, values))}
            for key in keys:
                if key[1]:
                    add(key, {"ui": "fsui", "title": title, "desc": desc, "where": where, "choices": choices})
    for key, (title_at, ti, desc_at, di) in MANUAL_ROWS.items():
        t = texts_at(title_at)
        d = texts_at(desc_at) if desc_at else []
        add(key, {"ui": "qt" if title_at.rpartition(":")[0].endswith(".ui") else "table" if "/Pad/" in title_at else "fsui",
                  "title": t[ti] if len(t) > ti else None, "desc": d[di] if len(d) > di else None,
                  "where": desc_at or title_at, "title_where": title_at, "choices": None})
    for path in pcsx2_files():
        if not path.startswith("pcsx2-qt/"):
            continue
        src = pristine(path)
        if "registerWidgetHelp" not in src:
            continue
        widgets = {}
        for m in re.finditer(r"Bind\w*Setting\w*\(\s*sif\s*,\s*([\w.\->]+)\s*,\s*\"([^\"]+)\"\s*,\s*\"([^\"]+)\"", src):
            widgets.setdefault(m.group(1).strip(), []).append((m.group(2), m.group(3)))
        for m in re.finditer(r"registerWidgetHelp\(\s*", src):
            args = call_args(src, m.end())
            if len(args) < 4:
                continue
            line = src.count("\n", 0, m.start()) + 1
            t1, t3 = texts_of(args[1]), texts_of(args[3])
            for key in widgets.get(args[0].strip(), []):
                add(key, {"ui": "qt", "title": t1[0] if t1 else None, "desc": t3[0] if t3 else None, "where": f"{path}:{line}",
                          "choices": None})
    # The controller's own settings (the DualShock 2's table, which FullscreenUI and Qt show as they are).
    path = "pcsx2/SIO/Pad/PadDualshock2.cpp"
    src = pristine(path)
    for m in re.finditer(r"\{\s*SettingInfo::Type::\w+\s*,\s*\"(\w+)\"", src):
        line = src.count("\n", 0, m.start()) + 1
        t = texts_of(statement_at(src, line))
        add(("Pad", m.group(1)), {"ui": "table", "title": t[0] if t else None, "desc": t[1] if len(t) > 1 else None,
                                  "where": f"{path}:{line}", "choices": None})
    # The bindings' names: the DualShock 2's buttons, and the hotkeys (Hotkeys.cpp, GS.cpp).
    for m in re.finditer(r"\{\s*\"(\w+)\"\s*,\s*TRANSLATE_NOOP\(\s*\"Pad\"", src):
        line = src.count("\n", 0, m.start()) + 1
        t = texts_at(f"{path}:{line}")
        add(("Pad", m.group(1)), {"ui": "table", "title": t[0] if t else None, "desc": None, "where": f"{path}:{line}", "choices": None})
    noop = r"TRANSLATE_NOOP\(\s*\"Hotkeys\"\s*,\s*\"([^\"]*)\"\s*\)"
    for path in ("pcsx2/Hotkeys.cpp", "pcsx2/GS/GS.cpp"):
        src = pristine(path)
        hotkeys = [(m.group(1), m.start(3)) for m in re.finditer(r"\"(\w+)\"\s*,\s*" + noop + r"\s*,\s*" + noop, src)]
        hotkeys += [(("SaveStateToSlot" if m.group(1) == "SAVE" else "LoadStateFromSlot") + m.group(2), m.start(3))
                    for m in re.finditer(r"DEFINE_HOTKEY_(SAVE|LOAD)STATE_X\((\d+),\s*" + noop, src)]
        for name, at in hotkeys:
            line = src.count("\n", 0, at) + 1
            add(("Hotkeys", name), {"ui": "table", "title": unescape(re.match(r'([^"]*)', src[at:]).group(1)), "desc": None,
                                    "where": f"{path}:{line}", "choices": None})
    for key, (names_at, values_at) in MANUAL_CHOICES.items():
        names = array_at(names_at)
        values = array_at(values_at) if isinstance(values_at, str) else [i + values_at for i in range(len(names))]
        for entry in found.get(key, []):
            if len(values) == len(names):
                entry["choices"] = {"where": names_at, "list": list(zip(names, values))}
    return found


def pcsx2_for(found, section, key):
    """PCSX2's words for a row's setting: a controller port's (Pad1..Pad8) are the controller table's."""
    if re.fullmatch(r"Pad\d", section or ""):
        return found.get(("Pad", key), [])
    if (section, key) in found:
        return found[(section, key)]
    # PCSX2's own spelling of a key can differ in case (EmuCore/TraceLog IOP.Bios): its settings are read without it.
    return next((v for (s, k), v in found.items() if s == section and k.lower() == (key or "").lower()), [])


def pcsx2_keys():
    """Every quoted word of PCSX2's own sources (the pristine ones): a setting's key that is not among them is Crux2's."""
    words = set()
    for path in pcsx2_files():
        if path.startswith(("pcsx2/ImGui/", "pcsx2-qt/Settings/")) or path in ("pcsx2/Pcsx2Config.cpp", "pcsx2/SIO/Pad/Pad.cpp",
                                                                               "pcsx2/Hotkeys.cpp", "pcsx2/GS/GS.cpp"):
            words |= set(re.findall(r'"([A-Za-z0-9_.]+)"', pristine(path)))
    # Pcsx2Config.cpp names many keys by the field (SettingsWrapBitBool(Name)): the field is the key.
    words |= set(re.findall(r"SettingsWrap\w*\(\s*(\w+)", pristine("pcsx2/Pcsx2Config.cpp")))
    return words


# ------------------------------------------------------------------------------- the revamp's map

def area_of(row):
    """(area, section, doubtful or None) for a row: the revamp's grouping (docs/dev/settings-inventory.md)."""
    page, heading, rid, key = row["page"], row["heading"], row["id"], row.get("key", "")
    sk = f"{row.get('section', '')}.{key}"
    if page == "Interface":
        if heading == "SOUND":
            return "Sound", "Interface sounds", None
        if heading == "CARE FOR THE SCREEN":
            return "Interface", "Screen care", None
        if sk in ("PS5UI.Look", "PS5UI.Accent", "PS5UI.Backdrop", "PS5UI.WallEvery", "PS5UI.WallDim") or rid.startswith("action:my-pictures"):
            return "Interface", "Skins", None
        if sk == "UI.PauseOnControllerDisconnection":
            return "Interface", "Interface", "a controller's setting: Controls > Controllers?"
        if sk == "EmuCore.WarnAboutUnsafeSettings":
            return "Interface", "Interface", "about emulation settings: Emulation > Emulation?"
        if sk == "PS5UI.SwapConfirm":
            return "Interface", "Interface", "the buttons in the menus: Controls > Controllers?"
        return "Interface", "Interface", None
    if page == "Game List":
        if sk == "PS5UI.View":
            return "Interface", "Library layouts", None
        if sk == "PS5UI.HeroPlay":
            return "Interface", "Library layouts", "a part of the Shelves layout; or Games and saves > Game List"
        if rid in ("action:download-cheats-for-all-games",):
            return "Games and saves", "Game List", "cheats: Emulation > Cheats?"
        if rid in ("action:download-pcsx2-s-patches",):
            return "Games and saves", "Game List", "patches: Emulation > Patches?"
        if rid == "action:share-the-covers":
            return "Games and saves", "Game List", "a sync with the backup place: Games and saves > Backups?"
        return "Games and saves", "Game List", None
    if page == "Folders":
        if heading in ("NETWORK SHARES", "A NEW SHARE"):
            return "Network", "Shares", None
        return "Games and saves", "Folders", None
    if page == "BIOS":
        if sk == "PS5.ConsoleLanguage":
            return "Games and saves", "BIOS", "the PlayStation 2's language, not the interface's: or Interface > Language?"
        if heading == "OPTIONS AND PATCHES":
            return "Games and saves", "BIOS", "the start-up of a game: Emulation > Emulation?"
        return "Games and saves", "BIOS", None
    if page == "Emulation":
        if sk in ("PS5.OutputRefresh", "PS5.VariableRefresh"):
            return "Picture", "Output and HDR", None
        if sk == "EmuCore/GS.BlackFrameInsertion":
            return "Picture", "Output and HDR", "with the frame pacing today; or Emulation > Emulation"
        if heading == "FRAME PACING / LATENCY CONTROL":
            return "Emulation", "Emulation", "frame pacing: or Picture > Output and HDR?"
        if sk in ("PS5.ThreadLayout", "PS5.DriverOwnCore"):
            return "Emulation", "Emulation", "the console's threads: or System > PlayStation 5?"
        return "Emulation", "Emulation", None
    if page == "Game Fixes":
        return "Emulation", "Game Fixes", None
    if page in ("Patches", "Cheats"):
        return "Emulation", page, "the eight areas name no place for a game's patches and cheats"
    if page == "Graphics":
        if heading == "TEXTURE REPLACEMENT":
            if key.startswith("Dump") or rid.startswith("EmuCore/GS.Dump"):
                return "Games and saves", "Texture packs", "making texture packs (dumping): or Picture > Graphics, or System > Debug?"
            return "Games and saves", "Texture packs", None
        if sk in ("PS5.OutputHeight", "PS5.HDR", "PS5.HDRPeak", "PS5.HDRWhite") or rid == "action:adjust-hdr-brightness":
            return "Picture", "Output and HDR", None
        if key.startswith("FrameGeneration") or key == "PS5FrameGenDebug":
            return "Picture", "Frame generation", None
        if sk in ("EmuCore/GS.TVShader", "EmuCore/GS.Look") or key.startswith("ShadeBoost") or rid == "action:a-look-of-your-own":
            return "Picture", "Looks", None
        if sk == "PS5.Deband":
            return "Picture", "Looks", "the last pass before the screen: or Picture > Output and HDR?"
        if sk == "PS5.ColourFilter":
            return "Picture", "Looks", "for colour blindness: or Interface > Interface (with the menus' colours)?"
        return "Picture", "Graphics", None
    if page == "On-Screen Display":
        if sk == "PS5UI.OsdMove":
            return "Picture", "On-Screen Display", "screen care (Options' care switches): or Interface > Screen care?"
        return "Picture", "On-Screen Display", None
    if page == "Audio":
        return "Sound", "Audio", None
    if page == "Memory Cards":
        if heading in ("MEMORY CARDS AND SAVED STATES", "MEMORY CARDS IN SYNC") or heading.startswith("BACKUPS ON"):
            return "Games and saves", "Backups", None
        return "Games and saves", "Memory Cards", None
    if page == "Network & HDD":
        if heading == "A PHONE OR A COMPUTER":
            return "Network", "Network & HDD", "the phone's web page: or Interface?"
        return "Network", "Network & HDD", None
    if page == "Achievements":
        return "Network", "Achievements", None
    if page == "Controllers":
        if row["kind"] == "bind" and row.get("section") == "Hotkeys" or heading in ("NAVIGATION", "SPEED", "SYSTEM", "SAVE STATES", "AUDIO",
                                                                                     "GRAPHICS"):
            return "Controls", "Hotkeys", None
        if heading == "LIGHT GUN" or key.startswith("MotionAim") or sk in ("PS5.MotionAim", "PS5.GunConTriggerFeel") or \
                rid == "action:measure-the-screen":
            return "Controls", "Light gun", None
        if sk == "PS5.MoveAim":
            return "Controls", "PS Move", None
        if sk in ("PS5.USBKeyboard", "PS5.USBPads"):
            return "Controls", "USB devices", None
        if heading == "SINGSTAR":
            return "Controls", "USB devices", "the PS2's USB microphones: or Sound?"
        if rid == "action:map-it-to-a-usb-controller":
            return "Controls", "Controllers", "a port's mapping to a USB pad: or Controls > USB devices?"
        return "Controls", "Controllers", None
    if page == "Advanced":
        if heading == "SAVESTATE SETTINGS":
            return "Games and saves", "Save states", None
        if heading == "GAME SETTINGS":
            return "Emulation", "Game Fixes", "PCSX2's Advanced page has them; with the game fixes, or Emulation > Advanced?"
        return "Emulation", "Advanced (EE, VU, IOP)", None
    if page == "Debug":
        if heading in ("WHAT CRUX2 WROTE DOWN", "TAKING THEM ALONG"):
            return "System", "Log", None
        return "System", "Debug", None
    if page == "PlayStation 5":
        if heading == "UPDATES":
            return "Network", "Updates", None
        if heading == "ABOUT":
            return "System", "About", None
        if heading == "NATIVE CODE MODULES (EXPERIMENTAL)":
            return "System", "PlayStation 5", "how games' code runs: or Emulation > Advanced?"
        return "System", "PlayStation 5", None
    if page == "Statistics":
        return "System", "Statistics", None
    if page == "This game":
        return "Games and saves", "This game", "a game's own first page; the eight areas are the Settings'"
    return "System", page, f"a page the map does not know ({page})"


def level_of(flags):
    return LEVELS["Expert" if "Expert" in flags else "Advanced" if "Advanced" in flags else "Normal"]


def dynamic(row):
    if row["kind"] == "bind" or re.fullmatch(r"Pad\d", row.get("section", "")) or row.get("section") in ("Patches", "Cheats"):
        return True
    return any(w in row["label"] or w in row.get("heading", "") for w in FAKE_WORDS)


# ----------------------------------------------------------------------------------- the file

def q(value):
    return json.dumps(value, ensure_ascii=False)


def entries(inventory, found, keys, old):
    """The file's pages, headings and entries from the inventory (and what `old` has of them by hand)."""
    pages = {}
    for row in inventory["rows"]:
        page = pages.setdefault(row["page"], {"headings": {}})
        heading = page["headings"].setdefault(row["heading"], {"label": row["heading"], "rows": []})
        if row["kind"] == "heading":
            continue
        e = {"id": row["id"], "label": row["label"], "desc": row.get("desc", "")}
        if row.get("choices"):
            e["choices"] = [c["name"] for c in row["choices"]]
        e["kind"] = row["kind"]
        area, section, doubt = area_of(row)
        e["area"], e["section"], e["level"] = area, section, level_of(row.get("flags", []))
        game = {v.get("flags") and level_of(v["flags"]) for k, v in row.get("variants", {}).items() if k.startswith("game/") and "flags" in v}
        if game and game != {e["level"]} and "game" in row["layers"]:
            e["level_one_game"] = sorted(g for g in game if g)[0]
        theirs = pcsx2_for(found, row.get("section"), row.get("key")) if row.get("key") else []
        crux2 = row.get("section") in ("PS5", "PS5UI", "PS5GamePatch") or row["kind"] in ("action", "info") or \
            (row.get("key") and row["kind"] != "bind" and row["key"] not in keys and not theirs and
             row.get("section") not in ("Patches", "Cheats") and not re.fullmatch(r"Pad\d", row.get("section", "")))
        e["origin"] = "crux2" if crux2 else "pcsx2"
        if e["origin"] == "pcsx2" and theirs:
            # PCSX2's words for it, for reference (FullscreenUI's first, then its controller tables, then Qt's).
            first = next((o for o in theirs if o["ui"] in ("fsui", "table") and (o["desc"] or o["title"])), None) or theirs[0]
            if first["title"]:
                e["pcsx2_title"] = first["title"]
            if first["desc"]:
                e["pcsx2_description"] = first["desc"]
            e["pcsx2_source"] = first["where"] if first["desc"] else first.get("title_where", first["where"])
            qt = next((o for o in theirs if o["ui"] == "qt" and o["desc"]), None)
            if first["ui"] != "qt" and qt and qt["desc"] != first["desc"]:
                e["pcsx2_tooltip"] = qt["desc"]
                e["pcsx2_tooltip_source"] = qt["where"]
        if row["layers"] != ["shared", "game"]:
            e["layers"] = row["layers"][0] if len(row["layers"]) == 1 else row["layers"]
        if dynamic(row):
            e["dynamic"] = True
        if doubt:
            e["doubtful"] = doubt
        # What was written by hand is kept.
        if row["id"] in old:
            for field in ("label", "desc", "choices", "area", "section", "level", "doubtful") + NEW_FIELDS:
                if field in old[row["id"]]:
                    e[field] = old[row["id"]][field]
        heading["rows"].append(e)
    return pages


# The revamp's words (phase 2), kept by make like the hand-written fields.
NEW_FIELDS = ("new_label", "label_text", "label_source", "new_desc", "text", "text_source", "text_note", "new_choices", "choices_text",
              "choices_source")
FIELDS = ("id", "label", "desc", "choices", "kind", "area", "section", "level", "level_one_game", "origin", "layers", "dynamic", "pcsx2_title",
          "pcsx2_description", "pcsx2_source", "pcsx2_tooltip", "pcsx2_tooltip_source", "doubtful") + NEW_FIELDS


def write_yaml(pages, path):
    """Writes the file from entries()' pages (a dict by page name, of headings by label)."""
    dump_yaml([{"name": name, "headings": [{"label": hlabel, "rows": heading["rows"]} for hlabel, heading in page["headings"].items()]}
               for name, page in pages.items()], path)


def dump_yaml(pages, path):
    """Writes the file from read_yaml()'s pages (a list of pages, each a list of headings)."""
    out = ["# Crux2 - every setting's words, for editing by hand (ps5/tools/settings-text.py: make, check, bake, review, apply).",
           "# Under each page and heading as the interface shows them today. label, desc and choices are today's words;",
           "# new_label, new_desc and new_choices the revamp's (text, label_text, choices_text: exact = PCSX2's, byte for",
           "# byte, at *_source; written = in PCSX2's voice; exact-but-wrong = PCSX2's but untrue here, for the owner to",
           "# decide). area, section and level place the row in the revamp's eight areas. Never change an id. A row marked",
           "# dynamic takes its words from data. pcsx2_* is PCSX2's own text and where it is (for reference).",
           "pages:"]
    for page in pages:
        out.append(f"  - page: {q(slug(page['name']))}")
        out.append(f"    name: {q(page['name'])}")
        out.append("    headings:")
        for heading in page["headings"]:
            out.append(f"      - heading: {q(slug(heading['label']))}")
            out.append(f"        label: {q(heading['label'])}")
            out.append("        rows:" + ("" if heading["rows"] else " []"))
            for e in heading["rows"]:
                first = True
                for field in FIELDS:
                    if field not in e:
                        continue
                    out.append(f"          {'- ' if first else '  '}{field}: {q(e[field])}")
                    first = False
    Path(path).write_text("\n".join(out) + "\n")


def read_yaml(path):
    """The file's pages (a reader for its own shape: one field a line, values in JSON's form or plain)."""
    pages = []
    page = heading = entry = None
    for number, raw in enumerate(Path(path).read_text().splitlines(), 1):
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        indent = len(raw) - len(raw.lstrip())
        text = raw.strip()
        item = text.startswith("- ")
        if item:
            text = text[2:]
            indent += 2
        key, sep, value = text.partition(":")
        if not sep:
            raise SystemExit(f"{path}:{number}: not a field: {raw}")
        value = value.strip()
        if value.startswith(('"', "[")) or value in ("true", "false"):
            try:
                value = json.loads(value)
            except json.JSONDecodeError as e:
                raise SystemExit(f"{path}:{number}: {e}: {raw}")
        if key == "pages":
            continue
        if key == "page" and item:
            page = {"page": value, "headings": []}
            pages.append(page)
        elif key == "heading" and item:
            heading = {"heading": value, "rows": []}
            page["headings"].append(heading)
        elif key == "id" and item:
            entry = {"id": value}
            heading["rows"].append(entry)
        elif key in ("headings", "rows"):
            continue
        elif indent >= 12 and entry is not None:
            entry[key] = value
        elif indent >= 8 and heading is not None:
            heading[key] = value
        elif page is not None:
            page[key] = value
    return pages


def yaml_entries(pages):
    return {e["id"]: e for p in pages for h in p["headings"] for e in h["rows"]}


# ------------------------------------------------------------------------------------ baking

def c(s):
    return json.dumps(s, ensure_ascii=False)


def bake_text(pages):
    lines = ["// Generated by ps5/tools/settings-text.py from settings-text.yaml: do not edit; edit the YAML and run",
             "// `ps5/tools/settings-text.py bake`. Each settings row's words by its id, and where the revamp puts it.",
             "#pragma once", "", "namespace bp", "{", "", "struct SettingsText", "{",
             "\tconst char* id;", "\tconst char* label;", "\tconst char* desc;",
             "\tconst char* choices; // the names, each ended by a \\0 (\"\" for none)",
             "\tconst char* page;    // where it is today",
             "\tconst char* heading;",
             "\tconst char* area;    // the revamp's",
             "\tconst char* section;",
             "\tint level;           // 0 Essentials, 1 More, 2 Everything",
             "\tbool dynamic;        // its words come from data, not from here",
             "};", "", "inline constexpr SettingsText settingsText[] = {"]
    levels = list(LEVELS.values())
    for p in pages:
        for h in p["headings"]:
            for e in h["rows"]:
                # (Each name its own literal, then "\0" as one: a \0 before a digit would be another number.)
                choices = " ".join(f'{c(n)} "\\0"' for n in e.get("choices", [])) or '""'
                lines.append(f"\t{{{c(e['id'])}, {c(e.get('label', ''))}, {c(e.get('desc', ''))}, {choices}, {c(p.get('name', ''))}, "
                             f"{c(h.get('label', ''))}, {c(e.get('area', ''))}, {c(e.get('section', ''))}, "
                             f"{levels.index(e['level']) if e.get('level') in levels else 0}, {'true' if e.get('dynamic') else 'false'}}},")
    lines += ["};", "", "} // namespace bp", ""]
    return "\n".join(lines)


# ------------------------------------------------------------------------------ the revamp's words (phase 2)

TEXT_KINDS = ("exact", "written", "exact-but-wrong")
LABEL_KINDS = ("exact", "written")
CHOICE_KINDS = ("exact", "mixed", "written", "exact-but-wrong")
# Words PCSX2's voice has no place for, in written words (labels, descriptions, choice names). A name in double quotes
# (a folder or a file the user must know, such as "Crux2") is left out of the search.
BANNED = [(re.compile(p), why) for p, why in (
    (r"Crux2|crux2|CRUX2", "names the title (\"Crux2\")"),
    (r"\b[Oo]ur\b", "\"our\""),
    (r"\bas before\b|\(before\)|\bof before\b", "history (\"as before\")"),
    (r"\(new\)|\bnew:|\bNew:", "history (\"new\")"),
    (r"'s own\b(?! (?:settings|DNS|account|core))", "\"own\" (whose?)"),
    (r"PS5PX", "an implementation name"),
    (r"\bv\d+\.\d+|\b\d+\.\d+\.\d+\b", "a version number"),
    (r"(?:^|[.!?:;]\s+)[Nn]ow\b", "\"now\" at a sentence's start"),
    (r"\bPS5's\b", "\"PS5's\" (the console's, or nothing)"),
    (r"[Tt]akes effect", "\"takes effect\" (PCSX2 never says it; the row's NextStart/Restart flag does)"),
    (r"[Ee]xperimental", "\"experimental\" (PCSX2 states the risk instead)"),
    (r"\b(?:[Cc]olour|[Cc]entre|[Gg]rey|[Ll]icence|[Bb]ehaviour|[Ff]avourite|\w*(?:optimis|customis|recognis|normalis|synchronis|"
     r"initialis|minimis|maximis|prioritis|organis|personalis|visualis))", "British spelling (PCSX2 writes American)"),
)]
# Rows that share one written description on purpose (the same thing for two players, two sticks, two ports).
TWINS = [{"action:test-player-1-s-microphone", "action:test-player-2-s-microphone"}, {"PS5.StickCurveLeft", "PS5.StickCurveRight"},
         {"Pad1.Type", "Pad2.Type"}]


def banned(text):
    bare = re.sub(r'"[^"]*"', '""', text or "")
    return [why for pat, why in BANNED if pat.search(bare)]


def pcsx2_words_at(where):
    """Every text PCSX2 has at `where`: the statement's, and the array's when one starts there."""
    words = list(texts_at(where))
    try:
        words += [w for w in array_at(where) if w not in words]
    except (ValueError, IndexError):
        pass
    return words


def same_value(a, b):
    try:
        return abs(float(a) - float(b)) < 1e-6
    except (TypeError, ValueError):
        return a == b


def check_new_words(have, rows):
    """Phase 2's checks of the new words: each row has its kinds; an exact text is PCSX2's at the pin byte for byte; a
    written one has none of the banned words; no description is empty or another row's by accident; a choice called
    (Default) is the default."""
    problems = []
    written = {}
    for rid, e in have.items():
        r = rows.get(rid, {})
        where = f"{rid} ({e.get('label')}, {e.get('area')} > {e.get('section')})"
        if e.get("text") not in TEXT_KINDS:
            problems.append(f"{where}: text {e.get('text')!r} is not one of {TEXT_KINDS}")
            continue
        if "new_desc" not in e:
            problems.append(f"{where}: no new_desc")
            continue
        if e.get("label_text") is not None and e["label_text"] not in LABEL_KINDS:
            problems.append(f"{where}: label_text {e['label_text']!r} is not one of {LABEL_KINDS}")
        if e.get("label_text") and not e.get("new_label"):
            problems.append(f"{where}: label_text without a new_label")
        if e.get("choices"):
            if len(e.get("new_choices") or []) != len(e["choices"]):
                problems.append(f"{where}: {len(e.get('new_choices') or [])} new choice names for {len(e['choices'])} choices")
            if e.get("choices_text") not in CHOICE_KINDS:
                problems.append(f"{where}: choices_text {e.get('choices_text')!r} is not one of {CHOICE_KINDS}")
        # (a) PCSX2's words, byte for byte, where they are said to be.
        if e["text"] in ("exact", "exact-but-wrong"):
            src = e.get("text_source")
            if not src:
                problems.append(f"{where}: text {e['text']} without a text_source")
            elif e["new_desc"] == "" and e.get("kind") == "bind":
                if e.get("new_label") not in texts_at(src):
                    problems.append(f"{where}: the binding's name {e.get('new_label')!r} is not PCSX2's at {src}")
            elif e["new_desc"] not in texts_at(src):
                found = texts_at(src)
                problems.append(f"{where}: new_desc is not PCSX2's text at {src} (there: {found[:3]!r})")
        if e.get("label_text") == "exact":
            src = e.get("label_source")
            if not src or e["new_label"] not in texts_at(src):
                problems.append(f"{where}: new_label {e.get('new_label')!r} is not PCSX2's at {src}")
        if e.get("choices_text") in ("exact", "exact-but-wrong", "mixed"):
            src = e.get("choices_source")
            theirs = pcsx2_words_at(src) if src else []
            mine = [n for n in e.get("new_choices", []) if n in theirs]
            if not src:
                problems.append(f"{where}: choices_text {e['choices_text']} without a choices_source")
            elif e["choices_text"] != "mixed" and len(mine) != len(e.get("new_choices", [])):
                problems.append(f"{where}: choices {[n for n in e['new_choices'] if n not in theirs]!r} are not PCSX2's at {src}")
            elif e["choices_text"] == "mixed" and not mine:
                problems.append(f"{where}: no choice name is PCSX2's at {src}")
        # (b) the banned words, in written words.
        own = []
        if e.get("label_text") == "written":
            own.append(("label", e["new_label"]))
        if e["text"] == "written":
            own.append(("desc", e["new_desc"]))
        if e.get("choices_text") in ("written", "mixed"):
            theirs = pcsx2_words_at(e["choices_source"]) if e.get("choices_source") else []
            own += [("choice", n) for n in e.get("new_choices", []) if n not in theirs]
        for what, text in own:
            for why in banned(text):
                problems.append(f"{where}: written {what} {text!r}: {why}")
        # (c) empty descriptions (a binding's and an information line's may be: PCSX2 shows none either / the line shows
        # its value), and written descriptions shared by accident.
        if not e["new_desc"].strip() and e.get("kind") not in ("bind", "info"):
            problems.append(f"{where}: empty new_desc")
        if e["text"] == "written" and e["new_desc"].strip():
            written.setdefault(e["new_desc"], []).append(rid)
        # (e) a choice called (Default) is the row's default.
        default = r.get("default")
        if e.get("choices_text") != "exact-but-wrong" and r.get("choices") and default is not None:
            for name, choice in zip(e.get("new_choices", []), r["choices"]):
                if "(Default)" in name and not same_value(choice["value"], default):
                    problems.append(f"{where}: choice {name!r} says (Default), but the default is {default!r}")
    for desc, ids in written.items():
        bases = {i.split("@")[0] for i in ids}
        if len(bases) > 1 and not any(set(ids) <= t for t in TWINS):
            problems.append(f"the same written description in {', '.join(sorted(ids))}: {desc[:60]!r}...")
    return problems


def apply(args):
    """Makes the new words the row's words (for phase 3), leaving every exact-but-wrong field as it is."""
    pages = read_yaml(YAML)
    kept, changed = [], 0
    for p in pages:
        for h in p["headings"]:
            for e in h["rows"]:
                if "new_label" in e and not e.get("dynamic"):
                    changed += e["label"] != e["new_label"]
                    e["label"] = e.pop("new_label")
                if "new_desc" in e:
                    if e.get("text") == "exact-but-wrong":
                        kept.append(f"{e['id']}: description (exact-but-wrong)")
                    else:
                        changed += e.get("desc", "") != e["new_desc"]
                        e["desc"] = e.pop("new_desc")
                if "new_choices" in e:
                    if e.get("choices_text") == "exact-but-wrong":
                        kept.append(f"{e['id']}: choices (exact-but-wrong)")
                    else:
                        changed += e.get("choices") != e["new_choices"]
                        e["choices"] = e.pop("new_choices")
    dump_yaml(pages, YAML)
    HEADER.write_text(bake_text(pages))
    for k in kept:
        print("  left as it is: " + k)
    print(f"settings-text: applied {changed} changed field(s); wrote {YAML.name} and {HEADER.name}")
    print("  (check now fails its comparison with the code's words until the interface takes its words from the table: phase 3;\n"
          "   an action's or an information line's id is made from its label: bless the inventory for those, settings-inventory.md)")
    return 0


def review(args):
    """The review sheet for the owner: one HTML page."""
    if not args.out:
        raise SystemExit("settings-text: review needs --out FILE (a work folder outside the repository)")
    import html as H
    inventory = load_inventory(args)
    rows = {r["id"]: r for r in inventory["rows"] if r["kind"] != "heading"}
    pages = read_yaml(YAML)
    have = yaml_entries(pages)
    esc = lambda s: H.escape(s or "").replace("\n", "<br>")
    kinds = {k: [i for i, e in have.items() if e.get("text") == k] for k in TEXT_KINDS}
    relabel = [i for i, e in have.items() if e.get("new_label") and e["new_label"] != e.get("label")]
    rechoice = [i for i, e in have.items() if e.get("new_choices") and e["new_choices"] != e.get("choices")]
    differ = [i for i, e in have.items() if e.get("text") in ("exact", "exact-but-wrong") and e.get("pcsx2_tooltip")]
    wrong = [i for i, e in have.items() if e.get("text") == "exact-but-wrong" or e.get("choices_text") == "exact-but-wrong"]
    doubt = [i for i, e in have.items() if e.get("doubtful")]
    ids_relabel_action = [i for i in relabel if have[i].get("kind") in ("action", "info")]

    def link(i):
        return f'<a href="#{H.escape(i)}">{H.escape(i)}</a>'

    out = ["<!doctype html><html lang=en><head><meta charset=utf-8><meta name=viewport content='width=device-width, initial-scale=1'>",
           "<title>Settings Words Review</title><style>",
           ":root{--ink:#1b1f24;--soft:#57606a;--line:#d0d7de;--bg:#ffffff;--band:#f6f8fa;--old:#fff1f0;--new:#effaf1;--warn:#fff8c5}",
           "@media (prefers-color-scheme: dark){:root:not([data-theme=light]){--ink:#1b1f24;--soft:#57606a;--line:#d0d7de;--bg:#ffffff;"
           "--band:#f6f8fa;--old:#fff1f0;--new:#effaf1;--warn:#fff8c5}}",
           ":root[data-theme=dark]{--ink:#1b1f24;--soft:#57606a;--line:#d0d7de;--bg:#ffffff;--band:#f6f8fa;--old:#fff1f0;--new:#effaf1;--warn:#fff8c5}",
           "body{background:var(--bg);color:var(--ink);font:15px/1.5 system-ui,-apple-system,Segoe UI,sans-serif;margin:0 auto;max-width:1100px;padding:16px}",
           "h1{font-size:24px}h2{font-size:20px;border-bottom:2px solid var(--ink);margin-top:40px}h3{font-size:16px;margin-top:28px;color:var(--soft)}",
           ".row{border:1px solid var(--line);border-radius:6px;padding:10px 12px;margin:10px 0;overflow-wrap:anywhere}",
           ".id{font:12px ui-monospace,monospace;color:var(--soft)}.k{display:inline-block;font-size:12px;border:1px solid var(--ink);border-radius:10px;"
           "padding:0 7px;margin-right:4px}.k.wrong{background:var(--warn)}",
           "table{border-collapse:collapse;width:100%;margin-top:6px}td{vertical-align:top;padding:4px 6px;border-top:1px solid var(--line)}",
           "td.f{width:90px;color:var(--soft);font-size:13px}td.o{background:var(--old);width:45%}td.n{background:var(--new);width:45%}",
           ".src{font:12px ui-monospace,monospace;color:var(--soft)}.note{background:var(--warn);padding:4px 6px;margin-top:6px;font-size:14px}",
           "ul{padding-left:20px}li{margin:2px 0}.same{color:var(--soft)}</style></head><body>",
           "<h1>Settings words: review sheet (phase 2)</h1>",
           "<p>Every row of the settings, grouped by the revamp's area and section: today's words on the left, the new on the right. "
           "<b>exact</b>: PCSX2's text at the pin (without ps5.patch), byte for byte, at the source shown. <b>written</b>: in PCSX2's voice. "
           "<b>exact-but-wrong</b>: PCSX2's text, untrue of this row: the owner decides (apply leaves today's words). "
           "Nothing on the console changes until these are applied (phase 3).</p>",
           "<h2>Summary</h2><ul>",
           f"<li>Rows: {len(have)}. Descriptions: exact {len(kinds['exact'])}, written {len(kinds['written'])}, "
           f"exact-but-wrong {len(kinds['exact-but-wrong'])}.</li>",
           f"<li>Labels changed: {len(relabel)} (of them actions and information lines, whose ids are made from the label: "
           f"{len(ids_relabel_action)}). Rows with changed choice names: {len(rechoice)}.</li>",
           f"<li>Labels: exact {sum(1 for e in have.values() if e.get('label_text') == 'exact')}, written "
           f"{sum(1 for e in have.values() if e.get('label_text') == 'written')}. Choice names: "
           + ", ".join(f"{k} {sum(1 for e in have.values() if e.get('choices_text') == k)}" for k in CHOICE_KINDS) + ".</li></ul>",
           f"<h3>Exact-but-wrong ({len(wrong)})</h3><ul>"]
    out += [f"<li>{link(i)}: {esc(have[i].get('text_note', ''))}</li>" for i in wrong]
    out += [f"</ul><h3>FullscreenUI's text and the Qt tooltip differ ({len(differ)}): FullscreenUI's is used</h3><ul>"]
    out += [f"<li>{link(i)}</li>" for i in differ]
    out += [f"</ul><h3>Doubtful places ({len(doubt)})</h3><ul>"]
    out += [f"<li>{link(i)}: {esc(have[i]['area'])} &gt; {esc(have[i]['section'])}: {esc(have[i]['doubtful'])}</li>" for i in doubt]
    out.append("</ul>")
    groups = {}
    for p in pages:
        for h in p["headings"]:
            for e in h["rows"]:
                groups.setdefault(e["area"], {}).setdefault(e["section"], []).append((p["name"], h["label"], e))
    for area in AREAS:
        if area not in groups:
            continue
        out.append(f"<h2>{esc(area)}</h2>")
        for section, items in groups[area].items():
            out.append(f"<h3>{esc(section)}</h3>")
            for page, heading, e in items:
                badges = f"<span class='k{' wrong' if e.get('text') == 'exact-but-wrong' else ''}'>text: {esc(e.get('text'))}</span>"
                if e.get("label_text"):
                    badges += f"<span class=k>label: {esc(e['label_text'])}</span>"
                if e.get("choices_text"):
                    badges += f"<span class='k{' wrong' if e['choices_text'] == 'exact-but-wrong' else ''}'>choices: {esc(e['choices_text'])}</span>"
                out.append(f"<div class=row id='{H.escape(e['id'])}'><div class=id>{esc(e['id'])} · today: {esc(page)} / {esc(heading)} · "
                           f"{esc(e.get('level'))}</div>{badges}<table>")

                def pair(field, old, new, src=None):
                    same = old == new
                    out.append(f"<tr><td class=f>{field}</td><td class=o>{esc(old)}</td><td class='n{' same' if same else ''}'>"
                               f"{'(the same)' if same and old else esc(new)}"
                               + (f"<div class=src>{esc(src)}</div>" if src else "") + "</td></tr>")
                if e.get("new_label") is not None:
                    pair("label", e.get("label"), e["new_label"], e.get("label_source"))
                else:
                    pair("label", e.get("label"), e.get("label"), "its words come from data")
                pair("description", e.get("desc"), e.get("new_desc"), e.get("text_source"))
                if e.get("choices"):
                    pair("choices", " · ".join(e["choices"]), " · ".join(e.get("new_choices", [])), e.get("choices_source"))
                out.append("</table>")
                if e.get("text_note"):
                    out.append(f"<div class=note>{esc(e['text_note'])}</div>")
                if e.get("pcsx2_tooltip") and e.get("text") != "written":
                    out.append(f"<div class=src>Qt's tooltip ({esc(e.get('pcsx2_tooltip_source'))}): {esc(e['pcsx2_tooltip'])}</div>")
                if e.get("text") == "exact-but-wrong" or (e.get("text") == "written" and e.get("pcsx2_description")):
                    out.append(f"<div class=src>PCSX2's text: {esc(e.get('pcsx2_description'))}</div>")
                if e.get("doubtful"):
                    out.append(f"<div class=src>Doubtful place: {esc(e['doubtful'])}</div>")
                out.append("</div>")
    out.append("</body></html>")
    Path(args.out).write_text("\n".join(out) + "\n")
    print(f"settings-text: wrote {args.out}")
    return 0


# ------------------------------------------------------------------------------------ commands

def load_inventory(args):
    """The inventory: made from a dump of the code as it is now (--dump), or the baseline."""
    if args.dump:
        import importlib.util
        spec = importlib.util.spec_from_file_location("settings_inventory", Path(__file__).with_name("settings-inventory.py"))
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module.make(json.loads(Path(args.dump).read_text()))
    return json.loads(Path(args.inventory).read_text())


def make(args):
    inventory = load_inventory(args)
    old = yaml_entries(read_yaml(YAML)) if YAML.exists() else {}
    pages = entries(inventory, pcsx2_text(), pcsx2_keys(), old)
    write_yaml(pages, YAML)
    ids = {e["id"] for p in pages.values() for h in p["headings"].values() for e in h["rows"]}
    for gone in sorted(set(old) - ids):
        print(f"  dropped (no such row any more): {gone}")
    print(f"settings-text: wrote {YAML.relative_to(ROOT)}: {len(ids)} rows on {len(pages)} pages")
    return 0


def check(args):
    inventory = load_inventory(args)
    pages = read_yaml(YAML)
    have = yaml_entries(pages)
    problems = []
    rows = {r["id"]: r for r in inventory["rows"] if r["kind"] != "heading"}
    for rid in rows.keys() - have.keys():
        problems.append(f"no entry for the row {rid} ({rows[rid]['label']}, {rows[rid]['page']}): settings-text.py make adds it")
    for rid in have.keys() - rows.keys():
        problems.append(f"stale entry: {rid} is no row of the inventory")
    count = sum(len(h["rows"]) for p in pages for h in p["headings"])
    if count != len(have):
        problems.append(f"{count - len(have)} id(s) twice")
    headings = {(r["page"], r["label"]) for r in inventory["rows"] if r["kind"] == "heading"}
    named = {(p.get("name"), h.get("label")) for p in pages for h in p["headings"] if h.get("label")}
    for page, label in sorted(headings - named):
        problems.append(f"no entry for the heading {label} of {page}")
    for rid, e in have.items():
        if e.get("area") not in AREAS:
            problems.append(f"{rid}: area {e.get('area')!r} is not one of {AREAS}")
        if e.get("level") not in LEVELS.values():
            problems.append(f"{rid}: level {e.get('level')!r} is not one of {list(LEVELS.values())}")
        if e.get("origin") not in ("pcsx2", "crux2"):
            problems.append(f"{rid}: origin {e.get('origin')!r}")
        if not e.get("section"):
            problems.append(f"{rid}: no section")
        r = rows.get(rid)
        if r is None:
            continue
        # Phase 1: the file's words are the code's (the interface does not take them from here yet).
        if e.get("label") != r["label"]:
            problems.append(f"{rid}: label {e.get('label')!r} is not the code's {r['label']!r}")
        if e.get("desc", "") != r.get("desc", ""):
            problems.append(f"{rid}: desc is not the code's")
        if e.get("choices", []) != [c_["name"] for c_ in r.get("choices", [])]:
            problems.append(f"{rid}: choices are not the code's")
    # Phase 2: the new words (they are not the interface's yet).
    problems += check_new_words(have, rows)
    baked = bake_text(pages)
    if not HEADER.exists() or HEADER.read_text() != baked:
        problems.append(f"{HEADER.name} is not what bake writes: run ps5/tools/settings-text.py bake")
    if args.baked:
        # The table as a compiler read it (run-inventory-test.sh prints it): the code's words, every one.
        table = {e["id"]: e for e in json.loads(Path(args.baked).read_text())}
        if len(table) != len(rows):
            problems.append(f"the compiled table has {len(table)} rows, the inventory {len(rows)}")
        for rid, r in rows.items():
            t = table.get(rid)
            if t is None:
                problems.append(f"the compiled table has no {rid}")
            elif (t["label"], t["desc"], t["choices"]) != (r["label"], r.get("desc", ""), [c_["name"] for c_ in r.get("choices", [])]):
                problems.append(f"the compiled table's words for {rid} are not the code's")
    for p in problems:
        print("  " + p)
    print(f"settings-text: {len(have)} entries, {len(problems)} problem(s)")
    return 1 if problems else 0


def bake(args):
    text = bake_text(read_yaml(YAML))
    if args.check:
        same = HEADER.exists() and HEADER.read_text() == text
        print("settings-text: bp_settings_text.h is up to date" if same else "settings-text: bp_settings_text.h is OUT OF DATE (bake)")
        return 0 if same else 1
    HEADER.write_text(text)
    print(f"settings-text: wrote {HEADER.relative_to(ROOT)}")
    return 0


def doubtful(args):
    for e in yaml_entries(read_yaml(YAML)).values():
        if e.get("doubtful"):
            print(f"{e['id']:55} {e['area']} > {e['section']}: {e['doubtful']}")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("command", choices=["make", "check", "bake", "doubtful", "review", "apply"])
    ap.add_argument("--inventory", default=str(INVENTORY), help="the inventory (default: the baseline)")
    ap.add_argument("--dump", help="an inventory dump of the code (build-inventory.sh's dump.json) in place of the baseline")
    ap.add_argument("--check", action="store_true", help="bake: only say whether the header is up to date")
    ap.add_argument("--baked", help="check: the compiled table, printed as JSON, to compare with the code's words")
    ap.add_argument("--out", help="review: the page to write (outside the repository)")
    args = ap.parse_args()
    return {"make": make, "check": check, "bake": bake, "doubtful": doubtful, "review": review, "apply": apply}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
