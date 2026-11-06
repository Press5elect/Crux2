#!/usr/bin/env python3
# Crux2 - the interface's Japanese, Chinese and Korean letters: a subset of Noto Sans CJK.
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
#
#   python3 ps5/tools/make-cjk-font.py NotoSansCJK-Regular.ttc \
#       ps5/package/resources/fonts/NotoSansCJK-Subset.otf
#
# The interface's fonts (Roboto, the icons, the emoji) have no letters for the games whose names
# are written in kana, kanji or hangul, so those names showed as "?". Noto Sans CJK (SIL OFL 1.1,
# which allows a subset to be bundled under the same licence) is whole 19 MB; this keeps the Japanese
# face's letters for the characters Shift-JIS (cp932), GB 2312, Big5 and KS X 1001 can write,
# plus kana, the CJK and full-width punctuation and the Hangul syllables of KS X 1001:
# enough for the names of the games of every region, a few MB. Latin is left to Roboto.
# The source is the fonts-noto-cjk package (Debian/Ubuntu) or the Noto CJK release at
# https://github.com/notofonts/noto-cjk. Needs fontTools (pip install fonttools).

import sys

from fontTools import subset
from fontTools.ttLib import TTFont


def codepoints():
    cps = set()
    for codec in ("cp932", "gb2312", "big5", "euc_kr"):
        # every two-byte code the codec decodes
        for hi in range(0x81, 0xFF):
            for lo in range(0x40, 0xFF):
                try:
                    s = bytes((hi, lo)).decode(codec)
                except UnicodeDecodeError:
                    continue
                if len(s) == 1:
                    cps.add(ord(s))
    for lo, hi in ((0x2010, 0x203B), (0x2190, 0x21FF), (0x2460, 0x24FF), (0x25A0, 0x25EF),
                   (0x2600, 0x266F), (0x3000, 0x30FF), (0x3200, 0x32FF), (0x31F0, 0x31FF),
                   (0xFF01, 0xFF9F)):
        cps.update(range(lo, hi + 1))
    # the Latin letters stay with Roboto
    return sorted(c for c in cps if c >= 0x2010)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    # Noto Sans CJK JP is the first face of the collection
    font = TTFont(src, fontNumber=0, lazy=False)
    options = subset.Options()
    options.layout_features = ["vert", "vrt2", "locl", "pwid", "fwid"]
    options.name_IDs = [0, 1, 2, 3, 4, 5, 6, 13, 14]
    options.notdef_outline = True
    options.hinting = False
    options.desubroutinize = True
    sub = subset.Subsetter(options)
    sub.populate(unicodes=codepoints())
    sub.subset(font)
    # The 16-bit cmap table (format 4) overflows with this many scattered letters: a full-range
    # one (format 12), which FreeType reads as well, takes its place.
    from fontTools.ttLib.tables._c_m_a_p import CmapSubtable

    cmap = font["cmap"]
    table = CmapSubtable.newSubtable(12)
    table.platformID, table.platEncID, table.language = 3, 10, 0
    table.cmap = dict(font.getBestCmap())
    cmap.tables = [table]
    font.flavor = None
    font.save(dst)
    print(f"wrote {dst}")


if __name__ == "__main__":
    main()
