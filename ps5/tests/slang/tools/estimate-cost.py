#!/usr/bin/env python3
"""Crux2 - what a preset's frame may cost the PS5's GPU at 4K, from measurements made on a PC.

SPDX-License-Identifier: GPL-3.0+

  estimate-cost.py FETCHES_TSV STATS_DIR

FETCHES_TSV is slang_vk --count's fetches.tsv (each pass's size at 4K, its texture fetches a pixel
counted on lavapipe, its picture's bytes a pixel); STATS_DIR holds <preset>.stats, the console's
compiler's statistics for each pipeline (RADV_DEBUG=shaderstats,nocache over the drm-shim build of
ps5/tests/driver, as run-slang-tests.sh makes them), one pixel shader a pass, in order.

The PS5's GPU taken as 36 RDNA 2 compute units at 2.23 GHz (72 SIMD32, 4 bilinear texels a clock a
unit, 448 GB/s). For each pass the larger of:
  ALU   waves (pixels / 64, RADV's pixel shader wave size) x ACO's "Inverse Throughput" (cycles a
        wave) / (72 x 2.23 GHz); ACO counts a loop's body once, so it is scaled by the fetches
        counted over the fetch instructions in the code when there are more;
  TEX   fetches / (36 x 4 x 2.23 GHz), halved in rate for a 64-bit input picture (assumed when the
        pass reads after a float pass: counted as 1.5x on average);
  WRITE the pass's picture written once at its bytes a pixel / 448 GB/s.
An estimate within perhaps a factor of 1.5 either way: what the console measures decides.
"""
import collections
import re
import sys

CLOCK, SIMDS, CUS, BANDWIDTH = 2.23e9, 72, 36, 448e9


def pixel_shaders(path):
    out, current = [], None
    for line in open(path, errors="replace"):
        if line.startswith("Pixel Shader"):
            current = {}
            out.append(current)
        elif current is not None:
            m = re.match(r"(Inverse Throughput|VMEM|Instructions|VGPRs): (\d+)", line)
            if m:
                current.setdefault(m.group(1), int(m.group(2)))
            if line.startswith("****") and current:
                current = None if "Inverse Throughput" in current else current
    return out


def main():
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    passes = collections.OrderedDict()
    for line in open(sys.argv[1]):
        name, p, w, h, fpp, bpp = line.split("\t")
        passes.setdefault(name, []).append((int(p), int(w), int(h), float(fpp), int(bpp)))
    for name, rows in passes.items():
        try:
            stats = pixel_shaders("%s/%s.stats" % (sys.argv[2], name))
        except OSError:
            print("%-30s no shader statistics" % name)
            continue
        total, worst = 0.0, (0.0, -1)
        floats = False
        parts = []
        for (p, w, h, fpp, bpp), s in zip(rows, stats + [{}] * len(rows)):
            pixels = w * h
            vmem = max(s.get("VMEM", 1), 1)
            loop = max(1.0, fpp / vmem) if fpp > 0 else 1.0
            alu = pixels / 64.0 * s.get("Inverse Throughput", 0) * loop / (SIMDS * CLOCK)
            tex = pixels * max(fpp, 0) * (1.5 if floats else 1.0) / (CUS * 4 * CLOCK)
            write = pixels * bpp / BANDWIDTH
            ms = 1e3 * max(alu, tex, write)
            parts.append("%.2f" % ms)
            total += ms
            worst = max(worst, (ms, p))
            floats = bpp >= 8
        flag = "  <-- over 2 ms" if total > 2.0 else ""
        print("%-30s %5.2f ms at 4K (by pass: %s; heaviest pass %d)%s" % (name, total, " ".join(parts), worst[1], flag))
    return 0


if __name__ == "__main__":
    sys.exit(main())
