# AMD FidelityFX: FSR 3 optical flow (and frame interpolation, for comparison)

Crux2's second frame generation (`ps5/frontend/ps5_compute_fx.h`, `OpticalFlowFG`) finds the
motion between two pictures with AMD's **FidelityFX FSR 3 optical flow**, the part of FSR 3
frame generation that needs nothing from the game but its pictures. Thanks to AMD for the
FidelityFX SDK and for publishing it under the MIT license.

## Where it comes from

| Files | Source | License |
|---|---|---|
| `gpu/ffx_common_types.h`, `gpu/ffx_core.h`, `gpu/ffx_core_gpu_common.h`, `gpu/ffx_core_gpu_common_half.h`, `gpu/ffx_core_portability.h` | FidelityFX SDK **2.3.0**, `Kits/FidelityFX/api/internal/gpu/` | MIT |
| `gpu/opticalflow/*.h` (all but the HLSL callbacks) | SDK 2.3.0, `Kits/FidelityFX/framegeneration/fsr3/include/gpu/opticalflow/` | MIT |
| `gpu/spd/ffx_spd.h` | SDK 2.3.0, `Kits/FidelityFX/framegeneration/fsr3/include/gpu/spd/` | MIT |
| `gpu/frameinterpolation/*.h` (all but the callbacks and the debug view) | SDK 2.3.0, `Kits/FidelityFX/framegeneration/fsr3/include/gpu/frameinterpolation/` | MIT |
| `gpu/ffx_core_glsl.h`, `gpu/frameinterpolation/ffx_frameinterpolation_callbacks_glsl.h` | FidelityFX SDK **1.1.4**, `sdk/include/FidelityFX/gpu/` | MIT |
| `crux2/*` | Crux2's own | GPL-3.0+ (the callbacks also keep AMD's MIT notice: they are derived from the SDK's) |

- SDK 2.3.0: <https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK>, tag `v2.3.0`,
  commit `60f4ea81909200d8542eca14dccb2628b763a9a3` (2026-06-24). The newest release; its
  `docs/license.md` lists every file used here among those "subject to the following license"
  (MIT); the rest of that file (binary-only terms) is about its signed DLLs, none of which is used.
- SDK 1.1.4: same repository, tag `v1.1.4`, commit `c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`.
  SDK 2.x ships for DirectX 12 only and has no GLSL; 1.1.4 is the last with the Vulkan backend's
  GLSL core header and callbacks. The pass headers are byte for byte the same in 1.1.4 and 2.3.0
  (but for the copyright year), so its GLSL glue fits the newest passes.

All the files under `gpu/` are unchanged. `LICENSE.txt` has both license texts.

## What Crux2 runs (and how it differs from the SDK)

`crux2/of_passes.comp` is the seven optical flow passes (prepare luma, luma pyramid by the single
pass downsampler, scene change histogram, scene change divergence, search, filter, scale), one
entry point per pass as the SDK's GLSL ones, plus a small pass of Crux2's (the two pictures'
mean brightness). `ps5_compute_fx.cpp` runs them as AMD's `ffx_opticalflow.cpp` does for a frame:
same order, sizes and dispatch counts; levels coarsest first; each way (the newer picture's blocks
in the older, the older's in the newer).

`crux2/ffx_opticalflow_callbacks_crux2.h` replaces the SDK's callbacks. The differences, each
explained in the file:

- constants are push constants;
- loads clamp to the picture and stores outside it are dropped (the SDK relies on Direct3D's
  out-of-bounds rules);
- the flow can run on a smaller luma picture than the game's (the quality setting);
- the scene change detection compares the two pictures it is given; its race (some workgroups of
  the divergence pass overwrite the previous histogram and clear the histogram while others still
  read them) is gone: the previous histogram is read from one image and written to another, and the
  host clears the histogram;
- the search and scale passes do not zero the vectors on a cut (Crux2's verdict pass decides);
- the second picture's luma is scaled to the first one's brightness before the search compares
  them (the SDK compares raw luma, and a fade then matches nowhere);
- the search's wave reductions work for any subgroup size;
- storage formats are ones every device has (R32_UINT luma, RGBA16_SINT vectors) because PCSX2
  does not enable `shaderStorageImageExtendedFormats`.

`crux2/fg_warp.comp` is Crux2's own: the camera's move (a translation, or a zoom and turn, voted on
by 256 blocks), each block's vector made sure of (against the camera's move and the vectors of
blocks 1-8 blocks away, which fixes what the block matcher gets wrong on repeating patterns and at
the edges of moving things) and refined to a fraction of a pixel, the cut verdict, and the picture
at time t.

## Why not FSR 3's own frame interpolation

FSR 3's frame interpolation passes (`gpu/frameinterpolation`) were run on the same flow for
comparison (`ps5/tests/computefx/fsr3_fi_route.h`, `--fsr3-fi`). Fed what a PS2 game can give
(no motion vectors, no depth: zero and a constant) they make little more than the two pictures
mixed: FSR 3 trusts the game's motion vectors and uses the optical flow where those fail, and with
zero vectors that is everywhere. PSNR at the middle, 720p, against the truth (dB):

| scene | Crux2 composite | FSR 3 FI, zero vectors | FSR 3 FI, the flow as game vectors | the two mixed |
|---|---|---|---|---|
| camera turn 48 px | 99 (exact) | 19.0 | 68.1 | 17.6 |
| camera turn 140 px | 99 (exact) | 21.1 | 93.5 | 18.9 |
| moving in 4% | 42.4 | 24.1 | 40.1 | 21.9 |
| disc over a still picture | 37.1 | 31.5 | 37.4 | 29.7 |
| disc one way, camera the other | 45.7 | 19.5 | 44.7 | 18.1 |
| camera following a ball | 38.2 | 23.3 | 35.5 | 20.1 |
| turn while fading | 42.5 | 24.4 | 46.4 | 21.8 |
| turn of 25.4 x 3.3 px | 39.9 | 21.8 | 41.4 | 20.8 |

Given Crux2's refined vectors as its game vectors, FSR 3's interpolation is about as good as
Crux2's own composite (better on the fade and the fractional turn, worse on the zoom and the ball),
but it only makes the middle picture (t = 0.5), takes ten more passes at full size, and needs
storage formats and unformatted image loads PCSX2 does not enable. Crux2's composite is one pass
and takes any t. So the title carries the optical flow and not the frame interpolation; the
frame interpolation headers are here for that comparison only (the tests compile them).
