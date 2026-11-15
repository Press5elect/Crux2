#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - the startup video (ps5/art/intro, README.md there): the name's letters for main.js.
# letters.png holds C, R, U and 2 in Saira ExtraBold, each in a cell of its own, in the colours of
# ps5/tools/make-title-art.py; letters.json says where each one stands in the name, laid out as
# crux_name lays it out there: the mark is the X, in a square of the type's size, and main.js draws
# the mark itself (the same drawing as crux_mark: a square of 100, bars 84 long, 7 and 16 wide).
#
#   python3 logo.py [outdir]          (numpy and Pillow; CRUX2_ROOT if this is not in the tree)
import importlib.util
import json
import os
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(os.environ.get("CRUX2_ROOT") or Path(__file__).resolve().parents[4])
spec = importlib.util.spec_from_file_location("title_art", ROOT / "ps5" / "tools" / "make-title-art.py")
art = importlib.util.module_from_spec(spec)
spec.loader.exec_module(art)

TYPE = 384                         # the type's size in letters.png, in pixels
CELL_W, CELL_H = 384, 448          # each letter's cell


def main() -> None:
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).parent
    face = ImageFont.truetype(str(art.BRAND_FONT), TYPE)
    parts = ["C", "R", "U", None, "2"]
    # crux_name: each letter as wide as the face makes it and a hundredth of the type more; the mark
    # a square of the type that the letters beside it reach into by an eleventh of it
    widths = [TYPE * (1 - 2 * 20 / 220) if p is None else face.getlength(p) + 0.01 * TYPE for p in parts]
    total = sum(widths)
    # straight alpha with the colour everywhere in the cell, so that its mipmaps have no dark fringe
    mask = Image.new("L", (CELL_W * 4, CELL_H), 0)
    ink = Image.new("RGB", (CELL_W * 4, CELL_H), art.CREAM)
    draw = ImageDraw.Draw(mask)
    layout = {"total": total / TYPE, "letters": []}
    x, cell = 0.0, 0
    for part, w in zip(parts, widths):
        if part is None:
            layout["mark"] = (x + w / 2 - total / 2) / TYPE          # from the name's middle, in types
        else:
            advance = face.getlength(part)
            # the capitals' middle (0.344 of the type above the line) on the cell's middle
            draw.text((cell * CELL_W + CELL_W / 2 - advance / 2, CELL_H / 2 + 0.344 * TYPE), part, font=face, fill=255, anchor="ls")
            if part == "2":
                ink.paste(art.ORANGE, (cell * CELL_W, 0, (cell + 1) * CELL_W, CELL_H))
            layout["letters"].append({"ch": part, "x": (x + advance / 2 - total / 2) / TYPE, "cell": cell,
                                      "w": CELL_W / TYPE, "h": CELL_H / TYPE, "orange": part == "2"})
            cell += 1
        x += w
    sheet = ink.convert("RGBA")
    sheet.putalpha(mask)
    sheet.save(out / "letters.png")
    (out / "letters.json").write_text(json.dumps(layout, indent=1) + "\n")
    print(json.dumps(layout))


if __name__ == "__main__":
    main()
