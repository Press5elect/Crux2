# Crux2 - the picture looks: libretro shader presets that come with the title

These are libretro "slang" presets (the format RetroArch reads, github.com/libretro/slang-shaders),
run by Crux2 between the game's picture and the screen. They are read by
`ps5/frontend/ps5_slang.cpp`, compiled on the console by PCSX2's shader compiler, and run by the
chain runner in PCSX2's Vulkan renderer. Presets of the user's own (.slangp with the files they
name, from a USB drive) go through the same importer.

The folders keep slang-shaders' layout (`crt/`, `crt/shaders/guest/...`) so that presets' relative
paths and `../` work as they do in RetroArch; Crux2's own presets are in `crux2/`.

## The presets

| Preset | Passes | What it is | Estimated at 4K on the PS5 |
| --- | --- | --- | --- |
| `crt/crt-guest-advanced.slangp` | 12 | guest(r)'s CRT: scanlines, mask, bloom, glow, afterglow, curvature off | about 2.6 ms |
| `crt/crt-guest-advanced-fast.slangp` | 9 | the same look with fewer, cheaper passes | about 1.9 ms |
| `crt/crt-guest-advanced-fastest.slangp` | 5 | the cheapest of guest's CRT looks | about 1.0 ms |
| `crux2/crt-guest-advanced-composite.slangp` | 12 | crt-guest-advanced fed by Crux2's composite signal (stands in for libretro's crt-guest-advanced-ntsc, whose NTSC passes state no licence) | about 2.6 ms |
| `crux2/composite.slangp` | 2 | a television's composite input: colour cut to a narrow band and late, rainbow on fine detail, crawling dots on colour edges | about 0.1 ms |
| `crux2/s-video.slangp` | 2 | a television's S-Video input: colour cut to a band and a little late, brightness a little soft | about 0.1 ms |
| `crux2/film-grain.slangp` | 1 | film grain, new every frame, strongest in the middle tones | about 0.5 ms |
| `crux2/colour.slangp` | 1 | colour temperature, vibrance, saturation, brightness, contrast, gamma | about 0.1 ms |

The estimates are from the PC tests (`ps5/tests/slang/run-slang-tests.sh`): each pass's texture
fetches counted on lavapipe, and the console's own compiler's statistics for each shader (RADV,
for a Navi 2 GPU) turned into time for the PS5's 36 compute units at 2.23 GHz
(`ps5/tests/slang/tools/estimate-cost.py`). The console's own measurement decides.

guest's presets show a 448-line PlayStation 2 picture as interlaced (his "Interlace Trigger
Resolution" is 375 lines): set the parameter `interm` (Interlace Mode) to 0 for 448 progressive
scanlines.

## Every file, where it is from, its licence

### From libretro slang-shaders, revision e1d75632a205c70f14a4cc947c46d5abb7b3f7f1 (2026-10-08)

Unchanged copies. Each states in its header: "This program is free software; you can redistribute
it and/or modify it under the terms of the GNU General Public License as published by the Free
Software Foundation; either version 2 of the License, or (at your option) any later version."
That is GPL-2.0-or-later; Crux2 (GPL-3.0-or-later) uses them under GPL-3.0.

| File | Copyright (as the file states) |
| --- | --- |
| `crt/shaders/guest/advanced/afterglow0.slang` | 2020-2025 guest(r) |
| `crt/shaders/guest/advanced/avg-lum.slang` | 2018-2025 guest(r) |
| `crt/shaders/guest/advanced/bloom_horizontal.slang` | 2020-2023 guest(r) |
| `crt/shaders/guest/advanced/bloom_vertical.slang` | 2020-2023 guest(r) |
| `crt/shaders/guest/advanced/crt-guest-advanced.slang` | 2018-2025 guest(r) |
| `crt/shaders/guest/advanced/deconvergence.slang` | 2018-2025 guest(r) |
| `crt/shaders/guest/advanced/gaussian_horizontal.slang` | 2020-2025 guest(r) |
| `crt/shaders/guest/advanced/gaussian_vertical.slang` | 2020-2025 guest(r) |
| `crt/shaders/guest/advanced/linearize.slang` | 2020-2025 guest(r) |
| `crt/shaders/guest/advanced/pre-shaders-afterglow.slang` | 2019-2025 guest(r) and Dr. Venom |
| `crt/shaders/guest/fast/bloom_horizontal.slang` | 2020-2025 guest(r) |
| `crt/shaders/guest/fast/bloom_vertical.slang` | 2020-2025 guest(r) |
| `crt/shaders/guest/fast/crt-guest-advanced-pass1.slang` | 2018-2025 guest(r) |
| `crt/shaders/guest/fast/crt-guest-advanced-pass1f.slang` | 2018-2025 guest(r) |
| `crt/shaders/guest/fast/crt-guest-advanced-pass2.slang` | 2018-2025 guest(r) |
| `crt/shaders/guest/fast/crt-guest-advanced-pass2f.slang` | 2018-2025 guest(r) |
| `crt/shaders/guest/fast/deconvergence-f.slang` | 2018-2025 guest(r) |
| `crt/shaders/guest/fast/linearize.slang` | 2020-2025 guest(r) |
| `crt/shaders/guest/fast/linearizef.slang` | 2020-2025 guest(r) |
| `crt/shaders/guest/fast/perf-pass.slang` | 2019-2025 guest(r) |
| `crt/shaders/guest/fast/pre-shaders.slang` | 2019-2025 guest(r) and Dr. Venom |
| `crt/shaders/guest/fast/pre-shadersf.slang` | 2019-2025 guest(r) and Dr. Venom |

### Crux2's own (GPL-3.0-or-later)

| File | What, and why it is Crux2's |
| --- | --- |
| `crt/crt-guest-advanced.slangp`, `crt/crt-guest-advanced-fast.slangp`, `crt/crt-guest-advanced-fastest.slangp` | the same passes, scales and pictures as libretro's presets of those names at the revision above (libretro's .slangp files state no licence, so these are written out anew) |
| `crt/shaders/guest/advanced/stock.slang`, `crt/shaders/guest/fast/stock.slang` | the picture passed through, as libretro's `stock.slang` does (which states no licence) |
| `crt/shaders/guest/advanced/lut/*.png` | the four colour lookup pictures guest's "LUT Colors" parameter chooses (Trinitron, inverse Trinitron, NEC MultiSync-like, NTSC 1953), made by `ps5/tests/slang/tools/make-crt-luts.py` from published phosphor primaries; libretro's own state no licence |
| `crux2/*.slangp`, `crux2/shaders/composite.slang`, `s-video.slang`, `signal.inc`, `film-grain.slang`, `colour.slang` | Crux2's looks |
| `crux2/shaders/scale.slang` | sharp bilinear to the screen's size, adapted from PSSwanStation's `look_scale.frag` (GPL-3.0-or-later, the same author) |

### What was looked at and is not shipped

- `crt/shaders/guest/advanced/ntsc/ntsc-pass1..3.slang` ("based on Themaister's NTSC shader") and
  `pal/pal-pass1..3.slang`: no licence stated, so `crt-guest-advanced-ntsc` and `-pal` are not
  shipped; `crux2/crt-guest-advanced-composite.slangp` takes their place.
- `crt/shaders/guest/hd/*` (GPL-2.0-or-later like the rest): `crt-guest-advanced-hd` is made for
  pictures of more lines than a PlayStation 2 game's and costs the most (about 3.6 ms at 4K).
