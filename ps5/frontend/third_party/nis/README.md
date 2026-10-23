# NVIDIA Image Scaling (NIS)

`PS5ComputeFX::NIS` (`ps5/frontend/ps5_compute_fx.h`) scales a picture by 1x to 2x on each axis and
sharpens it with **NVIDIA Image Scaling v1.0.3**: a 6-tap, 64-phase scaler with edge-directed
filters and an adaptive unsharp mask, or the sharpener alone at 1x. Thanks to NVIDIA for
publishing it under the MIT license.

| Files | Source | License |
|---|---|---|
| `NIS_Scaler.h`, `NIS_Config.h`, `licence.txt` | <https://github.com/NVIDIAGameWorks/NVIDIAImageScaling>, tag `v1.0.3`, commit `35e13ba316c98eeecf16f37eae70ce88019911f6` (2022-08-22), `NIS/` and the root | MIT (`licence.txt`) |
| `crux2/nis_main.comp` | Crux2's own, after NVIDIA's `NIS/NIS_Main.glsl` | GPL-3.0+ |

v1.0.3 is the newest release (the repository has not changed since).

`NIS_Config.h` is unchanged: `ps5_compute_fx.cpp` uses its `NVScalerUpdateConfig` and
`NVSharpenUpdateConfig` for the constants and its `coef_scale` and `coef_usm` tables for the two
coefficient textures (2x64 RGBA32F, filled through a buffer by the first record).

`NIS_Scaler.h` has one change of Crux2's, marked `Crux2:` in it: its GLSL `NVTEX_STORE` is
wrapped in `#ifndef NVTEX_STORE`, so `nis_main.comp` can give its own. NIS writes whole blocks of
32x24 (or 32x32) pixels whether or not the output ends inside one, and its own viewport check
(`NIS_VIEWPORT_SUPPORT`) lets a store one past the end through; a store outside an image is
undefined in Vulkan without `robustImageAccess`, and the image may be larger than the picture.
Crux2's store is made only inside the output.

`nis_main.comp` differs from NVIDIA's main in two more ways: the constants are push constants
(the 28 values of `NISConfig`) instead of a uniform buffer, and the output image has a format
(RGBA8 or RGBA16F; a variant without one needs `shaderStorageImageWriteWithoutFormat`, which
PCSX2 does not enable). Block sizes are NVIDIA's for AMD hardware (`NISOptimizer`, `AMD_Generic`).
The half precision scaler (`NIS_USE_HALF_PRECISION`) is used when the device was created with
`shaderFloat16`.
