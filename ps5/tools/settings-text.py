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


def pcsx2_index():
    """PCSX2's words for each setting: {(section, key): {"fsui": (title, summary, where), "qt": (...)}}, and every
    translatable text of its controller and hotkey tables: {text: where}."""
    by_key = {}
    fsui_path = "pcsx2/ImGui/FullscreenUI_Settings.cpp"
    for path in (fsui_path, "pcsx2/ImGui/FullscreenUI.cpp"):
        src = pristine(path)
        for m in re.finditer(r"\bDraw(Toggle|IntList|IntRange|IntSpinBox|FloatRange|FloatSpinBox|StringList|FloatList|Enum|IPAddress|"
                             r"IntRect|Clamping)\w*Setting\(\s*bsi\s*,", src):
            args = call_args(src, m.end())
            if len(args) < 3:
                continue
            line = src.count("\n", 0, m.start()) + 1
            title = literals(args[0])
            summary = literals(args[1]) if ("CSTR" in args[1] or args[1].strip().startswith('"')) else ""
            keys = []
            if m.group(1) == "Clamping":
                vu = args[2].strip()
                keys = [("EmuCore/CPU/Recompiler", {"-1": "fpuOverflow", "0": "vu0Overflow", "1": "vu1Overflow"}.get(vu, ""))]
            elif len(args) >= 4 and args[2].strip().startswith('"') and args[3].strip().startswith('"'):
                keys = [(literals(args[2]), literals(args[3]))]
            for key in keys:
                if key[1]:
                    by_key.setdefault(key, {}).setdefault("fsui", (title, summary, f"{path}:{line}"))
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
            widget = args[0].strip()
            line = src.count("\n", 0, m.start()) + 1
            for key in widgets.get(widget, []):
                by_key.setdefault(key, {}).setdefault("qt", (literals(args[1]), literals(args[3]), f"{path}:{line}"))
    texts = {}
    for path in pcsx2_files():
        if not (path.startswith("pcsx2/SIO/Pad/") or path.startswith("pcsx2/USB/") or path in ("pcsx2/Hotkeys.cpp", "pcsx2/GS/GS.cpp")):
            continue
        src = pristine(path)
        for m in re.finditer(r"TRANSLATE_NOOP\(\s*\"[^\"]*\"\s*,\s*((?:\"(?:[^\"\\]|\\.)*\"\s*)+)\)", src):
            texts.setdefault(literals(m.group(1)), f"{path}:{src.count(chr(10), 0, m.start()) + 1}")
    return by_key, texts


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


# A PCSX2 text that names games (rule 4: no game titles in anything published) is not copied: its place is given
# instead. It names one when it says "games like", "e.g. X" and the like, or when it has a run of capitalised
# words that is not one of the technical names below, or a capitalised word inside a sentence that is a whole
# title of PCSX2's game database.
NAMES_GAMES = re.compile(r"\b(games? (?:like|such as)|following games|(?:e\.g\.|for example|for instance),? (?!DualShock|Xbox)[A-Z]|SOTC|"
                         r"Known to affect)", re.I)
TECHNICAL = {"discord presence", "emotion engine", "emotion engine cpu", "auto standard", "aspect ratio", "contrast adaptive sharpening",
             "forced excluding sprites", "force 32bit", "rasterizer ordered view", "sprite renderer", "unscale point", "memory clearing",
             "dolby pro logic", "pro logic ii", "hard disk drive", "xbox one", "analog sensitivity", "floating point unit", "vector unit",
             "scratchpad mfifo", "playstation 2", "playstation 5", "full screen", "game list", "big picture", "save state", "game fixes",
             "dualshock 4", "dualshock 2", "game database"}


def game_titles():
    """The titles of PCSX2's game database (its names and English names), as words, and every first few words of them."""
    titles, starts = set(), set()
    index = VENDOR / "bin" / "resources" / "GameIndex.yaml"
    if index.exists():
        for line in index.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith(("  name:", "  name-en:")):
                words = re.findall(r"[a-z0-9']+", line.split(":", 1)[1].strip().strip('"').lower())
                titles.add(" ".join(words))
                for n in range(2, len(words) + 1):
                    starts.add(" ".join(words[:n]))
    return titles, starts


# Names of games PCSX2's texts give by a short name, a series or a studio, which the database's titles do not
# begin with.
NAMED_GAMES = {"socom", "tales", "ace games", "snowblind", "sotc", "namco", "jak", "gta", "goemon"}


def names_games(text, titles):
    """Whether the text names a game: "games like", "e.g. X" and the like, two or more capitalised words that a
    title of the database begins with, a capitalised word inside a sentence that is a whole title, or a name above."""
    whole, starts = titles
    if NAMES_GAMES.search(text):
        return True
    for m in re.finditer(r"\b[A-Z][A-Za-z0-9']*(?: [A-Z0-9][A-Za-z0-9']*)+", text):
        words = re.findall(r"[a-z0-9']+", m.group(0).lower())
        for i in range(len(words)):
            for j in range(i + 2, len(words) + 1):
                run = " ".join(words[i:j])
                if run in starts and run not in TECHNICAL:
                    return True
    for m in re.finditer(r"(?<=[a-z,;(] )([A-Z][a-z']{2,})\b", text):
        if m.group(1).lower() in whole:
            return True
    return any(re.search(r"\b" + re.escape(n) + r"\b", text, re.I) for n in NAMED_GAMES)


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


def entries(inventory, index, keys, old):
    """The file's pages, headings and entries from the inventory (and what `old` has of them by hand)."""
    by_key, texts = index
    titles = game_titles()
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
        crux2 = row.get("section") in ("PS5", "PS5UI", "PS5GamePatch") or row["kind"] in ("action", "info") or \
            (row.get("key") and row["kind"] != "bind" and row["key"] not in keys and row.get("section") not in ("Patches", "Cheats") and
             not re.fullmatch(r"Pad\d", row.get("section", "")))
        e["origin"] = "crux2" if crux2 else "pcsx2"
        if e["origin"] == "pcsx2":
            found = by_key.get((row.get("section"), row.get("key")), {})
            first = found.get("fsui") or found.get("qt")
            if first is None and row.get("desc") in texts:
                first = (None, row["desc"], texts[row["desc"]])
            if first is None and row["label"] in texts:
                first = (row["label"], None, texts[row["label"]])
            if first is not None:
                title, text, where = first
                if title and names_games(title, titles):
                    e["pcsx2_title_withheld"] = "PCSX2's label names a game (rule 4): read it at its source"
                elif title:
                    e["pcsx2_title"] = title
                if text and names_games(text, titles):
                    e["pcsx2_description_withheld"] = "PCSX2's text names games (rule 4): read it at its source"
                elif text:
                    e["pcsx2_description"] = text
                e["pcsx2_source"] = where
                if "fsui" in found and "qt" in found and found["qt"][1] and found["qt"][1] != text:
                    qt = found["qt"]
                    if names_games(qt[1], titles):
                        e["pcsx2_tooltip_withheld"] = "PCSX2's text names games (rule 4): read it at its source"
                    else:
                        e["pcsx2_tooltip"] = qt[1]
                    e["pcsx2_tooltip_source"] = qt[2]
        if row["layers"] != ["shared", "game"]:
            e["layers"] = row["layers"][0] if len(row["layers"]) == 1 else row["layers"]
        if dynamic(row):
            e["dynamic"] = True
        if doubt:
            e["doubtful"] = doubt
        # What was written by hand is kept.
        if row["id"] in old:
            for field in ("label", "desc", "choices", "area", "section", "level", "doubtful"):
                if field in old[row["id"]]:
                    e[field] = old[row["id"]][field]
        heading["rows"].append(e)
    return pages


FIELDS = ("id", "label", "desc", "choices", "kind", "area", "section", "level", "level_one_game", "origin", "layers", "dynamic", "pcsx2_title",
          "pcsx2_title_withheld", "pcsx2_description", "pcsx2_description_withheld", "pcsx2_source", "pcsx2_tooltip", "pcsx2_tooltip_withheld",
          "pcsx2_tooltip_source", "doubtful")


def write_yaml(pages, path):
    out = ["# Crux2 - every setting's words, for editing by hand (ps5/tools/settings-text.py: make, check, bake).",
           "# Under each page and heading as the interface shows them today. Change a label, desc or choices; area,",
           "# section and level place the row in the revamp's eight areas. Never change an id. A row marked dynamic",
           "# takes its words from data. pcsx2_* is PCSX2's own text, word for word, and where it is (for reference).",
           "pages:"]
    for name, page in pages.items():
        out.append(f"  - page: {q(slug(name))}")
        out.append(f"    name: {q(name)}")
        out.append("    headings:")
        for hlabel, heading in page["headings"].items():
            out.append(f"      - heading: {q(slug(hlabel))}")
            out.append(f"        label: {q(hlabel)}")
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
    pages = entries(inventory, pcsx2_index(), pcsx2_keys(), old)
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
    ap.add_argument("command", choices=["make", "check", "bake", "doubtful"])
    ap.add_argument("--inventory", default=str(INVENTORY), help="the inventory (default: the baseline)")
    ap.add_argument("--dump", help="an inventory dump of the code (build-inventory.sh's dump.json) in place of the baseline")
    ap.add_argument("--check", action="store_true", help="bake: only say whether the header is up to date")
    ap.add_argument("--baked", help="check: the compiled table, printed as JSON, to compare with the code's words")
    args = ap.parse_args()
    return {"make": make, "check": check, "bake": bake, "doubtful": doubtful}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
