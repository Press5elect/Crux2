#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - the startup video (ps5/art/intro, README.md there): the picture that glows behind the glass,
# logo.png (1536 x 1024, on black): the mark large in the upper part, the name under it. Both are
# drawn by ps5/tools/make-title-art.py (crux_mark, crux_name), as the title's own pictures are.
# main.js reads where they are from the constants below (MARK_Y, MARK_SIZE, SPLIT).
#
#   python3 logo.py [out.png]          (numpy and Pillow)
import importlib.util
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[4]
spec = importlib.util.spec_from_file_location("title_art", ROOT / "ps5" / "tools" / "make-title-art.py")
art = importlib.util.module_from_spec(spec)
spec.loader.exec_module(art)

WIDTH, HEIGHT = 1536, 1024
MARK_Y, MARK_SIZE = 318, 560       # the mark's middle, from the top, and its square
NAME_TOP = 652                     # the name's box, from the top (main.js: SPLIT = 0.40, y = 614)


def main() -> None:
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).with_name("logo.png")
    canvas = Image.new("RGBA", (WIDTH, HEIGHT), (0, 0, 0, 255))
    mark = art.crux_mark(MARK_SIZE)
    canvas.alpha_composite(mark, (WIDTH // 2 - MARK_SIZE // 2, MARK_Y - MARK_SIZE // 2))
    name = art.crux_name(250)
    canvas.alpha_composite(name, (WIDTH // 2 - name.size[0] // 2, NAME_TOP))
    canvas.convert("RGB").save(out)
    print(f"{out}: mark {MARK_SIZE} at y {MARK_Y}, name {name.size[0]} x {name.size[1]} at y {NAME_TOP}")


if __name__ == "__main__":
    main()
