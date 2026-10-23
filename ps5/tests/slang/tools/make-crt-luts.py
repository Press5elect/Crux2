#!/usr/bin/env python3
"""Crux2 - the four colour lookup pictures crt-guest-advanced's pre-shaders pass offers (LUT Colors).

SPDX-License-Identifier: GPL-3.0+

  make-crt-luts.py OUT_DIR

libretro's own (crt/shaders/guest/advanced/lut/*.png) state no licence, so Crux2 makes its own from
published colour primaries. Each is 1024 x 32: 32 tiles of 32 x 32, blue choosing the tile, red
across a tile and green down it, as the shader reads them. Each maps a colour as the game drew it
(sRGB) to how it would look on that set's phosphors, shown on an sRGB / Rec.709 screen: decoded
with gamma 2.4, through the set's primaries and white to CIE XYZ, Bradford-adapted to D65, to
linear sRGB, clipped, encoded with gamma 2.4 again (the shader keeps the brightness of the
original, fix_lut()).

  trinitron-lut.png      SMPTE-C phosphors (the Trinitron's P22), 9300 K white
  inv-trinitron-lut.png  the inverse: sRGB as a 9300 K SMPTE-C set would be driven to show it
  nec-lut.png            EBU phosphors, 9300 K white (a NEC MultiSync-like monitor)
  ntsc-lut.png           NTSC 1953 (FCC) phosphors, illuminant C
"""
import os
import struct
import sys
import zlib


def xy_to_xyz(x, y):
    return [x / y, 1.0, (1.0 - x - y) / y]


def inverse(m):
    a, b, c = m[0]
    d, e, f = m[1]
    g, h, i = m[2]
    det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
    return [[(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det],
            [(f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det],
            [(d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det]]


def mul(a, b):
    return [[sum(a[r][k] * b[k][c] for k in range(3)) for c in range(3)] for r in range(3)]


def apply(m, v):
    return [sum(m[r][k] * v[k] for k in range(3)) for r in range(3)]


def rgb_to_xyz(primaries, white):
    cols = [xy_to_xyz(*p) for p in primaries]
    m = [[cols[c][r] for c in range(3)] for r in range(3)]
    s = apply(inverse(m), xy_to_xyz(*white))
    return [[m[r][c] * s[c] for c in range(3)] for r in range(3)]


BRADFORD = [[0.8951, 0.2664, -0.1614], [-0.7502, 1.7135, 0.0367], [0.0389, -0.0685, 1.0296]]


def adapt(src_white, dst_white):
    s = apply(BRADFORD, xy_to_xyz(*src_white))
    d = apply(BRADFORD, xy_to_xyz(*dst_white))
    scale = [[d[0] / s[0], 0, 0], [0, d[1] / s[1], 0], [0, 0, d[2] / s[2]]]
    return mul(inverse(BRADFORD), mul(scale, BRADFORD))


D65 = (0.3127, 0.3290)
D93 = (0.2831, 0.2971)
ILL_C = (0.3101, 0.3162)
SRGB = [(0.640, 0.330), (0.300, 0.600), (0.150, 0.060)]
SMPTE_C = [(0.630, 0.340), (0.310, 0.595), (0.155, 0.070)]
EBU = [(0.640, 0.330), (0.290, 0.600), (0.150, 0.060)]
NTSC_1953 = [(0.670, 0.330), (0.210, 0.710), (0.140, 0.080)]


def to_srgb(primaries, white):
    """The matrix from a set's linear RGB to linear sRGB."""
    return mul(inverse(rgb_to_xyz(SRGB, D65)), mul(adapt(white, D65), rgb_to_xyz(primaries, white)))


def png(path, width, height, rows):
    raw = b"".join(b"\0" + bytes(row) for row in rows)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def lut(path, matrix):
    size = 32
    rows = []
    for g in range(size):
        row = []
        for b in range(size):
            for r in range(size):
                v = [(r / 31.0) ** 2.4, (g / 31.0) ** 2.4, (b / 31.0) ** 2.4]
                o = apply(matrix, v)
                row += [int(round(255.0 * min(max(c, 0.0), 1.0) ** (1 / 2.4))) for c in o]
        rows.append(row)
    png(path, size * size, size, rows)


def main():
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    trinitron = to_srgb(SMPTE_C, D93)
    lut(os.path.join(out, "trinitron-lut.png"), trinitron)
    lut(os.path.join(out, "inv-trinitron-lut.png"), inverse(trinitron))
    lut(os.path.join(out, "nec-lut.png"), to_srgb(EBU, D93))
    lut(os.path.join(out, "ntsc-lut.png"), to_srgb(NTSC_1953, ILL_C))
    return 0


if __name__ == "__main__":
    sys.exit(main())
