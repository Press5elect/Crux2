# The paraLLEl-GS renderer's sources (from Crux2 Recomp)

Crux2's second GS renderer, paraLLEl-GS, is built from the same sources as Crux2 Recomp's. This
folder holds what Crux2's build takes from Crux2 Recomp, so that this repository is the whole
source of Crux2:

- `ps5/tools/fetch-parallel-gs.sh` fetches paraLLEl-GS (Arntzen Software, LGPL-3.0-or-later), the
  parts of Granite it is built on (MIT) and volk (MIT) at their pinned revisions into
  `vendor/parallel-gs`, and applies `ps5/patches/parallel-gs/`: def-jam-vendetta-recomp's changes
  (jcsix694, GPL-3.0) and Crux2 Recomp's.
- `ps5/tools/fetch-ps2recomp.sh` fetches PS2Recomp's runtime (ran-j/PS2Recomp, GPL-3.0) at its
  pinned revision into `vendor/PS2Recomp`, and applies `ps5/patches/ps2recomp/`:
  def-jam-vendetta-recomp's runtime work and Crux2 Recomp's changes (the renderer's PS5 build,
  save states, GIF path resets, pictures read back while the next frame is drawn).

`ps5/tools/build-parallel-gs.sh` uses this folder when there is no `PS5_Crux2Recomp` checkout
beside Crux2's (or `PS5_CRUX2RECOMP_DIR` names another). Copyright (C) 2026 Press5elect, under the
licences of the files the patches change; keep the credit "Crux2 by Press5elect" (`NOTICE`).
