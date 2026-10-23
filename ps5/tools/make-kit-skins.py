#!/usr/bin/env python3
# Crux2 - makes bigpicture/bp_kit_skins.inc: the thirty themes of the ps5-homebrew-ui kit as Crux2 skins.
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0+
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
#
#   ps5/tools/make-kit-skins.py <kit folder> [--out FILE] [--commit HASH] [--sheet PNG] [--check]
#
# The kit (https://github.com/blackbearreloaded/ps5-homebrew-ui, by BlackBearReloaded,
# GPL-3.0-or-later) describes each theme as data: src/ui/theme.cpp fills in a struct of design
# tokens (page, surface, text, primary, ...), mostly starting from one of two web-like bases and
# changing some fields. This script reads that file as the kit writes it (a function per theme,
# assignments to its fields, the gallery order in themes()), works out each theme's tokens, and
# maps them onto the fields of Crux2's Skin (bp_internal.h): the colours its screens are drawn in.
# Crux2 draws with Dear ImGui, not with the kit's renderer, so only colours, corner roundness, the
# light or dark page and the backdrop's kind are taken; shadows, bevels, glass and the kit's fonts
# are not.
#
# The colours are checked for contrast as WCAG measures it (text on panels and on the page 4.5:1,
# secondary text on panels 3:1, text on the accent 4.5:1). Where a theme falls short as Crux2
# draws it, the Crux2 colour (never the kit's own file) is moved, by the least it takes, toward
# black or white; each move is written into the .inc's comments and printed.
#
# The output depends on nothing but the kit's file and the commit named: run it again when the kit
# changes, and the .inc changes only where the themes did.
#
# --sheet draws a contact sheet of the skins (needs Pillow) for a person to look at; --check exits
# non-zero when the .inc on disk is not what this would write.

import argparse
import codecs
import os
import re
import subprocess
import sys

# ------------------------------------------------------------------------------ reading the kit


class Color:
    """A colour as the kit holds it: r, g, b, a from 0 to 1."""

    def __init__(self, r, g, b, a=1.0):
        self.r, self.g, self.b, self.a = float(r), float(g), float(b), float(a)

    @staticmethod
    def hex(value, alpha=1.0):
        return Color(((value >> 16) & 0xFF) / 255.0, ((value >> 8) & 0xFF) / 255.0, (value & 0xFF) / 255.0, alpha)

    def rgb255(self):
        return tuple(int(round(max(0.0, min(1.0, c)) * 255)) for c in (self.r, self.g, self.b))

    def __eq__(self, other):
        return isinstance(other, Color) and self.rgb255() == other.rgb255() and round(self.a * 255) == round(other.a * 255)

    def __repr__(self):
        r, g, b = self.rgb255()
        return "#%02x%02x%02x%s" % (r, g, b, "" if self.a >= 1 else "@%.2f" % self.a)


def split_arguments(text):
    """Splits a call's arguments at the commas that are not inside brackets or strings."""
    parts, depth, current, quoted, i = [], 0, "", False, 0
    while i < len(text):
        c = text[i]
        if quoted:
            current += c
            if c == "\\":
                current += text[i + 1]
                i += 1
            elif c == '"':
                quoted = False
        elif c == '"':
            quoted = True
            current += c
        elif c in "([{":
            depth += 1
            current += c
        elif c in ")]}":
            depth -= 1
            current += c
        elif c == "," and depth == 0:
            parts.append(current.strip())
            current = ""
        else:
            current += c
        i += 1
    if current.strip():
        parts.append(current.strip())
    return parts


def split_statements(text):
    """Splits a function's body at the semicolons that are not inside a string."""
    statements, current, quoted, i = [], "", False, 0
    while i < len(text):
        c = text[i]
        if quoted and c == "\\":
            current += text[i : i + 2]
            i += 2
            continue
        if c == '"':
            quoted = not quoted
        if c == ";" and not quoted:
            statements.append(current)
            current = ""
        else:
            current += c
        i += 1
    statements.append(current)
    return statements


def number(text):
    text = text.strip()
    if text.startswith("0x"):
        return int(text, 16)
    return float(text.rstrip("f"))


class Kit:
    def __init__(self, folder):
        self.folder = folder
        with open(os.path.join(folder, "src/ui/theme.hpp"), encoding="utf-8") as f:
            self.header = f.read()
        with open(os.path.join(folder, "src/ui/theme.cpp"), encoding="utf-8") as f:
            self.source = f.read()
        self.defaults = self.read_defaults()
        self.functions = self.read_functions()
        self.order = self.read_order()

    def read_defaults(self):
        """The default values of struct Theme's fields (theme.hpp)."""
        body = re.search(r"struct Theme\s*\{(.*?)\n\};", self.header, re.S).group(1)
        defaults = {}
        for line in body.splitlines():
            line = line.split("//")[0].strip()
            m = re.match(r"[\w:]+\s+(\w+)\s*(?:=\s*(.+?)|\{(.*?)\})\s*;$", line)
            if not m:
                continue
            name, value, braces = m.group(1), m.group(2), m.group(3)
            if braces is not None:
                values = [number(v) for v in split_arguments(braces)]
                defaults[name] = Color(*values) if len(values) == 4 else None
            else:
                defaults[name] = self.evaluate(value.replace("gfx::Color::rgb", "rgb"), {})
        return defaults

    def read_functions(self):
        """Every `Theme name(...) { ... }` of theme.cpp: its parameters and its statements."""
        functions = {}
        for m in re.finditer(r"^Theme (\w+)\(([^)]*)\)\s*\n\{\n(.*?)^\}", self.source, re.S | re.M):
            name, parameters, body = m.group(1), m.group(2), m.group(3)
            names = [p.split()[-1].lstrip("*") for p in parameters.split(",") if p.strip()]
            statements = [s.strip() for s in split_statements(re.sub(r"//[^\n]*", "", body)) if s.strip()]
            functions[name] = (names, statements)
        return functions

    def read_order(self):
        body = re.search(r"kThemes = \{(.*?)\};", self.source, re.S).group(1)
        body = re.sub(r"//[^\n]*", "", body)
        return [f.strip()[:-2] for f in body.split(",") if f.strip().endswith("()")]

    def evaluate(self, expression, t, arguments=None):
        """One right-hand side of theme.cpp."""
        e = " ".join(expression.split())
        arguments = arguments or {}
        if e in arguments:
            return arguments[e]
        if e.startswith('"'):
            return e  # a C string literal, kept as written
        if e in ("true", "false"):
            return e == "true"
        if e.startswith("t."):
            return t[e[2:]]
        if "::" in e and "(" not in e:
            return e.split("::")[-1]  # SurfaceStyle::flat -> "flat"
        m = re.match(r"(\w+)\((.*)\)$", e)
        if m:
            function, args = m.group(1), split_arguments(m.group(2))
            if function == "rgb":
                return Color.hex(int(number(args[0])), number(args[1]) if len(args) > 1 else 1.0)
            if function == "plain":
                c = Color.hex(int(number(args[0])))
                return {"mode": "gradient", "colors": [c, c, Color(0, 0, 0), Color(0, 0, 0)], "params": [0, 0, 0]}
            if function == "backdrop":
                colours = [Color.hex(int(number(a))) for a in args[1:5]] + [Color(0, 0, 0)] * (4 - len(args[1:5]))
                params = [number(a) for a in args[5:8]] + [0.0] * (3 - len(args[5:8]))
                return {"mode": args[0].split("::")[-1], "colors": colours[:4], "params": params}
            if function in self.functions:
                return self.theme(function, [self.evaluate(a, t, arguments) for a in args])
        try:
            return number(e)
        except ValueError:
            raise SystemExit("make-kit-skins: cannot read `%s` in theme.cpp" % e)

    def theme(self, function, arguments=()):
        """Runs one theme function of theme.cpp: its tokens."""
        parameters, statements = self.functions[function]
        given = dict(zip(parameters, arguments))
        t = None
        for statement in statements:
            s = " ".join(statement.split())
            if s.startswith("Theme t{}"):
                t = dict(self.defaults)
            elif s.startswith("Theme t ="):
                t = dict(self.evaluate(s[len("Theme t =") :], {}, given))
            elif s.startswith("Theme t"):
                m = re.match(r"Theme t\s*=?\s*(.*)", s)
                t = dict(self.evaluate(m.group(1), {}, given))
            elif s.startswith("t."):
                field, value = s[2:].split("=", 1)
                t[field.strip()] = self.evaluate(value, t, given)
            elif s.startswith("return t"):
                return t
        raise SystemExit("make-kit-skins: %s() does not return its theme" % function)

    def themes(self):
        return [self.theme(f) for f in self.order]


# ------------------------------------------------------------------------------ colour arithmetic


def mix(a, b, t):
    return Color(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t)


def over(c, base):
    """c drawn over an opaque base: the opaque colour that is seen."""
    return Color(c.r * c.a + base.r * (1 - c.a), c.g * c.a + base.g * (1 - c.a), c.b * c.a + base.b * (1 - c.a), 1.0)


def luminance(c):
    def channel(v):
        v = max(0.0, min(1.0, v))
        return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4

    # (On 8-bit values, as the .inc will hold them.)
    r, g, b = (v / 255.0 for v in c.rgb255())
    return 0.2126 * channel(r) + 0.7152 * channel(g) + 0.0722 * channel(b)


def contrast(a, b):
    la, lb = luminance(a), luminance(b)
    return (max(la, lb) + 0.05) / (min(la, lb) + 0.05)


def distance(a, b):
    return sum((x - y) ** 2 for x, y in zip(a.rgb255(), b.rgb255())) ** 0.5


BLACK, WHITE = Color(0, 0, 0), Color(1, 1, 1)

# ------------------------------------------------------------------------------ the mapping

# The kit's backdrop modes and the nearest of Crux2's backdrops (bp_draw.cpp drawBackdrop).
BACKDROPS = {
    "none": "BackStill",
    "gradient": "BackStill",  # a plain page, or two colours and a highlight: the still wash
    "aurora": "BackAurora",  # colour clouds
    "grid": "BackHorizon",  # a synthwave floor and its sun
    "stars": "BackSparks",
    "waves": "BackWaves",
    "bokeh": "BackSparks",  # drifting soft discs: Crux2's rising motes
    "phosphor": "BackStill",
    "paper": "BackStill",  # paper grain: a still page
    "dots": "BackMatrix",  # a dot matrix with a travelling pulse
    "vista": "BackHorizon",
}

# Which of the kit's colours is a theme's accent in Crux2 (the focus, selections, the backdrop's
# light, filled buttons). The kit's `primary` is its main action and has its own text colour;
# where a theme is known by another colour, that one is taken instead.
ACCENT_CHOICE = {
    # Material after Propeller: "a pink accent" is what it is recognised by (its primary is the
    # same blue as half the other web themes).
    "paper": ("accent", "the pink accent the theme is known by; its primary is a common blue"),
    # Sketch after PaperCSS: its primary is a pale blue fill (#deefff) that would not show as a
    # focus on a cream page; the pen blue it writes on that fill, its focus colour, is taken.
    "sketch": ("focus", "the pen blue of its focus ring; its primary is a pale fill that does not show on the page"),
}


def skin_of(theme):
    """Crux2's Skin fields for one kit theme, before the contrast checks."""
    page = Color(theme["page"].r, theme["page"].g, theme["page"].b)
    dark = bool(theme["dark"])
    backdrop = theme["backdrop"]
    c0, c1 = backdrop["colors"][0], backdrop["colors"][1]
    notes = []
    s = {}

    # The page: the backdrop's two base colours when it has two; else a slight fall of the page's
    # own (lighter toward the bottom on a dark page, darker on a light one).
    if c0 != c1:
        s["bgTop"], s["bgBottom"] = Color(c0.r, c0.g, c0.b), Color(c1.r, c1.g, c1.b)
    else:
        s["bgTop"] = page
        s["bgBottom"] = mix(page, WHITE, 0.04) if dark else mix(page, BLACK, 0.035)
    bar = mix(page, BLACK, 0.30 if dark else 0.06)
    s["bar"] = Color(bar.r, bar.g, bar.b, 235 / 255.0)

    panel = over(theme["surface"], page)
    s["panel"] = panel
    s["panelHi"] = over(theme["surface_high"], panel)
    s["sheetHi"] = s["panelHi"]

    text = theme["page_text"] if theme["page_text"] is not None and theme["page_text"].a > 0 else theme["text"]
    s["text"] = over(text, panel)
    s["dim"] = over(theme["text_muted"], panel)

    which, why = ACCENT_CHOICE.get(theme["id"].strip('"'), ("primary", ""))
    accent = over(theme[which], page)
    s["accent"] = accent
    s["accent_kit"] = accent
    on = over(theme["on_primary"], accent)
    if which != "primary" and contrast(on, accent) < 4.5:
        on = WHITE if contrast(WHITE, accent) >= contrast(BLACK, accent) else BLACK
    s["onAccent"] = on
    s["warm"] = over(theme["warning"], page)
    s["good"] = over(theme["success"], page)
    s["bad"] = over(theme["danger"], page)

    outline = theme["outline"]
    if outline.a > 0:
        s["line"] = Color(outline.r, outline.g, outline.b, min(round(outline.a * 255), 40) / 255.0)
    else:
        s["line"] = Color(s["text"].r, s["text"].g, s["text"].b, (22 if dark else 28) / 255.0)

    corner = theme["corner"]
    s["round"] = 0.0 if corner == "pixel" else max(0.0, min(2.0, float(theme["radius_card"]) / 20.0))
    s["light"] = not dark
    s["backdrop"] = BACKDROPS.get(backdrop["mode"], "BackStill")
    s["accent_from"] = which
    s["accent_why"] = why
    s["page_text_set"] = theme["page_text"] is not None and theme["page_text"].a > 0
    s["kit_text"] = over(theme["text"], panel)
    s["kit_muted"] = theme["text_muted"]
    return s


def derive(s):
    """The fields made of others: worked out again after any of those moved."""
    s["card"] = mix(s["panel"], s["panelHi"], 0.5)
    s["sheet"] = mix(s["bgTop"], s["panel"], 0.5)
    s["faint"] = mix(s["dim"], s["bgTop"], 0.4)


# The pairs that are checked: (foreground, background, the least ratio).
CHECKS = [("text", "panel", 4.5), ("dim", "panel", 3.0), ("onAccent", "accent", 4.5), ("text", "bgTop", 4.5)]
# The fields that may be moved to meet them (the page moves with its lower colour).
MOVABLE = ["text", "dim", "onAccent", "accent", "panel", "bgTop"]


def failing(s):
    return [(a, b, need) for a, b, need in CHECKS if contrast(s[a], s[b]) < need - 1e-9]


def toward(colour, target, t):
    return mix(colour, target, t)


# The accent is a theme's identity: it is darkened or lightened by no more than this to fit the
# text on it. Beyond that its text is changed instead, to a deep shade (or a pale tint) of the accent
# itself, as many of the kit's own frameworks write on their brighter colours.
ACCENT_MOST = 0.15


def best_move(s, a, b, need, fields=None):
    """The least move of one field, toward black or white, that meets this check and breaks no
    other that held: (distance, field, new colour, target, t) or None."""
    held = [c for c in CHECKS if contrast(s[c[0]], s[c[1]]) >= c[2] - 1e-9]
    if (a, b) == ("onAccent", "accent") and fields is None:
        move = best_move(s, a, b, need, ["accent"])
        if move is not None and move[4] <= ACCENT_MOST:
            return move
        # The text on the accent: a shade of the accent, the least deep that reads.
        shades = []
        for target in (BLACK, WHITE):
            for step in range(1, 101):
                t = step / 100.0
                on = toward(s["accent"], target, t)
                if contrast(on, s["accent"]) >= need:
                    shades.append((distance(s["onAccent"], on), "onAccent", on, target, t, True))
                    break
        if shades:
            shade = min(shades, key=lambda m: m[4])
            return shade
        return move
    best = None
    for field in fields or (a, b):
        if field not in MOVABLE:
            continue
        for target in (BLACK, WHITE):
            for step in range(1, 101):
                t = step / 100.0
                moved = toward(s[field], target, t)
                trial = dict(s)
                trial[field] = moved
                if field == "bgTop":
                    trial["bgBottom"] = toward(s["bgBottom"], target, t)
                if contrast(trial[a], trial[b]) < need:
                    continue
                if any(contrast(trial[x], trial[y]) < n - 1e-9 for x, y, n in held):
                    break
                d = distance(s[field], moved)
                if best is None or d < best[0]:
                    best = (d, field, moved, target, t)
                break
    return best


def fix_contrast(s):
    """Moves Crux2's colours until every check holds. Returns the list of moves, and the total."""
    moves, total = [], 0.0
    for _ in range(12):
        failed = failing(s)
        if not failed:
            break
        a, b, need = failed[0]
        before = contrast(s[a], s[b])
        best = best_move(s, a, b, need)
        if best is None:
            moves.append("%s on %s stays at %.2f:1 (nothing one field could do)" % (a, b, before))
            break
        d, field, moved, target, t = best[:5]
        shade = len(best) > 5
        old = s[field]
        s[field] = moved
        if field == "bgTop":
            s["bgBottom"] = toward(s["bgBottom"], target, t)
        total += d
        if shade:
            how = "a %s of the accent, %d%% toward %s: the accent itself would have had to move more than %d%%" % (
                "shade" if target is BLACK else "tint", round(t * 100), "black" if target is BLACK else "white", round(ACCENT_MOST * 100))
        else:
            how = "%d%% toward %s" % (round(t * 100), "black" if target is BLACK else "white")
        moves.append("%s on %s was %.2f:1 (< %.1f): %s %s -> %s (%s), now %.2f:1" % (a, b, before, need, field, old, moved, how, contrast(s[a], s[b])))
    return moves, total


def make_skin(theme):
    s = skin_of(theme)
    moves, total = fix_contrast(s)
    # A page with text of its own (Classic: white on teal, black on grey panels): Crux2 has one text
    # colour for both, so the kit's panel text is tried too, and the one that needs less is kept.
    if s["page_text_set"]:
        other = skin_of(theme)
        other["text"] = other["kit_text"]
        other_moves, other_total = fix_contrast(other)
        if not failing(other) and (failing(s) or other_total < total):
            other_moves.insert(0, "one text colour for page and panels: the kit's panel text %s, not its page text" % other["text"])
            s, moves = other, other_moves
        else:
            moves.insert(0, "one text colour for page and panels: the kit's page text %s" % s["text"])
    # (dim moved: faint follows it.)
    derive(s)
    return s, moves


# ------------------------------------------------------------------------------ writing the .inc


def rgba(c, alpha=None):
    r, g, b = c.rgb255()
    a = round((c.a if alpha is None else alpha) * 255)
    return "col::rgba(%d, %d, %d)" % (r, g, b) if a >= 255 else "col::rgba(%d, %d, %d, %d)" % (r, g, b, a)


def c_string(literal):
    return literal  # theme.cpp's own literal, escapes and all


def decoded(literal):
    return codecs.decode(literal.strip('"'), "unicode_escape").encode("latin-1").decode("utf-8")


FIELDS = ["bgTop", "bgBottom", "bar", "panel", "panelHi", "card", "sheet", "sheetHi", "dim", "faint", "accent", "warm"]


def write_inc(kit, commit, skins):
    out = []
    out.append("// Crux2 - the thirty themes of the ps5-homebrew-ui kit as skins: one initializer of struct Skin each.")
    out.append("// SPDX-License-Identifier: GPL-3.0+")
    out.append("//")
    out.append("// Generated by ps5/tools/make-kit-skins.py from ps5-homebrew-ui (src/ui/theme.cpp, revision")
    out.append("// %s): do not edit; run the script again when the kit changes. The themes are the work of" % commit)
    out.append("// BlackBearReloaded (https://github.com/blackbearreloaded/ps5-homebrew-ui), Copyright (C) 2026,")
    out.append("// licensed GPL-3.0-or-later; their colours are taken here, mapped onto Crux2's skin fields, in the")
    out.append("// kit's gallery order. Crux2 draws them with its own interface, not with the kit's renderer: the")
    out.append("// kit's shadows, bevels, glass and fonts are not part of a skin.")
    out.append("//")
    out.append("// How a theme's tokens become a skin: bgTop/bgBottom the backdrop's two base colours, or the page")
    out.append("// with a slight fall; bar the page darkened, at 235; panel the surface over the page; panelHi and")
    out.append("// sheetHi surface_high over the panel; card halfway between them; sheet the page halfway to the")
    out.append("// panel; text the page's text if the theme has one, else its text; dim text_muted over the panel;")
    out.append("// faint dim 40 % toward the page; accent the primary (or the colour noted); onAccent on_primary;")
    out.append("// warm, good and bad the warning, success and danger colours; round the panels' radius / 20")
    out.append("// (0 for pixel corners); line the outline at no more than 40 of 255, or the text at 22 (dark) or")
    out.append("// 28 (light) when the theme draws no outline. Contrast (WCAG): text on panel and on the page")
    out.append("// 4.5:1, dim on panel 3:1, onAccent on accent 4.5:1; a colour that fell short was moved by the")
    out.append("// least it took, as each theme's comment says.")
    out.append("")
    for function, theme, s, moves in skins:
        name = theme["name"]
        out.append("// %s (%s): %s." % (decoded(name), theme["id"].strip('"'), decoded(theme["summary"])))
        line = "//   accent: the kit's %s %s" % (s["accent_from"], s["accent_kit"])
        if s["accent_why"]:
            line += " (%s)" % s["accent_why"]
        out.append(line + "; backdrop: %s -> %s." % (theme["backdrop"]["mode"], s["backdrop"]))
        for move in moves:
            out.append("//   contrast: " + move + ".")
        values = [c_string(name)] + [rgba(s[f]) for f in FIELDS] + [rgba(s["text"]), rgba(s["onAccent"]), s["backdrop"]]
        values += ["%.2ff" % s["round"], "true" if s["light"] else "false", rgba(s["line"]), rgba(s["good"]), rgba(s["bad"]), c_string(theme["family"])]
        out.append("{" + ", ".join(values) + "},")
    return "\n".join(out) + "\n"


# ------------------------------------------------------------------------------ the contact sheet


def draw_sheet(path, skins):
    from PIL import Image, ImageDraw, ImageFont

    def font(size, bold=False):
        for name in (["DejaVuSans-Bold.ttf"] if bold else ["DejaVuSans.ttf"]):
            for folder in ("/usr/share/fonts/truetype/dejavu", "/usr/share/fonts/dejavu", "/usr/share/fonts/TTF"):
                p = os.path.join(folder, name)
                if os.path.exists(p):
                    return ImageFont.truetype(p, size)
        return ImageFont.load_default()

    columns, w, h, gap = 5, 420, 300, 16
    rows = (len(skins) + columns - 1) // columns
    image = Image.new("RGB", (columns * (w + gap) + gap, rows * (h + gap) + gap + 40), (40, 40, 44))
    d = ImageDraw.Draw(image, "RGBA")
    d.text((gap, 10), "Crux2 skins from ps5-homebrew-ui themes (BlackBearReloaded, GPL-3.0-or-later)", fill=(230, 230, 230), font=font(18, True))
    big, small, tiny = font(22, True), font(15), font(12)
    for i, (function, theme, s, moves) in enumerate(skins):
        x0 = gap + (i % columns) * (w + gap)
        y0 = 40 + gap + (i // columns) * (h + gap)
        c = {k: v.rgb255() for k, v in s.items() if isinstance(v, Color)}
        radius = int(round(s["round"] * 10))
        # The page: its two colours top to bottom.
        for y in range(h):
            t = y / (h - 1)
            d.line([(x0, y0 + y), (x0 + w, y0 + y)], fill=tuple(int(a + (b - a) * t) for a, b in zip(c["bgTop"], c["bgBottom"])))
        d.rectangle([x0, y0, x0 + w, y0 + 34], fill=c["bar"] + (round(s["bar"].a * 255),))
        d.text((x0 + 12, y0 + 7), decoded(theme["name"]), fill=c["text"], font=big)
        fam = decoded(theme["family"])
        d.text((x0 + w - 12 - d.textlength(fam, font=tiny), y0 + 12), fam, fill=c["dim"], font=tiny)
        # A panel with text, secondary text, a hairline, a card and a button in the accent.
        d.rounded_rectangle([x0 + 12, y0 + 46, x0 + w - 12, y0 + 196], radius=radius, fill=c["panel"])
        d.text((x0 + 24, y0 + 54), "Ape Escape 2", fill=c["text"], font=font(18, True))
        d.text((x0 + 24, y0 + 78), "SCES-50885 - played 12 h", fill=c["dim"], font=small)
        d.text((x0 + 24, y0 + 97), "faint: last played in May", fill=c["faint"], font=tiny)
        d.line([(x0 + 24, y0 + 117), (x0 + w - 24, y0 + 117)], fill=c["line"] + (round(s["line"].a * 255),), width=2)
        d.rounded_rectangle([x0 + 24, y0 + 126, x0 + 160, y0 + 182], radius=radius, fill=c["card"])
        d.rounded_rectangle([x0 + 172, y0 + 126, x0 + 280, y0 + 182], radius=radius, fill=c["panelHi"])
        d.text((x0 + 36, y0 + 146), "card", fill=c["text"], font=small)
        d.text((x0 + 184, y0 + 146), "panelHi", fill=c["text"], font=small)
        d.rounded_rectangle([x0 + 292, y0 + 136, x0 + w - 24, y0 + 172], radius=max(radius, 4), fill=c["accent"])
        label = "Play"
        d.text((x0 + 292 + (w - 24 - 292 - d.textlength(label, font=font(16, True))) / 2, y0 + 143), label, fill=c["onAccent"], font=font(16, True))
        # Text straight on the page, and the status colours.
        d.text((x0 + 14, y0 + 204), "On the page: Library  Settings", fill=c["text"], font=small)
        d.rounded_rectangle([x0 + 12, y0 + 228, x0 + w - 12, y0 + 260], radius=radius, fill=c["sheet"])
        d.text((x0 + 22, y0 + 235), "sheet", fill=c["text"], font=small)
        for k, name in enumerate(["warm", "good", "bad"]):
            d.ellipse([x0 + 140 + k * 70, y0 + 236, x0 + 156 + k * 70, y0 + 252], fill=c[name])
            d.text((x0 + 160 + k * 70, y0 + 236), name, fill=c["dim"], font=tiny)
        status = "%s  round %.2f  %s%s" % (s["backdrop"], s["round"], "light" if s["light"] else "dark", "  adjusted" if any("contrast" not in m and "one text" not in m for m in moves) else "")
        d.text((x0 + 14, y0 + 272), status, fill=c["dim"], font=tiny)
    image.save(path)


# ------------------------------------------------------------------------------ the command


def main():
    parser = argparse.ArgumentParser(description="Makes bp_kit_skins.inc from the ps5-homebrew-ui kit's themes.")
    parser.add_argument("kit", help="the kit's folder (a clone of ps5-homebrew-ui)")
    here = os.path.dirname(os.path.abspath(__file__))
    parser.add_argument("--out", default=os.path.join(here, "..", "frontend", "bigpicture", "bp_kit_skins.inc"))
    parser.add_argument("--commit", help="the kit's revision, for the header (default: git's, in the kit's folder)")
    parser.add_argument("--sheet", help="also draw a contact sheet of the skins into this PNG (needs Pillow)")
    parser.add_argument("--check", action="store_true", help="only say whether the .inc on disk is up to date")
    args = parser.parse_args()

    commit = args.commit
    if not commit:
        try:
            commit = subprocess.run(["git", "-C", args.kit, "rev-parse", "--short=7", "HEAD"], capture_output=True, text=True, check=True).stdout.strip()
        except (OSError, subprocess.CalledProcessError):
            raise SystemExit("make-kit-skins: the kit is not a git clone: name its revision with --commit")

    kit = Kit(args.kit)
    themes = kit.themes()
    if len(themes) != len(kit.order):
        raise SystemExit("make-kit-skins: read %d themes of %d" % (len(themes), len(kit.order)))
    skins = []
    for function, theme in zip(kit.order, themes):
        s, moves = make_skin(theme)
        skins.append((function, theme, s, moves))
        if failing(s):
            print("%s: still short of contrast after the moves" % decoded(theme["name"]), file=sys.stderr)
    text = write_inc(kit, commit, skins)

    if args.check:
        with open(args.out, encoding="utf-8") as f:
            same = f.read() == text
        print("bp_kit_skins.inc is %s" % ("up to date" if same else "out of date"))
        return 0 if same else 1
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print("%d skins written to %s (kit %s)" % (len(skins), os.path.normpath(args.out), commit))
    for function, theme, s, moves in skins:
        ratios = "  ".join("%s/%s %.2f" % (a, b, contrast(s[a], s[b])) for a, b, _ in CHECKS)
        print("  %-14s accent=%-7s %s" % (decoded(theme["name"]), s["accent_from"], ratios))
        for move in moves:
            print("      " + move)
    if args.sheet:
        draw_sheet(args.sheet, skins)
        print("contact sheet: %s" % args.sheet)
    return 0


if __name__ == "__main__":
    sys.exit(main())
