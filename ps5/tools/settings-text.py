#!/usr/bin/env python3
# Crux2 - the settings' words in one file a person can edit (ps5/frontend/bigpicture/settings-text.yaml).
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
#
#   ps5/tools/settings-text.py extract <dump.json> [--out FILE]
#       Writes the file from the interface as it is now: the rows ps5/tests/bigpicture's preview
#       dumps (`preview dump-settings <fonts>` runs the real bp_options.cpp pages on the PC) and the
#       rows bp_pcsx2.cpp builds on the console, read from its source (Heading/Toggle/Action/Range/
#       Line calls and `o.label = "..."` fields). Every setting is listed once, under the page and
#       heading it is shown in, with its label and description.
#
#   ps5/tools/settings-text.py bake [--in FILE] [--out bp_settings_text.inc] [--check]
#       Writes the .inc the interface compiles in: for each row id its label, description, page,
#       heading and order. --check only says whether the .inc on disk is what this would write.
#
#   ps5/tools/settings-text.py lint [--in FILE]
#       Says what is wrong with the file: an id twice, a label or description missing, a
#       description long enough to scroll (about 120 characters at the row's width), a page or
#       heading the interface does not have.
#
# The file's shape (YAML, by hand):
#
#   pages:
#     - page: graphics                     # fixed id: which page's code builds its rows
#       name: Graphics                     # the name shown
#       headings:
#         - heading: display               # fixed id
#           label: DISPLAY                 # the name shown
#           rows:
#             - id: EmuCore/GS.Renderer    # fixed id (section.key; headings and actions have ids of their own)
#               label: Renderer
#               desc: Which part draws ...
#
# What may be changed by hand: every `name`, `label` and `desc`; the order of rows under a heading;
# the order of headings on a page; and a row may be moved to another heading or page by moving its
# entry (the row's behaviour, values and conditions stay in code). An id is `section.key`, or
# `section.key@page/heading` where one setting is shown in two places. What may not: an `id` (the code
# finds the row by it), or `dynamic: true` rows, which the interface builds from data (a game's
# patches, the controller's settings) and whose text the file cannot change.

import argparse
import collections
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
FRONT = ROOT / "ps5" / "frontend" / "bigpicture"
DEFAULT_YAML = FRONT / "settings-text.yaml"
DEFAULT_INC = FRONT / "bp_settings_text.inc"

# The section constants bp_options.cpp and bp_pcsx2.cpp write rows with.
SECTIONS = {
    "GS": "EmuCore/GS", "CORE": "EmuCore", "SPEED": "EmuCore/Speedhacks", "CPU": "EmuCore/CPU",
    "REC": "EmuCore/CPU/Recompiler", "FIX": "EmuCore/Gamefixes", "FRONT": "PS5UI", "OUT": "SPU2/Output",
    "TRACE": "EmuCore/TraceLog", "PAD": "Pad", "UI": "UI", "PS5": "PS5",
}


def slug(text):
    s = re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")
    return s[:48] or "row"


def row_id(row, page=None):
    """A row's fixed id: section.key for a setting, else kind:slug(label), with the page's id in front
    where two pages have a row of the same words (About and PlayStation 5 both have "Crux2")."""
    if row.get("section") and row.get("key"):
        return f"{row['section']}.{row['key']}"
    base = f"{row['kind']}:{slug(row['label'])}"
    return f"{PAGE_IDS.get(page, slug(page))}/{base}" if page and row["kind"] == "info" else base


# ---------------------------------------------------------------------------- bp_pcsx2.cpp rows

C_STRING = r'"((?:[^"\\]|\\.)*)"'
# Adjacent string literals, with the whitespace and comments between them: one text.
C_TEXT = rf'(?:{C_STRING}\s*)+'


def c_text(src):
    """The C++ string literal(s) at the start of src: (text, rest)."""
    parts = []
    i = 0
    while True:
        m = re.match(r'\s*' + C_STRING, src[i:], re.S)
        if not m:
            break
        parts.append(bytes(m.group(1), "utf-8").decode("unicode_escape").encode("latin-1").decode("utf-8", "replace"))
        i += m.end()
    return "".join(parts), src[i:]


def c_arg(src):
    """The next argument of a call (a string, an identifier, a number, a nested call): (arg, rest)."""
    depth = 0
    i = 0
    while i < len(src):
        c = src[i]
        if c == '"':
            j = i + 1
            while j < len(src) and not (src[j] == '"' and src[j - 1] != '\\'):
                j += 1
            i = j + 1
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            if depth == 0:
                return src[:i].strip(), src[i:]
            depth -= 1
        elif c == "," and depth == 0:
            return src[:i].strip(), src[i + 1:]
        i += 1
    return src.strip(), ""


def section_of(expr):
    expr = expr.strip()
    if expr.startswith('"'):
        return c_text(expr)[0]
    return SECTIONS.get(expr, expr)


def backend_rows(source):
    """The rows bp_pcsx2.cpp builds: Heading/Toggle/Action/Range/Line(...) and `x.label = "..."` fields,
    with the page function each is in and the heading above it, in source order."""
    rows = []
    page = None
    page_name = None
    heading = ""
    for m in re.finditer(r'bp::Page (\w+)\((?:[^)]*)\) override\s*\{\s*bp::Page page(?:\{[^,]*,\s*' + C_STRING + ')?', source):
        pass
    # Walk the page functions, in order of appearance.
    funcs = [(m.start(), m.group(1), m.group(2)) for m in re.finditer(
        r'(?:bp::Page|std::vector<bp::Option>) (\w+)\(([^)]*)\) override', source)]
    funcs.append((len(source), None, None))
    for (start, name, _), (end, _, _) in zip(funcs, funcs[1:]):
        body = source[start:end]
        if name is None:
            break
        nm = re.search(r'bp::Page page\{[^,]*,\s*(?:' + C_STRING + r'|(\w+) \? ' + C_STRING + r' : ' + C_STRING + ')', body)
        page_name = (nm.group(1) or nm.group(3)) if nm else name
        page = name
        heading = ""
        where = None
        for call in re.finditer(r'\b(Heading|Toggle|Action|Range|Line)\(\s*', body):
            kind = call.group(1)
            rest = body[call.end():]
            # extraRows: the `where` the rows are added under.
            before = body[:call.start()]
            ws = list(re.finditer(r'w == "(\w+)"', before))
            if ws and name == "extraRows":
                where = ws[-1].group(1)
            label, rest = c_text(rest)
            if not label:
                continue  # a label computed at run time (fmt::format, a variable): dynamic
            row = {"kind": kind.lower(), "label": label, "desc": "", "section": "", "key": "",
                   "page": page_name, "func": page, "where": where, "heading": heading}
            if kind == "Heading":
                heading = label
                row["kind"] = "heading"
                rows.append(row)
                continue
            if kind == "Line":
                row["kind"] = "info"
                rows.append(row)
                continue
            rest = rest.lstrip().lstrip(",")
            desc, rest = c_text(rest)
            row["desc"] = desc
            if kind in ("Toggle", "Range"):
                rest = rest.lstrip().lstrip(",")
                sect, rest = c_arg(rest)
                key, rest = c_arg(rest)
                row["section"] = section_of(sect)
                row["key"] = c_text(key)[0] if key.startswith('"') else key
                row["kind"] = "bool" if kind == "Toggle" else "intrange"
            else:
                row["kind"] = "action"
            rows.append(row)
        # Hand-built rows: `name.label = "..."` then `name.desc = "..."`, `name.section`, `name.key`.
        for m in re.finditer(r'\b(\w+)\.label = ' + C_STRING + ';', body):
            var = m.group(1)
            label = c_text('"' + m.group(2) + '"')[0]
            after = body[m.end():m.end() + 3000]
            d = re.search(rf'\b{var}\.desc = ((?:{C_STRING}\s*)+);', after)
            s = re.search(rf'\b{var}\.section = ([^;]+);', after)
            k = re.search(rf'\b{var}\.key = ([^;]+);', after)
            kd = re.search(rf'\b{var}\.kind = bp::Option::(\w+);', after)
            # The heading above it, by position.
            above = body[:m.start()]
            hs = list(re.finditer(r'Heading\(\s*' + C_STRING, above))
            row = {"kind": kd.group(1).lower() if kd else "bool", "label": label,
                   "desc": c_text(d.group(1))[0] if d else "", "section": section_of(s.group(1)) if s else "",
                   "key": (c_text(k.group(1))[0] if k and k.group(1).strip().startswith('"') else (k.group(1).strip() if k else "")),
                   "page": page_name, "func": page, "where": None, "heading": hs[-1].group(1) if hs else heading}
            if re.search(rf'\b{var}\.kind = bp::Option::Heading', after) or row["kind"] == "heading":
                continue
            rows.append(row)
    return rows


# -------------------------------------------------------------------------------- extraction

# Where each `extraRows(where)` hook's rows are shown (bp_options.cpp's addRows calls).
WHERE_TO_PAGE = {"library": "Game List", "interface": "Interface", "motion": "Controllers", "output": "Audio",
                 "picture": "Graphics", "controllers": "Controllers", "saves": "Memory Cards", "network": "Network & HDD",
                 "system": "PlayStation 5"}
# Backend pages the preview invents stand-ins for: their real rows come from the source.
BACKEND_PAGES = {"controllersPage": "Controllers", "hotkeysPage": "Controllers", "memoryCardsPage": "Memory Cards",
                 "networkPage": "Network & HDD", "foldersPage": "Folders", "biosPage": "BIOS", "patchesPage": None,
                 "backupPage": "Memory Cards", "achievementsPage": "Achievements", "logPage": "Debug", "aboutPage": None}
PAGE_IDS = {"Interface": "interface", "Game List": "game-list", "Folders": "folders", "BIOS": "bios", "Emulation": "emulation",
            "Graphics": "graphics", "On-Screen Display": "osd", "Audio": "audio", "Memory Cards": "memory-cards",
            "Network & HDD": "network", "Achievements": "achievements", "Controllers": "controllers", "Advanced": "advanced",
            "Debug": "debug", "PlayStation 5": "ps5", "Statistics": "statistics", "Patches": "patches", "Cheats": "cheats",
            "Game Fixes": "game-fixes"}


def yaml_str(s):
    if s == "" or re.search(r'[:#\[\]{}&*!|>\'"%@`,\n]|^\s|\s$|^(true|false|null|yes|no|~|-)$', s) or s[0].isdigit():
        return json.dumps(s, ensure_ascii=False)
    return s


def extract(dump_path, out_path):
    dump = json.load(open(dump_path))
    source = (FRONT / "bp_pcsx2.cpp").read_text()
    backend = backend_rows(source)

    # Preview rows: the shared pages first, then what only a game's pages have.
    pages = {}  # name -> {"id","headings": [(label, [rows])]}
    order = []

    def page_of(name):
        if name not in pages:
            pages[name] = {"id": PAGE_IDS.get(name, slug(name)), "headings": [], "index": {}}
            order.append(name)
        return pages[name]

    def add(name, heading, row, dynamic=False, only_game=False):
        # A key computed at run time (fmt::format, a variable): the row is one of a family (a card slot, a
        # port); its words come from the file by label under a family id, and the family is marked dynamic.
        if row.get("key") and not re.fullmatch(r"[A-Za-z0-9_./-]+", row["key"]):
            row = dict(row, key=re.sub(r"[^A-Za-z0-9_]+", "", re.sub(r'\{\}', "N", row["key"].split('"')[1] if '"' in row["key"] else row["key"])))
            dynamic = True
        if row["kind"] == "info" and not row.get("desc"):
            dynamic = True  # a line of text whose value the title computes
        pg = page_of(name)
        if heading not in pg["index"]:
            pg["index"][heading] = []
            pg["headings"].append((heading, pg["index"][heading]))
        rid = row_id(row, name)
        for r in pg["index"][heading]:
            if r["id"] == rid:
                return
        pg["index"][heading].append({"id": rid, "kind": row["kind"], "label": row["label"], "desc": row.get("desc", ""),
                                     "dynamic": dynamic, "only_game": only_game, "names": row.get("names", [])})

    preview_pages_backend = {"Folders", "Achievements", "Controllers", "Memory Cards", "Network & HDD", "BIOS"}
    # (wholly or mostly from the Backend: the preview invents stand-ins; the real rows are read from the source)
    seen_shared = set()
    for r in dump:
        if r["kind"] == "heading":
            continue
        if r["page"] in ("Patches", "Cheats"):
            continue
        if r["page"] in preview_pages_backend and not (r.get("section") and r.get("key") and r["section"] in SECTIONS.values()):
            continue  # a stand-in of the preview's; the Backend's real rows come from the source
        # The preview's invented rows on the shared Controllers/Memory Cards/Network/BIOS pages: those come from
        # bp_pcsx2.cpp's functions below; keep only rows that bp_options.cpp itself makes (they carry a key, or are
        # an action/info bp_options.cpp owns). Rows that the real backend defines with the same label are replaced.
        key = row_id(r, r["page"])
        only_game = r["pages"] == "game" and (r["page"], key) not in seen_shared
        if r["pages"] == "shared":
            seen_shared.add((r["page"], key))
        elif not only_game:
            continue
        add(r["page"], r["heading"], r, dynamic=(r["page"] == "Statistics"), only_game=only_game)

    # Backend rows, by their real page. (Headings are the structure, not rows; gameRows is the strip of
    # quick settings on a game's own screen, not a settings page.)
    for r in backend:
        if r["kind"] == "heading" or r["func"] in ("gameRows", "patchesPage"):
            continue
        if r["func"] == "extraRows":
            name = WHERE_TO_PAGE.get(r["where"])
            if name is None:
                continue
        else:
            name = BACKEND_PAGES.get(r["func"], r["page"])
            if name is None:
                name = r["page"]
        add(name, r["heading"], r)

    # The order bp_options.cpp shows pages in.
    shown = ["Interface", "Game List", "Folders", "BIOS", "Emulation", "Game Fixes", "Graphics", "On-Screen Display", "Audio",
             "Memory Cards", "Network & HDD", "Achievements", "Controllers", "Advanced", "Debug", "PlayStation 5", "Statistics",
             "About"]
    order.sort(key=lambda n: shown.index(n) if n in shown else len(shown))

    # A setting shown in two places (the accurate floating point switches are on a game's Emulation page and on
    # Advanced; the second internal resolution list; the file logging switch) is two rows with one section.key:
    # each gets the page and heading the code shows it under, after an "@", so that every id is one row's.
    counts = collections.Counter(r["id"] for n in order for _, rows in pages[n]["headings"] for r in rows)
    for n in order:
        pg = pages[n]
        for heading, rows in pg["headings"]:
            for r in rows:
                if counts[r["id"]] > 1:
                    r["id"] = f"{r['id']}@{pg['id']}/{slug(heading) if heading else 'top'}"

    out = []
    out.append("# Crux2 - every setting's words, for editing by hand (ps5/tools/settings-text.py bakes it in).")
    out.append("# Change any name, label or desc; reorder rows and headings; move a row's entry to another heading or")
    out.append("# page. Never change an id. Rows marked dynamic are built from data and their text is not from here.")
    out.append("# A desc longer than about 120 characters scrolls sideways while the row is focused.")
    out.append("pages:")
    total = 0
    for name in order:
        pg = pages[name]
        out.append(f"  - page: {pg['id']}")
        out.append(f"    name: {yaml_str(name)}")
        out.append("    headings:")
        for heading, rows in pg["headings"]:
            hid = slug(heading) if heading else "top"
            out.append(f"      - heading: {hid}")
            out.append(f"        label: {yaml_str(heading)}")
            out.append("        rows:")
            for r in rows:
                total += 1
                out.append(f"          - id: {yaml_str(r['id'])}")
                out.append(f"            label: {yaml_str(r['label'])}")
                if r["desc"]:
                    out.append(f"            desc: {yaml_str(r['desc'])}")
                if r["kind"] not in ("bool",):
                    out.append(f"            kind: {r['kind']}")
                if r["names"]:
                    out.append(f"            choices: [{', '.join(yaml_str(n) for n in r['names'])}]")
                if r["dynamic"]:
                    out.append("            dynamic: true")
                if r["only_game"]:
                    out.append("            only-game: true")
    Path(out_path).write_text("\n".join(out) + "\n")
    print(f"wrote {out_path}: {len(order)} pages, {total} rows")


# -------------------------------------------------------------------------------- yaml reading

def load_yaml(path):
    """A small reader for the file's own shape (no PyYAML needed): pages/headings/rows lists of key: value."""
    try:
        import yaml  # type: ignore
        return yaml.safe_load(Path(path).read_text())
    except ImportError:
        pass
    data = {"pages": []}
    cur_page = cur_heading = cur_row = None
    for raw in Path(path).read_text().splitlines():
        line = raw.split(" #")[0].rstrip() if not raw.lstrip().startswith("#") else ""
        if not line.strip():
            continue
        indent = len(line) - len(line.lstrip())
        text = line.strip()
        if text.startswith("- "):
            text = text[2:]
            key, _, value = text.partition(":")
            value = value.strip()
            if indent == 2 and key == "page":
                cur_page = {"page": value, "headings": []}
                data["pages"].append(cur_page)
                cur_heading = cur_row = None
                continue
            if indent == 6 and key == "heading":
                cur_heading = {"heading": value, "rows": []}
                cur_page["headings"].append(cur_heading)
                cur_row = None
                continue
            if indent == 10 and key == "id":
                cur_row = {"id": json.loads(value) if value.startswith('"') else value}
                cur_heading["rows"].append(cur_row)
                continue
        key, _, value = text.partition(":")
        value = value.strip()
        if value.startswith('"'):
            value = json.loads(value)
        elif value.startswith("["):
            value = [json.loads(v) if v.strip().startswith('"') else v.strip() for v in value[1:-1].split(",") if v.strip()]
        elif value in ("true", "false"):
            value = value == "true"
        target = cur_row if indent >= 12 else cur_heading if indent >= 8 else cur_page
        if target is not None and key not in ("headings", "rows"):
            target[key] = value
    return data


def c_lit(s):
    return json.dumps(s, ensure_ascii=False)


def bake(in_path, out_path, check):
    data = load_yaml(in_path)
    lines = ["// Generated by ps5/tools/settings-text.py from settings-text.yaml: do not edit; edit the YAML.",
             "// Each row the interface builds is looked up by id (bp_settings_text.cpp): its words, its page and",
             "// heading, and its place under the heading.",
             "static const SettingsText kSettingsText[] = {"]
    order = 0
    for page in data["pages"]:
        for heading in page.get("headings", []):
            for row in heading.get("rows", []):
                lines.append(f"\t{{{c_lit(row['id'])}, {c_lit(row.get('label', ''))}, {c_lit(row.get('desc', ''))}, "
                             f"{c_lit(page['page'])}, {c_lit(page.get('name', ''))}, {c_lit(heading['heading'])}, "
                             f"{c_lit(heading.get('label', ''))}, {order}}},")
                order += 1
    lines.append("};")
    text = "\n".join(lines) + "\n"
    if check:
        same = Path(out_path).exists() and Path(out_path).read_text() == text
        print("up to date" if same else "OUT OF DATE: run ps5/tools/settings-text.py bake")
        return 0 if same else 1
    Path(out_path).write_text(text)
    print(f"wrote {out_path}: {order} rows")
    return 0


def lint(in_path):
    data = load_yaml(in_path)
    ids = {}
    problems = 0
    for page in data["pages"]:
        for heading in page.get("headings", []):
            for row in heading.get("rows", []):
                rid = row.get("id", "")
                if rid in ids:
                    print(f"id twice: {rid} (also under {ids[rid]})")
                    problems += 1
                ids[rid] = f"{page['page']}/{heading['heading']}"
                if not row.get("label"):
                    print(f"no label: {rid}")
                    problems += 1
                desc = row.get("desc", "")
                if len(desc) > 120 and row.get("kind") != "info":
                    print(f"long desc ({len(desc)} chars, will scroll): {rid}")
    print(f"{len(ids)} rows, {problems} problem(s)")
    return 1 if problems else 0


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("extract")
    e.add_argument("dump")
    e.add_argument("--out", default=str(DEFAULT_YAML))
    b = sub.add_parser("bake")
    b.add_argument("--in", dest="inp", default=str(DEFAULT_YAML))
    b.add_argument("--out", default=str(DEFAULT_INC))
    b.add_argument("--check", action="store_true")
    l = sub.add_parser("lint")
    l.add_argument("--in", dest="inp", default=str(DEFAULT_YAML))
    a = ap.parse_args()
    if a.cmd == "extract":
        extract(a.dump, a.out)
        return 0
    if a.cmd == "bake":
        return bake(a.inp, a.out, a.check)
    return lint(a.inp)


if __name__ == "__main__":
    sys.exit(main())
