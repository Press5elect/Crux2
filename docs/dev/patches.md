# The patches Crux2 carries

Crux2 keeps no copy of PCSX2, Mesa, paraLLEl-GS or PS2Recomp. Each is fetched at a pinned revision
and Crux2's changes are laid over it from a patch in this repository. Pins:
[`building.md`](building.md#pins-and-the-source-check).

## PCSX2: `ps5/patches/pcsx2/ps5.patch`

One patch (about 124 files) with every change to PCSX2: the build, the Vulkan loader and display
surface (`GSDeviceVK_PS5.cpp`: looks, frame generation, FSR, HDR and debanding passes), files,
controller defaults, 16 KiB pages, the native-module hook in `iR5900.cpp`, the disc thread, the
soft-float (accurate floating point) recompiler paths, and more. PS5-only code is behind
`__PROSPERO__`. Crux2's changes are Copyright (C) 2026 Press5elect under PCSX2's GPL-3.0-or-later
with the attribution term of `NOTICE`.

### The loop

```bash
ps5/tools/fetch-pcsx2.sh            # vendor/pcsx2 = PCSX2 at the pin + ps5.patch (does nothing if present)
# ... edit vendor/pcsx2 ...
ps5/tools/fetch-pcsx2.sh --export   # vendor/pcsx2's changes against the pin -> ps5.patch
```

`vendor/` is gitignored: an edit that is not exported is lost, and another worker's export can
overwrite yours if you both edited `vendor/pcsx2` from different states. Export once, from the one
tree everyone edits.

### Checking the patch

After an export, check that the patch applies to a fresh checkout at the pin (this is what CI and
every other machine do):

```bash
rev=$(sed -n 's/^revision=//p' ps5/tools/fetch-pcsx2.sh)
tmp=$(mktemp -d) && git -C "$tmp" init -q && git -C "$tmp" fetch -q --depth 1 https://github.com/PCSX2/pcsx2 "$rev" &&
    git -C "$tmp" checkout -q FETCH_HEAD && git -C "$tmp" apply --check --whitespace=nowarn "$PWD/ps5/patches/pcsx2/ps5.patch" &&
    echo "ps5.patch applies to $rev"
```

### Decisions on record

- PCSX2 stays a pinned revision plus one patch (one file to read for everything the port changed).
  The README of v1.0.18 suggested turning the patch into a fork before publishing; that was not
  done, and the patch remains the form.
- Nothing from the patch is offered to PCSX2 ([`workflow.md`](workflow.md#the-rules), rule 2).

## Mesa (RADV): `ps5/patches/mesa-27/`

Applied in order to PS5 Mesa at the revision PS5_Vulkan pins, by `ps5/tools/build-radv-crux2.sh`
(its default folder). [`ps5/patches/mesa-27/README.md`](../../ps5/patches/mesa-27/README.md) says
what each patch does and where it comes from. In brief:

| Patch | What |
| --- | --- |
| `0001`-`0011` | Crux2's and PS5CEMU-HAR's driver fixes: start-up breadcrumbs, threaded recording, 120 Hz settle, timed waits, freed buffers waiting for the GPU, wait spin/sleep, queue depth |
| `0012` | Mesa main (27.0-devel, `0618bfe0ffb`, 2026-10-31) merged into the PS5 RADV (about 59 MB) |
| `0013` | The shader cache's index without MAP_FIXED |
| `0014` | GFX10.1 hardware workarounds for the console's GPU (`RADV_PS5_GFX101_WORKAROUNDS=0` turns them off) |
| `0015`-`0017` | VRR: 119.88 Hz on a VRR display, variable refresh on request, found at run time |
| `0020` | HDR10 framebuffers on request (from OGBox; needs `param.json`'s attribute `0x20000000`) |

`ps5/patches/mesa/` is the older series on Mesa 26.2.5 ([its README](../../ps5/patches/mesa/README.md));
it is used only with `CRUX2_MESA_PATCHES=ps5/patches/mesa`.

After any change here, rebuild RADV (`build-radv-crux2.sh`) and say in the commit that it was
rebuilt; a build links whatever archive is in PS5_Vulkan's `.deps/native/radv-crux2`. A patch that
changes `0012` is regenerated as a binary-safe `git diff --binary` (its README has the recipe).

## paraLLEl-GS and PS2Recomp: `crux2recomp/`

The second GS renderer and the native-module runtime come from Crux2 Recomp's sources.
[`crux2recomp/README.md`](../../crux2recomp/README.md): `fetch-parallel-gs.sh` and
`fetch-ps2recomp.sh` fetch them at their pins into `crux2recomp/vendor/` and apply
`crux2recomp/ps5/patches/parallel-gs/` and `.../ps2recomp/` (def-jam-vendetta-recomp's work and
Crux2 Recomp's). The recomp kit's own patch to PS2Recomp is `ps5/recomp/patches/ps2recomp-crux2.patch`.

## Dependencies: `ps5/patches/deps/`

`libpng-1.6.56-apng.patch` (APNG for libpng, applied by `build-deps.sh`).
