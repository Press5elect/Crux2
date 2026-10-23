#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - the title's pictures in the console's menu:
#
#   sce_sys/icon0.png   the tile on the home screen (512x512)
#   sce_sys/pic0.dds    the picture behind the title when it is selected (3840x2160)
#   sce_sys/pic1.dds    the picture while the title loads (3840x2160)
#
# They are drawn here, in one of two styles:
#
#   --style crux     (the default, and what a package that is passed on carries) the title's own
#                    mark and name: one broad orange bar crossing two thin cream ones, which is the
#                    X of the name CRUX2, set in Saira ExtraBold (OFL-1.1, in
#                    ps5/package/resources/fonts). The same drawing the interface makes
#                    (frontend/bigpicture/bp_draw.cpp). Every pixel's origin is this script.
#   --style own      the mark the builds before the name Crux2 could be passed on with: a disc on a
#                    blue tile, with the name set in the interface's text font (Roboto, Apache-2.0).
#
# The DDS files are what the console's launcher reads: a DX10 header, BC7_UNORM, one level.
# The header and the small BC7 encoder (mode 6: two endpoints and a 4-bit index a pixel) are
# PS5_RetroArch's tools/make-title-art.py (Copyright (C) 2026 Mihawk-99, GPL-3.0-or-later),
# whose header the console has shown.
#
#   python3 ps5/tools/make-title-art.py [--style crux|own] [--out DIR] [--preview DIR]
#
# Needs numpy and Pillow.
# --out names another folder than ps5/sce_sys; --preview also writes the two backgrounds as PNG.
import argparse
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

WIDTH, HEIGHT = 3840, 2160
ROOT = Path(__file__).resolve().parent.parent.parent
RESOURCES = ROOT / "vendor" / "pcsx2" / "bin" / "resources"
SCE_SYS = ROOT / "ps5" / "sce_sys"

BACKGROUND_TOP = np.array([22.0, 28.0, 46.0])     # the top of the picture
BACKGROUND_BOTTOM = np.array([6.0, 7.0, 12.0])    # the bottom
LIGHT = np.array([58.0, 120.0, 230.0])            # the soft light behind the mark
TAG_COLOUR = (255, 255, 255, 150)
STYLE = "crux"
BRAND_FONT = ROOT / "ps5" / "package" / "resources" / "fonts" / "Saira-ExtraBold.ttf"
GROUND = (23, 18, 15)
CREAM = (243, 236, 228)
ORANGE = (255, 106, 61)


def background(width: int, height: int, light_x: float, light_y: float, light_radius: float) -> np.ndarray:
    """A gradient from top to bottom, a soft light at (light_x, light_y), a darker rim."""
    y = np.linspace(0.0, 1.0, height)[:, None, None]
    img = BACKGROUND_TOP * (1.0 - y) + BACKGROUND_BOTTOM * y
    img = np.broadcast_to(img, (height, width, 3)).copy()

    yy, xx = np.mgrid[0:height, 0:width].astype(np.float32)
    distance = np.sqrt((xx - light_x) ** 2 + (yy - light_y) ** 2) / light_radius
    glow = np.exp(-distance * distance * 1.6)[..., None]
    img += (LIGHT - img) * glow * 0.38

    # rings of light around it, faint
    rings = (np.cos(distance * 9.0) * 0.5 + 0.5) ** 6 * np.exp(-distance * 1.1)
    img += LIGHT * rings[..., None] * 0.035

    # the rim
    rim = ((xx / width - 0.5) ** 2 + (yy / height - 0.5) ** 2) * 1.5
    img *= (1.0 - np.clip(rim, 0.0, 0.55))[..., None]

    # Half a step of noise, so the slow gradient is dithered and shows no bands. (No more than
    # that: noise does not compress, and the title folder has to stay easy to send.)
    rng = np.random.default_rng(1994)
    img += rng.uniform(-0.5, 0.5, size=(height, width, 1))
    return np.clip(img, 0.0, 255.0)


def paste_with_shadow(canvas: Image.Image, mark: Image.Image, left: int, top: int, depth: float) -> Image.Image:
    """The picture with the mark on it, a soft shadow under the mark."""
    shadow = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
    silhouette = Image.new("RGBA", mark.size, (0, 0, 0, 255))
    silhouette.putalpha(mark.getchannel("A").point(lambda a: int(a * 0.55)))
    shadow.paste(silhouette, (left + int(depth * 0.55), top + int(depth)), silhouette)
    shadow = shadow.filter(ImageFilter.GaussianBlur(depth * 0.8))
    canvas = Image.alpha_composite(canvas, shadow)

    layer = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
    layer.paste(mark, (left, top), mark)
    return Image.alpha_composite(canvas, layer)


def font(size: int) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(str(RESOURCES / "fonts" / "Roboto-Regular.ttf"), size)


def own_mark(size: int) -> Image.Image:
    """This port's own mark: a disc on a rounded blue tile. Drawn at twice the size and scaled down."""
    n = size * 2
    yy, xx = np.mgrid[0:n, 0:n].astype(np.float32)
    u, v = xx / n, yy / n

    def edge(distance, width=1.5 / n):
        """1 inside (distance < 0), 0 outside, one and a half pixels of slope between."""
        return np.clip(0.5 - distance / width, 0.0, 1.0)

    # the tile: a rounded square, lighter at the top, with a soft sheen across its upper left
    radius = 0.21
    qx, qy = np.abs(u - 0.5) - (0.5 - radius), np.abs(v - 0.5) - (0.5 - radius)
    tile_distance = np.sqrt(np.maximum(qx, 0.0) ** 2 + np.maximum(qy, 0.0) ** 2) + np.minimum(np.maximum(qx, qy), 0.0) - radius
    tile = edge(tile_distance)
    top, bottom = np.array([58.0, 132.0, 245.0]), np.array([22.0, 34.0, 110.0])
    rgb = top * (1.0 - v[..., None]) + bottom * v[..., None]
    rgb += 26.0 * np.clip(1.0 - (u + v) * 1.15, 0.0, 1.0)[..., None]

    # the disc
    cx, cy = 0.5, 0.5
    distance = np.sqrt((u - cx) ** 2 + (v - cy) ** 2)
    angle = np.arctan2(v - cy, u - cx)
    disc = edge(distance - 0.335) * (1.0 - edge(distance - 0.062))
    # its surface: silver, with two opposite wedges of reflected light, tinted towards the rim
    wedge = np.cos(angle * 2.0 - 0.9) * 0.5 + 0.5
    silver = 205.0 + 45.0 * wedge ** 3
    surface = np.stack([silver - 30.0 * wedge * (distance / 0.335), silver - 6.0 * wedge, silver + 6.0], axis=-1)
    surface *= (0.90 + 0.10 * np.cos(distance * 260.0) ** 2)[..., None]      # the tracks, faintly
    # the clear ring around the hub, and the hub's own ring
    clear = edge(distance - 0.135)
    surface = surface * (1.0 - clear[..., None]) + (surface * 0.55 + rgb * 0.45) * clear[..., None]
    hub = edge(distance - 0.098) * (1.0 - edge(distance - 0.085))
    surface = surface * (1.0 - hub[..., None]) + np.array([245.0, 248.0, 255.0]) * hub[..., None]
    # a thin dark rim, so the disc stands off the tile
    rim = edge(distance - 0.335) * (1.0 - edge(distance - 0.327))
    surface *= (1.0 - 0.35 * rim)[..., None]

    # its shadow on the tile
    shadow = np.exp(-((np.sqrt((u - cx - 0.012) ** 2 + (v - cy - 0.02) ** 2) - 0.335).clip(0.0, None) / 0.03) ** 2) * 0.45
    rgb *= (1.0 - shadow * (1.0 - disc))[..., None]
    rgb = rgb * (1.0 - disc[..., None]) + surface * disc[..., None]

    out = np.dstack([np.clip(rgb, 0.0, 255.0), tile * 255.0])
    return Image.fromarray(np.round(out).astype(np.uint8), "RGBA").resize((size, size), Image.LANCZOS)


def crux_mark(size: int) -> Image.Image:
    """The title's mark in a square of `size`: the drawing it was designed as, a square of 100 with
    the thin bars 7 wide and 7 to either side of the middle, the broad one 16 wide, all 84 long
    with round ends, the thin ones cut 2.5 clear of the broad one. Drawn at four times the size."""
    n = size * 4
    u = n / 100.0

    def capsule(draw, angle_degrees, aside, length, width, fill):
        # a bar that stands upright in the drawing, `aside` from the middle, turned about the middle
        a = np.radians(angle_degrees)
        across, along = np.array([np.cos(a), np.sin(a)]), np.array([-np.sin(a), np.cos(a)])
        middle = np.array([n / 2, n / 2]) + across * aside * u
        half = (length / 2 - width / 2) * u
        p0, p1 = middle - along * half, middle + along * half
        r = width / 2 * u
        draw.line([tuple(p0), tuple(p1)], fill=fill, width=int(round(width * u)))
        for p in (p0, p1):
            draw.ellipse([p[0] - r, p[1] - r, p[0] + r, p[1] + r], fill=fill)

    thin = Image.new("L", (n, n), 0)
    draw = ImageDraw.Draw(thin)
    capsule(draw, -45, -7, 84, 7, 255)
    capsule(draw, -45, 7, 84, 7, 255)
    capsule(draw, 45, 0, 84 + 5, 16 + 5, 0)           # the gap where the broad bar passes
    broad = Image.new("L", (n, n), 0)
    capsule(ImageDraw.Draw(broad), 45, 0, 84, 16, 255)
    out = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    out.paste(Image.new("RGBA", (n, n), CREAM + (255,)), (0, 0), thin)
    out.paste(Image.new("RGBA", (n, n), ORANGE + (255,)), (0, 0), broad)
    return out.resize((size, size), Image.LANCZOS)


def crux_name(type_size: int) -> Image.Image:
    """The name: C R U, the mark as its X, 2, laid out as the interface lays it out (each letter as
    wide as the face makes it and a hundredth of the type more; the mark in a square of the type's
    size that the letters beside it reach into by an eleventh of it; the capitals' middle on the
    mark's)."""
    scale = 2
    type_px = type_size * scale
    face = ImageFont.truetype(str(BRAND_FONT), type_px)
    parts = ["C", "R", "U", None, "2"]
    widths = [type_px * (1 - 2 * 20 / 220) if part is None else face.getlength(part) + 0.01 * type_px for part in parts]
    width, height = int(np.ceil(sum(widths))) + 2 * type_px // 5, int(type_px * 1.1)
    out = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    draw = ImageDraw.Draw(out)
    x, middle = type_px / 5, height / 2
    for part, part_width in zip(parts, widths):
        if part is None:
            mark = crux_mark(type_px)
            out.alpha_composite(mark, (int(round(x + part_width / 2 - type_px / 2)), int(round(middle - type_px / 2))))
        else:
            # (Saira's capitals are 0.688 of the type high: their middle is that far above the line.)
            draw.text((x, middle + 0.344 * type_px), part, font=face, fill=(ORANGE if part == "2" else CREAM) + (255,), anchor="ls")
        x += part_width
    return out.resize((width // scale, height // scale), Image.LANCZOS)


def with_mark(img, centre_x: float, centre_y: float, size: int) -> Image.Image:
    """The mark, centred on the point."""
    canvas = img if isinstance(img, Image.Image) else Image.fromarray(np.round(img).astype(np.uint8), "RGB").convert("RGBA")
    if STYLE == "crux":
        mark = crux_mark(size)
    else:
        mark = own_mark(size)
    return paste_with_shadow(canvas, mark, int(round(centre_x - size / 2)), int(round(centre_y - size / 2)), size / 26)


def with_wordmark(canvas: Image.Image, centre_x: float, top: float, width: int) -> tuple:
    """The name, its top edge at top. Returns the picture and the y under the name."""
    if STYLE == "crux":
        mark = crux_name(int(width * 0.27))
    else:
        face = font(int(width * 0.19))
        box = face.getbbox("Crux2", stroke_width=max(1, width // 300))
        mark = Image.new("RGBA", (box[2] - box[0] + 8, box[3] - box[1] + 8), (0, 0, 0, 0))
        ImageDraw.Draw(mark).text((4 - box[0], 4 - box[1]), "Crux2", font=face, fill=(255, 255, 255, 255),
                                  stroke_width=max(1, width // 300), stroke_fill=(255, 255, 255, 255))
    canvas = paste_with_shadow(canvas, mark, int(round(centre_x - mark.size[0] / 2)), int(round(top)), mark.size[1] / 9)
    return canvas, top + mark.size[1]


def centred_text(canvas: Image.Image, text: str, centre_x: float, top: float, face, fill) -> float:
    """Draws the text centred on centre_x; returns the y under it."""
    layer = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)
    box = draw.textbbox((0, 0), text, font=face)
    draw.text((centre_x - (box[2] - box[0]) / 2 - box[0], top - box[1]), text, font=face, fill=fill)
    canvas.alpha_composite(layer)
    return top + (box[3] - box[1])


def draw_icon() -> Image.Image:
    size = 512
    img = background(size, size, size * 0.5, size * 0.47, size * 0.62)
    canvas = with_mark(img, size * 0.5, size * 0.5, int(size * (0.86 if STYLE == "crux" else 0.80)))
    return canvas.convert("RGB")


def draw_selected() -> Image.Image:
    """Behind the title in the console's menu: the console writes the title's name and its buttons
    over the lower left, so the icon and the wordmark stand to the right and nothing else is written."""
    img = background(WIDTH, HEIGHT, WIDTH * 0.70, HEIGHT * 0.40, HEIGHT * 0.95)
    if STYLE == "crux":
        # (The mark is in the name: the name alone, large.)
        canvas = Image.fromarray(np.round(img).astype(np.uint8), "RGB").convert("RGBA")
        canvas, _ = with_wordmark(canvas, WIDTH * 0.70, HEIGHT * 0.26, 1690)
        return canvas.convert("RGB")
    canvas = with_mark(img, WIDTH * 0.70, HEIGHT * 0.36, 820)
    canvas, _ = with_wordmark(canvas, WIDTH * 0.70, HEIGHT * 0.36 + 820 / 2 + 70, 1690)
    return canvas.convert("RGB")


def draw_loading() -> Image.Image:
    """While the title loads: the icon, the wordmark and a line under them, in the middle."""
    img = background(WIDTH, HEIGHT, WIDTH * 0.5, HEIGHT * 0.42, HEIGHT * 0.95)
    if STYLE == "crux":
        canvas = Image.fromarray(np.round(img).astype(np.uint8), "RGB").convert("RGBA")
        canvas, under = with_wordmark(canvas, WIDTH * 0.5, HEIGHT * 0.30, 2200)
        centred_text(canvas, "by Press5elect  -  github.com/Press5elect", WIDTH * 0.5, under + 60, font(72), TAG_COLOUR)
        return canvas.convert("RGB")
    canvas = with_mark(img, WIDTH * 0.5, HEIGHT * 0.36, 800)
    canvas, under = with_wordmark(canvas, WIDTH * 0.5, HEIGHT * 0.36 + 800 / 2 + 60, 1970)
    centred_text(canvas, "by Press5elect  -  github.com/Press5elect", WIDTH * 0.5, under + 90, font(72), TAG_COLOUR)
    return canvas.convert("RGB")


# ---- BC7 mode 6 and the DDS header (PS5_RetroArch's tools/make-title-art.py) ----------------------
# One subset, RGBA endpoints of 7 bits and a p-bit, 4-bit indices.
WEIGHTS = np.array([0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64], dtype=np.int64)


def quantise(e: np.ndarray) -> np.ndarray:
    """Endpoint colours to the 7 stored bits (the p-bit is 1, for alpha 255)."""
    return np.clip(np.round((e - 1.0) / 2.0), 0, 127).astype(np.int64)


def palette(q0: np.ndarray, q1: np.ndarray) -> np.ndarray:
    """The 16 colours two quantised endpoints give, (blocks, 16, 3)."""
    e0, e1 = (q0 * 2 + 1)[:, None, :], (q1 * 2 + 1)[:, None, :]
    w = WEIGHTS[None, :, None]
    return ((64 - w) * e0 + w * e1 + 32) >> 6


def choose(px: np.ndarray, q0: np.ndarray, q1: np.ndarray):
    """Each pixel's best index for these endpoints, and the block's error."""
    pal = palette(q0, q1)
    err = ((px[:, :, None, :] - pal[:, None, :, :]) ** 2).sum(-1)
    idx = err.argmin(-1)
    return idx, np.take_along_axis(err, idx[..., None], -1)[..., 0].sum(-1)


def encode_blocks(px: np.ndarray) -> bytes:
    """BC7 mode 6 for blocks of 16 RGB pixels (blocks, 16, 3), alpha 255."""
    pf = px.astype(np.float64)
    mean = pf.mean(1)
    cen = pf - mean[:, None, :]
    cov = np.einsum('nki,nkj->nij', cen, cen)
    axis = np.ones((len(px), 3)) / np.sqrt(3.0)
    for _ in range(8):
        axis = np.einsum('nij,nj->ni', cov, axis)
        norm = np.sqrt((axis * axis).sum(1, keepdims=True))
        axis = np.where(norm > 1e-9, axis / np.maximum(norm, 1e-9), 1.0 / np.sqrt(3.0))
    proj = np.einsum('nki,ni->nk', cen, axis)
    q0 = quantise(mean + proj.min(1, keepdims=True) * axis)
    q1 = quantise(mean + proj.max(1, keepdims=True) * axis)
    idx, err = choose(px, q0, q1)
    # One least-squares refit of the endpoints to the chosen indices
    w = WEIGHTS[idx].astype(np.float64) / 64.0
    a, b, c = ((1 - w) ** 2).sum(1), ((1 - w) * w).sum(1), (w * w).sum(1)
    d0 = ((1 - w)[..., None] * pf).sum(1)
    d1 = (w[..., None] * pf).sum(1)
    det = a * c - b * b
    ok = det > 1e-9
    safe = np.where(ok, det, 1.0)[:, None]
    r0 = quantise(np.where(ok[:, None], (c[:, None] * d0 - b[:, None] * d1) / safe, mean))
    r1 = quantise(np.where(ok[:, None], (a[:, None] * d1 - b[:, None] * d0) / safe, mean))
    idx2, err2 = choose(px, r0, r1)
    better = (err2 < err)[:, None]
    q0, q1 = np.where(better, r0, q0), np.where(better, r1, q1)
    idx = np.where(better, idx2, idx)
    # The first pixel's index is stored in 3 bits: swap the ends if it needs 4
    swap = idx[:, 0] >= 8
    q0, q1 = np.where(swap[:, None], q1, q0), np.where(swap[:, None], q0, q1)
    idx = np.where(swap[:, None], 15 - idx, idx)

    lo = np.full(len(px), 1 << 6, dtype=np.uint64)   # mode 6
    hi = np.zeros(len(px), dtype=np.uint64)
    pos = 7

    def put(value: np.ndarray, bits: int):
        nonlocal pos
        for bit in range(bits):
            v = ((value >> bit) & 1).astype(np.uint64)
            if pos < 64:
                lo[:] |= v << np.uint64(pos)
            else:
                hi[:] |= v << np.uint64(pos - 64)
            pos += 1

    for ch in range(3):
        put(q0[:, ch], 7)
        put(q1[:, ch], 7)
    alpha = np.full(len(px), 127, dtype=np.int64)
    put(alpha, 7)
    put(alpha, 7)
    one = np.ones(len(px), dtype=np.int64)
    put(one, 1)   # P0
    put(one, 1)   # P1
    put(idx[:, 0], 3)
    for i in range(1, 16):
        put(idx[:, i], 4)
    out = np.empty(len(px) * 2, dtype="<u8")
    out[0::2], out[1::2] = lo, hi
    return out.tobytes()


def bc7(img: np.ndarray) -> bytes:
    """The picture as BC7 blocks, row of blocks by row of blocks."""
    rgb = np.clip(np.round(img), 0, 255).astype(np.int64)
    by, bx = HEIGHT // 4, WIDTH // 4
    blocks = rgb.reshape(by, 4, bx, 4, 3).transpose(0, 2, 1, 3, 4).reshape(by * bx, 16, 3)
    step = bx * 16
    return b"".join(encode_blocks(blocks[i:i + step]) for i in range(0, len(blocks), step))


def dds_header() -> bytes:
    """The DDS header of the launcher art the console has shown: DX10, BC7_UNORM
    (98), 2D, one level, straight alpha."""
    DDSD_CAPS, DDSD_HEIGHT, DDSD_WIDTH, DDSD_PIXELFORMAT = 0x1, 0x2, 0x4, 0x1000
    DDSD_MIPMAPCOUNT, DDSD_LINEARSIZE = 0x20000, 0x80000
    flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT | DDSD_MIPMAPCOUNT | DDSD_LINEARSIZE
    linear = WIDTH * HEIGHT               # BC7: 16 bytes a 4x4 block, one byte a pixel
    header = struct.pack("<4sIIIIIII", b"DDS ", 124, flags, HEIGHT, WIDTH, linear, 1, 1)
    header += b"\0" * 44                  # reserved
    header += struct.pack("<II4sIIIII", 32, 0x4, b"DX10", 0, 0, 0, 0, 0)
    header += struct.pack("<IIIII", 0x1000, 0, 0, 0, 0)   # caps: texture
    header += struct.pack("<IIIII", 98, 3, 0, 1, 1)       # BC7_UNORM, TEXTURE2D, array 1, straight alpha
    assert len(header) == 148
    return header


def decode_mode6(data: bytes) -> np.ndarray:
    """The picture back from the blocks above (mode 6 only): the check on the encoder."""
    words = np.frombuffer(data, dtype="<u8")
    lo, hi = words[0::2], words[1::2]

    def bits(position: int, count: int) -> np.ndarray:
        out = np.zeros(len(lo), dtype=np.int64)
        for bit in range(count):
            p = position + bit
            v = ((lo >> np.uint64(p)) if p < 64 else (hi >> np.uint64(p - 64))) & np.uint64(1)
            out |= v.astype(np.int64) << bit
        return out

    assert (bits(0, 7) == 64).all(), "a block is not mode 6"
    position = 7
    ends = np.zeros((len(lo), 2, 3), dtype=np.int64)
    for ch in range(3):
        for end in range(2):
            ends[:, end, ch] = bits(position, 7)
            position += 7
    position += 14 + 2  # alpha endpoints, the two p-bits (both 1)
    indices = np.zeros((len(lo), 16), dtype=np.int64)
    indices[:, 0] = bits(position, 3)
    position += 3
    for i in range(1, 16):
        indices[:, i] = bits(position, 4)
        position += 4
    pal = palette(ends[:, 0], ends[:, 1])
    px = np.take_along_axis(pal, indices[..., None].repeat(3, -1), 1)
    by, bx = HEIGHT // 4, WIDTH // 4
    return px.reshape(by, bx, 4, 4, 3).transpose(0, 2, 1, 3, 4).reshape(HEIGHT, WIDTH, 3)


def main() -> int:
    global STYLE, SCE_SYS
    parser = argparse.ArgumentParser(description="Draw the title's pictures for the console's menu.")
    parser.add_argument("--style", choices=("crux", "own"), default="crux")
    parser.add_argument("--out", type=Path, default=SCE_SYS, help="where to write them (default ps5/sce_sys)")
    parser.add_argument("--preview", type=Path, help="also write the two backgrounds as PNG files here")
    args = parser.parse_args()
    STYLE, SCE_SYS, preview = args.style, args.out, args.preview
    if STYLE == "crux":
        # The mark's own ground: its warm black, and its orange as the light behind it.
        global BACKGROUND_TOP, BACKGROUND_BOTTOM, LIGHT
        BACKGROUND_TOP = np.array([30.0, 23.0, 19.0])
        BACKGROUND_BOTTOM = np.array([17.0, 13.0, 11.0])
        LIGHT = np.array([120.0, 52.0, 30.0])
    if preview:
        preview.mkdir(parents=True, exist_ok=True)

    SCE_SYS.mkdir(parents=True, exist_ok=True)
    icon = draw_icon()
    icon.save(SCE_SYS / "icon0.png", optimize=True)
    print(f"sce_sys/icon0.png: {icon.size[0]}x{icon.size[1]}")

    for name, picture in (("pic0", draw_selected()), ("pic1", draw_loading())):
        img = np.asarray(picture, dtype=np.float64)
        blocks = bc7(img)
        (SCE_SYS / f"{name}.dds").write_bytes(dds_header() + blocks)
        error = decode_mode6(blocks).astype(np.float64) - np.round(img)
        psnr = 10.0 * np.log10(255.0 * 255.0 / max(float((error ** 2).mean()), 1e-9))
        print(f"sce_sys/{name}.dds: {148 + len(blocks)} bytes, {psnr:.1f} dB against the drawing")
        if psnr < 38.0:
            print(f"{name}: the compressed picture is too far from the drawing", file=sys.stderr)
            return 1
        if preview:
            picture.save(preview / f"{name}.png")
    if preview:
        icon.save(preview / "icon0.png")
    return 0


if __name__ == "__main__":
    sys.exit(main())
