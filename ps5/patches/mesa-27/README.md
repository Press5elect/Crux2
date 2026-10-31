# The changes Crux2's RADV carries, on Mesa main (27.0-devel)

`CRUX2_MESA_PATCHES=ps5/patches/mesa-27 ps5/tools/build-radv-crux2.sh` applies these, in order, to
PS5 Mesa at the revision PS5_Vulkan pins, and builds the archive Crux2 links
(`.deps/native/radv-crux2` in PS5_Vulkan). Without the variable the script uses `ps5/patches/mesa`
(Mesa 26.2.5), as before. Applied to the pin, they give the tree of branch `crux2-radv-27` in
PS5_Mesa (its merge commit `7d698f21d65`, on branch `crux2-radv-27b`; the merge of 2026-10-23, `ebe30d18aca`, is on `crux2-radv-27`).

| Patch | From | What it does |
| --- | --- | --- |
| `0001` to `0011` | Crux2, PS5CEMU-HAR | The same files as in `ps5/patches/mesa` (see its README): they apply to the pin before the merge |
| `0012-mesa-27.0-devel` | [Mesa](https://gitlab.freedesktop.org/mesa/mesa) main at `0618bfe0ffb` (2026-10-31, 27.0.0-devel; MIT), merged by Crux2 | Mesa's development branch merged into the PS5 RADV, in place of the 26.2.4/26.2.5 merge (which it contains). Files the PS5 port never changed are main's as they are; the PS5 changes are merged onto main with both kept. The PS5 winsys follows main's interfaces: buffers are zeroed when created (main dropped `RADEON_FLAG_ZERO_VRAM` and relies on the kernel clearing memory), CP DMA headers are tracked per chunk, and emulated task shaders stay enabled. The merge commit's message lists every conflict and its resolution |
| `0013-disk-cache-index-without-map-fixed` | Crux2 | As in `ps5/patches/mesa`: the shader cache's index is mapped where the system chooses when a reserved range (MAP_FIXED) is refused |
| `0014-radv-ps5-gfx101-hardware-workarounds` | Crux2 | The GPU keeps GC 10.1.3's shader core and fixed-function bugs under its GFX10.3 identity, so Mesa's GFX10-only workarounds are applied to it too: ACO's GFX10.1 hazard mitigations (VMEMtoScalarWrite, VcmpxPermlane, VcmpxExecWAR, SMEMtoVectorWrite, LdsBranchVmemWAR, NSAToVMEM waits, the 0x3f branch fix, NSA at most 5 addresses and out of hard clauses, no negative unaligned scratch offsets, no `s_inst_prefetch`, the NGG barrier before GS_ALLOC_REQ) through a new `ac_compiler_info.has_gfx101_hazards`, GFX10's late-alloc limits (LATE_ALLOC_GS at most 64, CU2-CU3 kept out), and the bug flags `has_ngg_fully_culled_bug`, `has_htile_stencil_mipmap_bug`, `has_zero_index_buffer_bug` and `has_two_planes_iterate256_bug`. Instruction selection and register programming stay GFX10.3's. `RADV_PS5_GFX101_WORKAROUNDS=0` turns them off (on by default); each device logs `radv/ps5: GFX10.1 hardware workarounds on/off`. The memory clock is reported as 875 MHz, 448 GB/s (it was 896) |
| `0015-wsi-videoout-119.88-Hz-on-a-VRR-display` | PSFlyCast (Press5elect), as it is | After the high-frame-rate mode is taken, a VRR display with nothing flipped yet refreshes at the 48 Hz floor of its window (about 20.8 ms); the settle check took that for a display still at 60 Hz and restored 59.94 Hz. A period of 19.5-22.5 ms is now 119.88 Hz on a VRR display, and `PS5_VIDEOOUT_59HZ` keeps a title at 59.94 Hz whatever `param.json` declares. Each outcome is logged (`wsi/videoout: 119.88 Hz selected ...`) |
| `0016-wsi-videoout-variable-refresh-on-request` | PSFlyCast (Press5elect), as it is | With `PS5_VIDEOOUT_VRR=1` before the first Vulkan call, the output is asked to leave its fixed rate once 119.88 Hz is configured; the variable then reads `on` or `refused`, and a refusal configures the mode again as it was |

`0012` is a binary-safe `git diff --binary` from the pin with `0001`-`0011` to the merged tree
without `0013`, about 59 MB. It is plain text that `git apply --index` takes; the build script
commits it as `Crux2: 0012-mesa-27.0-devel`.

What main brings for the console's GPU (GFX10.3, NGG) since 26.2, among about 620 RADV, 60 ACO
and 200 common AMD commits: cheaper draw calls (vertex, task/mesh and compute user SGPRs emitted
before the barrier, PS inputs and VGT state precomputed per shader, per-draw helpers and user SGPR
lookups inlined, depth bias and line stipple registers emitted only when needed); fewer cache
flushes and waits (queries no longer wait per query nor invalidate L2/VMEM after writes,
ACQUIRE_MEM in ME, L2 metadata flushed only when dirty, CS_PARTIAL_FLUSH dropped with CB/DB
timestamp events on GFX10+); HTILE fixes (HiZ range reset after decompression, layer ranges,
fills instead of read-modify-write shaders, VMEM invalidation before HTILE passes, Z copy fixups)
and DCC fixes (cleared-image compression state, decompression order in copies); mrtz exported in
the PS epilog; the SMEM cache line size used for uploads; and addrlib 10.9.

Mesa's licences apply to all of them (the PS5 winsys is MIT, like the code around it;
PS5CEMU-HAR's patches are published under GPL-3.0). Crux2's own changes: Crux2 by Press5elect.
