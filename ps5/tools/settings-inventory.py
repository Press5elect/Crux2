#!/usr/bin/env python3
# Crux2 - the settings inventory: every option the interface has, and the test that none is lost.
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
#
#   ps5/tools/settings-inventory.py check [--dump FILE] [--work DIR]
#       Makes the inventory again from the code (ps5/tests/bigpicture/inventory: the real pages,
#       bp_options.cpp's and bp_pcsx2.cpp's, built for the PC) and compares it with the baseline,
#       ps5/frontend/bigpicture/settings-inventory.json. Fails when a setting (section and key), a
#       controller or hotkey binding, an action, a library layout or a skin of the baseline is gone,
#       is no longer offered for one game where it was, or when its kind, default, choices (their
#       values), range or combined switches changed. New rows, and changed words, levels and flags,
#       are listed and do not fail. --dump: an inventory dump already made (build-inventory.sh).
#
#   ps5/tools/settings-inventory.py bless [--dump FILE] [--work DIR]
#       The same comparison, printed; then the baseline is written from the code as it is now. For a
#       change that is meant to remove or change an option: the diff it prints goes in the commit.
#
#   ps5/tools/settings-inventory.py show [--dump FILE] [--work DIR]
#       Counts: rows by page, by kind and by level; layouts; skins.
#
# The baseline holds one row per place a row is shown (the settings of all games and a game's own
# share a row when it is the same one), each with its id (settings-text.yaml's), page, heading,
# kind, section and key, default, choices, range, flags, the layers it is on ("shared" all games',
# "game" one game's) and the states of the console it was seen in ("new": nothing set up, one USB
# drive in; "set-up": signed in, a game folder and a share, a share being added, USB controllers on).
import argparse
import collections
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
BASELINE = ROOT / "ps5" / "frontend" / "bigpicture" / "settings-inventory.json"
BUILD = ROOT / "ps5" / "tests" / "bigpicture" / "inventory" / "build-inventory.sh"

SETTING_KINDS = {"toggle", "intlist", "floatlist", "stringlist", "intrange", "floatrange", "combo", "bind"}
# What the no-loss test compares of a setting (the words are settings-text.yaml's, and may change).
VALUE_FIELDS = ("kind", "default", "range", "parts")


def slug(text):
    return re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")[:56] or "row"


def base_id(row):
    if row["kind"] == "heading":
        return "heading:" + slug(row["label"])
    if row["kind"] in SETTING_KINDS and row.get("key"):
        return f"{row['section']}.{row['key']}"
    # (An action's or a line's id is made from the label the code gives it, so that it stays when the
    # words shown, settings-text.yaml's, change.)
    return f"{row['kind']}:{slug(row.get('code_label', row['label']))}"


def place(row):
    return (row["page"], row["heading"])


def make(dump):
    """The dump (every row of every page, in each state and layer) as the inventory: one row per
    place, with an id that is one row's."""
    seen = {}  # (base id, page, heading, label) -> its occurrences
    firsts = {}
    # Order: as the pages show them, the shared pages' first; a row only another state or a game's
    # pages have goes after the row it follows there.
    order = []
    sequence = None
    prev = None
    for row in dump["rows"]:
        k = (base_id(row), row["page"], row["heading"], row["label"])
        if (row["state"], row["layer"]) != sequence:
            sequence, prev = (row["state"], row["layer"]), None
        if k not in seen:
            seen[k] = []
            firsts[k] = row
            if prev is None:
                # The first row of a sequence not seen yet: after the last row of its page, or at the end.
                at = max((i + 1 for i, o in enumerate(order) if o[1] == row["page"]), default=len(order))
            else:
                at = order.index(prev) + 1
            order.insert(at, k)
        seen[k].append(row)
        prev = k
    order = [(k, firsts[k]) for k in order]
    # Ids: the base id, and where one is in more than one place, its page and heading after an "@".
    places = collections.defaultdict(set)
    for (bid, page, heading, label), _ in order:
        places[bid].add((page, heading, label))
    rows = []
    taken = set()
    for k, first in order:
        bid, page, heading, label = k
        rid = bid
        if len(places[bid]) > 1:
            here = [1 for p, _, _ in places[bid] if p == page]
            rid = f"{bid}@{slug(page)}" if len(here) == 1 else f"{bid}@{slug(page)}/{slug(heading) or 'top'}"
        n = 2
        unique = rid
        while unique in taken:
            unique = f"{rid}#{n}"
            n += 1
        taken.add(unique)
        occurrences = seen[k]
        out = {"id": unique}
        for field in ("page", "heading", "kind", "label", "code_label", "desc", "section", "key", "default", "choices", "range", "parts", "bind_type",
                      "flags", "conditional", "game_default", "shows_value"):
            if field in first:
                out[field] = first[field]
        out["layers"] = sorted({o["layer"] for o in occurrences}, reverse=True)
        out["states"] = sorted({o["state"] for o in occurrences})
        # A row a game's page makes otherwise than the shared page (another default, other choices).
        differs = {}
        for o in occurrences:
            for field in ("default", "choices", "flags", "desc"):
                if o.get(field) != first.get(field):
                    differs.setdefault(o["layer"] + "/" + o["state"], {})[field] = o.get(field)
        if differs:
            out["variants"] = differs
        rows.append(out)
    return {"rows": rows, "layouts": dump["layouts"], "skins": dump["skins"]}


def write(inventory, path):
    lines = ["{", '  "about": "Every settings row the interface can show, made by ps5/tools/settings-inventory.py from the code '
             '(ps5/tests/bigpicture/inventory). The no-loss test compares the code with this file; bless writes it.",', '  "rows": [']
    lines.append(",\n".join("    " + json.dumps(r, ensure_ascii=False) for r in inventory["rows"]))
    lines.append("  ],")
    lines.append('  "layouts": [')
    lines.append(",\n".join("    " + json.dumps(r, ensure_ascii=False) for r in inventory["layouts"]))
    lines.append("  ],")
    lines.append('  "skins": [')
    lines.append(",\n".join("    " + json.dumps(r, ensure_ascii=False) for r in inventory["skins"]))
    lines.append("  ]")
    lines.append("}")
    Path(path).write_text("\n".join(lines) + "\n")


def dump_from(args):
    if args.dump:
        return json.loads(Path(args.dump).read_text())
    work = Path(args.work) if args.work else Path(tempfile.mkdtemp(prefix="crux2-inventory-"))
    subprocess.run(["bash", str(BUILD), str(work)], check=True)
    return json.loads((work / "dump.json").read_text())


def level(row):
    flags = row.get("flags", [])
    return "Expert" if "Expert" in flags else "Advanced" if "Advanced" in flags else "Normal"


# ------------------------------------------------------------------------------- the comparison

def settings_of(inventory):
    """Every setting by what it is (section, key, kind of thing): its rows."""
    out = collections.defaultdict(list)
    for r in inventory["rows"]:
        if r["kind"] in SETTING_KINDS and r.get("key"):
            out[(r["section"], r["key"])].append(r)
    return out


def actions_of(inventory):
    return {r["id"]: r for r in inventory["rows"] if r["kind"] == "action"}


def values(row):
    return [c["value"] for c in row.get("choices", [])]


def compare(old, new):
    """(losses, notes): what fails the test, and what is only said."""
    losses, notes = [], []
    olds, news = settings_of(old), settings_of(new)
    for key, rows in olds.items():
        name = f"{key[0]}.{key[1]} ({rows[0]['label']}, {rows[0]['page']})"
        if key not in news:
            losses.append(f"setting gone: {name}")
            continue
        now = news[key]
        layers_then = {l for r in rows for l in r["layers"]}
        layers_now = {l for r in now for l in r["layers"]}
        for layer in sorted(layers_then - layers_now):
            losses.append(f"setting no longer offered {'for one game' if layer == 'game' else 'for all games'}: {name}")
        # Each row it had (a setting shown twice keeps both kinds of row): the same values somewhere.
        for r in rows:
            match = [n for n in now if all(n.get(f) == r.get(f) for f in VALUE_FIELDS) and sorted(map(json.dumps, values(n))) ==
                     sorted(map(json.dumps, values(r)))]
            if match:
                same = [n for n in match if n["id"] == r["id"]]
                n = (same or match)[0]
                if [c["name"] for c in n.get("choices", [])] != [c["name"] for c in r.get("choices", [])]:
                    notes.append(f"choices' names or order changed: {name}")
                if level(n) != level(r):
                    notes.append(f"level {level(r)} -> {level(n)}: {name}")
                if (n["page"], n["heading"]) != (r["page"], r["heading"]):
                    notes.append(f"moved from {r['page']} / {r['heading']} to {n['page']} / {n['heading']}: {name}")
                continue
            n = now[0]
            for f in VALUE_FIELDS:
                if n.get(f) != r.get(f):
                    losses.append(f"{f} changed: {name}: {json.dumps(r.get(f))} -> {json.dumps(n.get(f))}")
            gone = [v for v in values(r) if v not in values(n)]
            added = [v for v in values(n) if v not in values(r)]
            if gone:
                losses.append(f"choices gone: {name}: {json.dumps(gone)}")
            if added:
                losses.append(f"choices added: {name}: {json.dumps(added)}")
    for key in news.keys() - olds.keys():
        notes.append(f"new setting: {key[0]}.{key[1]} ({news[key][0]['label']}, {news[key][0]['page']})")
    olda, newa = actions_of(old), actions_of(new)
    for rid, r in olda.items():
        if rid not in newa:
            losses.append(f"action gone: {rid} ({r['label']}, {r['page']})")
        elif {l for l in r["layers"]} - set(newa[rid]["layers"]):
            losses.append(f"action no longer offered on every layer it was: {rid}")
    for rid in newa.keys() - olda.keys():
        notes.append(f"new action: {rid} ({newa[rid]['label']}, {newa[rid]['page']})")
    for what in ("layouts", "skins"):
        now = {x["id"]: x for x in new[what]}
        for x in old[what]:
            if x["id"] not in now:
                losses.append(f"{what[:-1]} gone: {x['id']} {x['name']}")
            elif now[x["id"]] != x:
                # The number is what [UI] View or [PS5UI] Look keeps: another one there is another look.
                losses.append(f"{what[:-1]} {x['id']} is {json.dumps(now[x['id']])}, was {json.dumps(x)}")
        for i in now.keys() - {x["id"] for x in old[what]}:
            notes.append(f"new {what[:-1]}: {i} {now[i]['name']}")
    return losses, notes


def report(losses, notes):
    for n in notes:
        print("  note: " + n)
    for l in losses:
        print("  LOST: " + l)


def show(inventory):
    rows = inventory["rows"]
    print(f"{len(rows)} rows ({sum(1 for r in rows if r['kind'] != 'heading')} without the headings), "
          f"{len(inventory['layouts'])} layouts, {len(inventory['skins'])} skins")
    by_page = collections.Counter((r["page"], "+".join(r["layers"])) for r in rows if r["kind"] != "heading")
    for (page, layers), n in by_page.items():
        print(f"  {page:20} {layers:12} {n}")
    print("  by kind: " + ", ".join(f"{k} {n}" for k, n in collections.Counter(r["kind"] for r in rows).most_common()))
    print("  by level: " + ", ".join(f"{k} {n}" for k, n in collections.Counter(level(r) for r in rows if r["kind"] != "heading").most_common()))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("command", choices=["check", "bless", "show"])
    ap.add_argument("--dump", help="an inventory dump made already (build-inventory.sh's dump.json)")
    ap.add_argument("--work", help="where to build the inventory (default: a new temporary folder)")
    ap.add_argument("--baseline", default=str(BASELINE))
    args = ap.parse_args()
    new = make(dump_from(args))
    if args.command == "show":
        show(new)
        return 0
    baseline = Path(args.baseline)
    old = json.loads(baseline.read_text()) if baseline.exists() else {"rows": [], "layouts": [], "skins": []}
    losses, notes = compare(old, new)
    report(losses, notes)
    if args.command == "bless":
        write(new, baseline)
        print(f"settings inventory: {baseline.relative_to(ROOT) if baseline.is_relative_to(ROOT) else baseline} written: "
              f"{len(new['rows'])} rows ({len(losses)} lost or changed, {len(notes)} notes above)")
        return 0
    if losses:
        print(f"settings inventory: FAILED: {len(losses)} option(s) lost or changed against {baseline.name}. If that is meant, "
              "bless it: ps5/tools/settings-inventory.py bless (and say why in the commit).")
        return 1
    print(f"settings inventory: nothing lost ({len(new['rows'])} rows, {len(notes)} note(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())
