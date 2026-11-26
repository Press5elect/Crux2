# Open work

What is known to be unfinished, untested on a console, or deliberately left. Update it when an
item is done or something new is learned; delete what no longer applies.

## State of the branch

- The branch was reverted to the v1.0.18 release tree in 883c832 (the owner asked to go back to
  the release on GitHub). The v1.0.19/v1.0.20 work is in history before it:
  [`../history/v1.0.19-v1.0.20-reverted.md`](../history/v1.0.19-v1.0.20-reverted.md). Do not assume
  anything from it is in the tree.
- b075fe1 and the work after it are Unreleased ([`CHANGELOG.md`](../../CHANGELOG.md)). **Nothing of
  it has run on a console**; the host tests and the interface crawl pass.
- The pins were moved on 2026-11-15 (PCSX2 master after v2.9.179, PS5_Vulkan
  `7f492b4`, the SDK `3345cde` with upstream v0.45's C++ runtime, PS2Recomp `e563698`); `check-sources.sh`
  reports every source current. None of it has run on a console. Save states from before don't load.
- The settings' revamp, phase 3 (bp_settings.cpp, bp_settings_home.cpp, bp_settings_words.cpp; the
  words of settings-text.yaml applied): checked on the PC only (the crawl, the shots, the inventory).
  Not run on a console. To check there: every area and section opens and its rows behave as before
  (lists, ranges, bindings and their capture, actions, the network share being added, the memory
  cards and their manager, the log, achievements' sign-in); a game's own settings from the library
  and the quick menu (Game options, Patches and cheats), L3 (Use Global Setting, clear a binding),
  the touch pad's switch to all games and back, and that what is changed there is written to the
  game's file and to PCSX2.ini; the texture-pack manager's "Texture settings of the game"; Restart
  and NextStart rows (Takes effect) and the quick menu's "Apply changes now"; pins and Changed lately
  kept across starts ([PS5UI] Pins, RecentChanges); the search's keyboard (a USB or the phone's one
  too); readability at Text size Largest and Interface size 70%; the skins (light ones: Brutal,
  Clay, Gloss) on the new layout; how fast the home is drawn (what is changed is looked at again every
  half second: about 700 values, twice; see phase 5 for the PC's figure). The README names settings
  by their area, section and PCSX2-style labels since phase 5.
- The interface's speed (`run-bigpicture-tests.sh <folder> bench`, [`testing.md`](testing.md)):
  **PC figures only, nothing profiled on a console.** On the PC a frame of any screen is 25 to 200
  microseconds of the interface's own code (4,000 to 22,000 vertices), with 33 games or 3,033: the
  frames are not where time goes. The changes of screen are: opening the quick menu with pins asked
  the Backend for its game list three times and for 64 pages of rows (0 and 19 now), the Settings
  for the game list once (0 now). To check on a console: that the quick menu, the Settings and a
  game's settings (Square on Home, a game's page) open quicker than before with a large library;
  the Statistics page, the settings' search for games, the card manager's game names and the texture
  pack manager's after a scan, a rename or a favourite (they read the library's list now, which is
  asked for again only when `gamesGeneration()` moves); pinned settings in the quick menu (made once
  as it opens, the rows of all games only when a pin is not one of the game's own); long titles cut
  with "..." in every view. Left as it is: a game's page still opens the game's settings once to
  read them and Game Specific Settings opens them again (one `openGame` each; a Backend that kept the last
  game's patches would save the second); the Settings look at what is changed every half second
  (about 600 values read); the library is drawn under an open game page; the first frame of a
  screen with text of a new size makes its glyphs (FreeType) once.
- The interface revamp, phase 4A (bp_home.cpp; the top bar, the library's filter row and strip in
  bp_library.cpp): checked on the PC only (crawl, shots). Not run on a console. To check there: Home
  after the start-up, its Resume (the newest state: the one made at closing or a slot) and Game page;
  "Right now" with a real texture pack download, a disc in the drive and a share that answers or not;
  L1 / R1 through Home, Library and Disc, Options to the settings and back to the same tab, the
  "Right now" card opening the texture pack manager during a download; the filter row's All (every place
  at once) with USB drives and a share, and its Favourites / Never played / Collections chips; the
  strip's save state count (Backend::states for the focused game, once per game); choosing a disc
  for a running game (the filter row's places, L1 / R1); how fast Home is drawn with a few thousand
  games (since phase 5 its lists and counts are made once per change of the library, not every frame).
- The interface revamp, phase 4B (a game's page in bp_library.cpp, the quick menu in bp_quick.cpp):
  checked on the PC only (crawl, shots). Not run on a console. To check there: how long a game's page
  takes to open (it opens the game's own settings once to count them, its patches, cheats and memory
  card; texture packs from the manager's list); the Overview's description, which now asks
  PS5About (Wikipedia, [PS5] FetchAbout) for every game whose page is opened, not only the disc in
  the drive; the quick menu's Screenshot tile (PS5Shots::Take while the game is paused under the
  menu: is the picture the game's, and is the menu not in it); pinned settings changed in the quick
  menu (written to the running game's own settings where a game can have one, as the Settings do;
  Apply changes now for those the game takes at its start); how fast the quick menu is drawn with
  several settings pinned (their rows are made once when it opens, from every page).
- The interface revamp, phase 5 (polish): checked on the PC only (crawl, shots, inventory, host
  tests). Not run on a console. What changed: one order of the hints on every screen (bp_draw.cpp
  hintBar/orderHints); messages (toasts) in the top right corner over the clock; the settings' home
  cards' two summary lines; corner radii no longer multiplied twice by a skin's roundness; the cards'
  hairline edge (light skins, Contrast); entrances 0.2 s (0.14 s and fades only with Motion: Less);
  Home's lists and counts and the settings' id lookups cached; Change disc offers the drive's disc
  again (a Disc chip, L1 / R1); L3 on a game's own pinned value in the quick menu. On the PC the
  changed-from-default scan took 75 microseconds on average (470 at most) for 392 rows, every half
  second. To check there: readability of the toasts over the clock and over a running game; that a
  toast never hides something one needs (the Disc tab's R1 glyph sits close to it with three tabs);
  the skin sheets (shots 71-*) against the television; Change disc from an image to the drive's disc
  and back, and from the drive to an image; L3 in the quick menu on a game's own value.
- USB controllers (DualShock 3, Xbox 360 pads, guitars and drums): never tried on a console.
- VRR has not been tried on a TV that has it.
- Surround: which console channel each ends up in has not been heard.
- Online play through the network adapter's DNS presets (PS2 Online, PS Rewired) has not been tried.
- The overlay's EE/GS/VU percentages are each thread's own report; whether a title may read
  another thread's processor time is not measured.
- The performance overlay's sizes ([PS5] OsdSize; ps5_osd.cpp, ps5_osd_draw.h, ps5_osd_logic.h with
  its host test `tests/osd/osd_logic_test.cpp`; pictured in the interface's shots 49c2-49c7). Not run
  on a console. To check: readability on a television from the sofa at OSD scale 100% (the design's
  pixels are a 1080-line screen's times the OSD scale: on the 2160-line output the overlay is twice
  as large in pixels as on a 1080-line one, and larger than the v1.0.2 panel was); Minimal, Compact
  and Full in each corner and top centre, with "Move the figures from corner to corner"; amber and red
  where a game is slow; "30 → 60" with frame generation; the card's footer (the kernel's temperature,
  the memory figure); a settings file from before it gets Custom when its switches match no size.
  Cost: the overlay is made 10 times a second and drawn every frame (about 150 thin bars and a few
  dozen text runs for Full); its GPU and GS-thread time is not measured.
- Not every setting has been tried one by one; paraLLEl-GS not in every game; native code modules
  and many PS2 accessories not tried. PGO's worth has not been measured before/after.
- Screen care: how fast a panel ages is not measured.
- Looks' black screen (GSDeviceVK_PS5.cpp, DoLook): the look's picture is now left in the shader
  read layout and PCSX2's vertex buffer bound again after the look's passes (the two causes the
  reverted eca7923/d01f2cd found). Not run on a console. To check: a Look (e.g. CRT) shows the game,
  the menus over it are drawn; the boot log has `look: frame 1 made, WxH from WxH`.
- Downsampling ([PS5] Downsample) and SMAA ([PS5] SMAA), both off by default (ps5_aa.h, ps5_aa.glsl,
  GSDevice::PS5Antialias, GSDeviceVK_PS5.cpp's DoDownsample/DoSMAA). Checked on lavapipe only
  (`tests/aa`). On a console, check: with Upscaling Off or CAS and 8x at 4K, Soft and Sharp show a
  picture with less shimmer than Bilinear, nothing shifted or cropped, the menus fine; the boot log's
  `picture: downsampling's passes made`, `picture: downsampled (Sharp) from ... to ...`, `picture:
  SMAA's passes made`, `picture: SMAA runs at WxH`; a paused game (menu open) looks the same as
  running; with frame generation and a look on top; HDR and debanding after it. GPU cost: the test
  window's `downsampling and SMAA` figure in the boot log's GPU line (and the overlay's conversions).
  Not measured: the cost at 8x-12x (Sharp's two passes reach up to 33 texels each at a ratio of 8).
- The HD texture pack manager (bp_textures.cpp, ps5_texture_pack_manager.cpp; the file work in
  ps5_texture_pack_store.cpp, host-tested in `tests/texturepacks/store_test.cpp`; the screen in the
  interface crawl). Not run on a console. To check: the manager lists packs on the console, a USB
  drive (elevated) and a share, with sizes (`texture packs: ... measured in` lines; how long 100k
  files take on a drive and over SMB); a move console to USB and back, and to a share (the share's
  copy is written with `ps5vfs_store_from`, file by file: its speed), stopped half-way (the source
  as it was, `replacements.crux2-moving` beside the target) and begun again; a delete stopped and
  finished; two downloads queued one after the other; the bar during a move. Free space: the
  console gives a title none (statvfs is a stand-in), so the "may not fit" question never comes on
  the console; a pack too large stops with the drive full. Whether rename() works on an exFAT USB
  drive from the elevated title (the move's last step there) is not known.
- PS Move: whether the console's own libSceMove gives a payload process Move data is unknown; the
  probe in [`../../ps5/probes/move/`](../../ps5/probes/move/README.md) (built, never run) calls it with
  the PS4's signatures and logs to `/data/crux2-move-probe.log`. Nothing in the title uses it yet.

## To do (the owner's list)

- **Restore the sandbox helper's reference-count fix** (`fd0a129`, 2026-11-05, from OGBox 0.0.2).
  The helper for USB drives (`ps5/elevation/payload/main.cpp`) writes the console's root vnode into
  the title's `fd_rdir` and `fd_jdir` without references: every elevated run's exit releases two it
  never took, and after many exits with USB drives on the console can go down. The fix was undone
  by the revert to v1.0.18 (`883c832`) and is in neither v1.0.19 nor v1.0.20. It still applies
  cleanly (`git diff fd0a129^ fd0a129 | git apply --check`): three files, the helper, its
  `protocol.hpp` (protocol version 2, `ok_unbalanced`) and `ps5_platform.cpp`; keep v1.0.19's
  "asked again for a few seconds" retry in `ps5_platform.cpp`. Kernel-side code that cannot be
  tested on a PC: a test build for the owner before any release.
- The profiler's sync-wait statistics (`b51d391`: `ps5_sync_stats.h`, hooks in PCSX2's `MTGS.cpp`,
  `MTVU.cpp` and `Memory.cpp`, about 94 lines) were not brought back either; the owner left them
  out on 2026-11-16.

## The frontend review of 2026-11-16: fixed, to check on a console

A read-only review of the interface and the host code it drives (at `88e6acd`) found about fifty
bugs; the list with files and lines is in this file's history (`b580cc7`). All of them are fixed
in the commit after it. Checked on the PC only: the host tests, the interface's crawl with new
checks, the texture pack store's, the backup's and the shares' tests, and a compile for the PS5.
**None of it has run on a console.** To check there:

- The quick menu's Screenshot tile during a game; a texture pack download, the card manager's save
  search and "Find in" ending during a game (their results now go by the emulation thread).
- A pack moved to a real share: refused when one is there; stopped, gone on with, deleted
  (`replacements.crux2-copying` marks an unfinished copy; does the share take an empty folder?).
- Memory-card writes, card sync and a restore on an exFAT USB drive (the old file is kept as
  `<name>.old` until the new one is in place); a restore with a game started in its middle.
- Offline at start: covers and pictures say "no connection" and `no-cover.txt` is left alone;
  the title killed during a cover download: one message at the next start, then downloads again.
  Covers with two downloaders (up to six connections). "Share covers": pictures only.
- Controllers: "Map to Console Controller" on ports 2 and up with a multitap (the focus stays);
  a pin of a port 2 setting; with fewer controllers than ports the first one is mapped.
- A game's own SingStar or light gun choice put back to the global one (L3): the game follows
  the global choice at its start.
- Settings: "Internal resolution above 12x" at 6x (Left stays, Right 13x); the search's
  "Changed from Default" after putting a row back; Thread Pinning reads "Dedicated Cores
  (Default)" and is not marked changed on a new install.
- The library: quick views kept in Wall or Discs after a restart; a disc set's last played, time
  and favourite; More > The next disc in Shelves; the Disc tab's More (no Rename, Favourite, Hide);
  the game page after the texture pack or card manager closes.
- The keyboard: Delete after an accented letter; a full field; logs and What's New at Text size
  Largest; the managers at Interface size 70%; long messages scrolled with Up and Down.

Not reviewed: `ps5_covers_match.h`'s matching, `ps5_aim_logic.h`, `ps5_input_source_logic.h`,
`ps5_texture_packs_extract.cpp`, `ps5_services.cpp`, `ps5_card_sync.*`, `ps5_osd_draw.h`'s layout.
Known and left: a cover run where every answer was a 403 or 5xx still says "no connection".

## The graphics thread at 100%: texture creation (measured 2026-11-16, in progress)

A console profile (SCES-53247, 6x, blending basic, the threaded driver on; `profile all`) showed the
graphics thread on a processor 100% of the time with **47% of it under `GSDevice::FetchSurface` →
`GSDeviceVK::CreateSurface` → VMA `AllocateDedicatedMemory` → `radv_ps5_buffer_create`**: 28% in
the kernel's mapping calls and 18.7% in `radv_ps5_cpu_flush` (a `clflushopt` per line of a ~41 MB
target). The same share shows in light scenes (49% of a thread 55% busy), so it is a cost of every
frame, about 8 ms. The GIF and vertex handlers are about 20%; the recorder thread is 30% busy.

- **Measured with both switches on (the owner, 2026-11-16, SLUS-21376, 6x):** the thread is on a
  processor 30-60% of the time (100% before), the driver's share of it 1-3% (20%), nothing under
  `CreateSurface` among the 40 functions on the stack, and 38% of the busy window is the wait for
  the next image at present. The owner reports about 50% more speed. The surfaces still created
  are small textures from `GSTextureCache::CreateSource` (+0xd97e6: 50 to 200 a second) and a few
  from `Target::Create`, `Target::Update` and `Palette::Palette`. A run of the first game with
  both switches off (the caller of the large targets there) was not made.
- Why it costs: `GSTextureVK.cpp:243` asks for a dedicated allocation for every target of 512x448
  and more; the winsys zeroes each new buffer and flushes it (`radv_ps5_bo.c:136`); the pool
  (`GSDevice.cpp:685`) reuses only an exact size and format.
- **Not known yet: who creates a target every frame.** The profile's stack list stopped one line
  short. The build after this note logs `graphics: N surfaces created in S s (M MB): target WxH
  format F xCOUNT from +offset; ...` (offsets from the program's base, in `symbols.txt`) and lists
  40 functions on the stack. Candidates: `GSRendererHW.cpp:8913` (the draw's copy),
  `GSTextureCache.cpp:6639`, `:6329`, `GSRendererHW.cpp:4386`, `:5127`, `:10879`.
- The two switches, on by default since `6ce03c1`'s measurements (the owner's decision, 2026-11-16;
  the change of defaults has not been built or tested yet): `[EmuCore/GS] PS5SharedTargetMemory` (no dedicated
  allocation, VMA blocks of 512 MiB) and `[PS5] StreamZero` (`RADV_PS5_STREAM_ZERO=1`, Mesa patch
  0021: non-temporal zeroing in place of memset and the flush).
- To do after the caller is known: keep or round that caller's texture so the pool finds it. A
  follow-up in the driver: no zeroing at all for application memory that did not ask for it.
- Smaller candidates from the same study (each about 1% or less here): `Target::Update` unswizzles
  twice (`GSTextureCache.cpp:8290`); `GSGetRGBA8AlphaMinMax` and the CLUT loops with 256-bit
  vectors; adjacent blocks in one XXH3 update (`:9327`); the descriptor builder made only when
  textures changed (`GSDeviceVK.cpp:6430`); the last pipeline remembered (`:5718`).
- The menus: the thread is 55% busy without a game; the present path's fence waits spin for
  `GpuWaitSpinUs` (1500 µs) at 120 Hz. Not settled.

## Performance work of 2026-11-21: built, to measure on a console

Nothing of this was run on a console. Full report: the session's `crux2-performance-implementation-2026-11-21.md`.

- **PGO**: the changes alter the control flow of profiled hot functions (`recClearIOP`, `rcntUpdate`,
  `psxRcntSync`, `recMicroVU1::Clear`, `DescriptorSetUpdateBuilder`, `VKStreamBuffer::CommitMemory`,
  `KernelSemaphore::Post`), which then build without their profile. Train a new profile
  (`PS5PX_PGO=generate`, MTVU on and off) before a release, and A/B against v1.0.22 only after it.
- To check: the `clock:` line (if the thread processor clock costs microseconds, try Coarse Thread
  Times); the `counters:` line in a heavy VU1 scene (XGKICK waits, wake-ups that slept, unchanged
  microprogram uploads with Skip Unchanged Microprograms on); the profile's `generated code:` line.
- Audio: the time stretch must sound the same (bit-identical on a PC with the build's compiler).
- Profile: `ps5/pgo/crux2-2026-11-21.profdata` (`104619efcd21`, MTVU on and off, 2026-11-21 runs on
  build `ae9956016f6d9052`) is the base of build B (`f65d7e7bb670863b`); `crux2.profdata` is still
  `f965a89d3a30`. 570 functions the old profile had run were not run in training (paraLLEl-GS,
  texture packs, paletted lookups, the overlay, `CheckOverlapVerts`): cold in B.
- Second batch (on B, needs its own training round, G2 then B2): the GS UV+XYZ2 packed loop, the
  24-bit depth test order, one `temp_draw_rect` store, the inlined block address functions, the
  event test's clock ratio, and **IOP Stores Inline** (off). Not done: microVU JR/JALR jump-cache
  hits in generated code.
- Console 2026-11-21 (profiling build): thread processor time 0.85-0.92 us a call; VU wakes
  100-150/s (Coarse Thread Times not worth it); XGKICK waits almost all under 50 us and spun (the
  spin length cannot help); the VU ring never full.
- PCSX2 master is 10 commits past the pin (GS texture cache target selection, an IOP fix, the
  controller settings rework; four patched files): moved separately from this work.

## GPU work of 2026-11-22: measured, closed

Console logs (B4.4, B4.7, B4.9) show the games below full speed are GPU-bound; the investigation is the
session's `crux2-gpu-bottleneck-investigation-2026-11-22.md`.

- What helps: the internal resolution (6x keeps SLUS-21376, SLUS-21065, SCES-53247 at 98-99%) and the
  ordered view on (the default; SLUS-21376 98% against 91% off, 6x).
- Tried and removed: leaving the ordered view after draws that did not need it. In SLUS-21376 those draws
  alternate one by one with draws that need it (705/705, 962/961 a frame), so a target either leaves and
  comes back each draw (a full-target copy each way: 87-88% speed) or never leaves. Taking one-barrier
  draws into the view and fetching textures before the interlock were removed with it, untested.
- Deinterlacing (SCES-53247, adaptive) costs 0.09 ms a frame at 6x: not worth work.
- Kept in the test windows: pixels shaded by pass kind (with the overlay's GPU statistics off) and the
  names of the merge, deinterlace, post and clear passes. Not done: why render passes end (counting it
  in EndRenderPass voided RenderHW's trained profile).

## Graphics switches read at each game's start (2026-11-24): built, to check on a console

- `GSGameChanged` (`vendor/pcsx2/pcsx2/GS/GS.cpp`) re-reads the PS5 rows of Graphics > Advanced
  (`PS5HoldFeedbackLayout`, `PS5RTNoCompression`, `PS5SkipRedundantBarriers`, `PS5ROVKeepPass`,
  `PS5ReuseRTCopies`) with the game's own settings. At a game's start it is queued while the VM is still
  Initializing, and it required a Running VM: whether it ran depended on how long the GS thread took to
  get to it. In the owner's logs it ran only for SLUS-20228 and SCUS-97399 (long pipeline precompiles),
  never for SLUS-21376, SCES-53247, SLUS-21065, SLUS-21452, SLUS-20965. Now Initializing counts too.
- To check: a game's own `PS5HoldFeedbackLayout` on, and the boot log shows "graphics (this game):
  render targets a draw has read stay in the feedback loop layout" at its start, for SLUS-21376.

## GS thread split (2026-11-24): built, to check on a console

- `[EmuCore/GS] PS5GSSplit` (0 off, 1 check on one thread, 2 two threads, the default since the owner's
  decision of 2026-11-24; GS Thread Split in Graphics > Advanced). B5.2/B5.3 on the console: SLUS-21376 4x
  100% (3 s under 95%, 1% low 59.7) against v1.0.23's 99% (12 s); SLUS-21065 4x 99%; no crash. Design: the session's `crux2-gs-thread-split-design-2026-11-24.md`. Code:
  `vendor/pcsx2/pcsx2/GS/GSSplit.cpp` (queue, DRAW thread, start/stop), GSState's `PS5Split*`/
  `PS5FlushPrimDraw`/`PS5CopyParseState`, GS.cpp and MTGS.cpp routing.
- The FRONT side (GSSplitFront, MTGS thread) reads the GIF packets; draws, uploads, CLUT loads, moves, vsyncs
  and resets go to g_gs_renderer in order. Not used (logged) with CPU/GPU-target CLUT fixes, hardware AA1, a
  game's move handler or GSC_IRem, GS dumping, paraLLEl-GS.
- To check on a console: the picture the same with 1 and 2 as with 0 (screenshots at fixed moments); speed of
  SLUS-21376 (4x and 6x) and SLUS-21065 with 2 against 0; save and load states, resets, settings changes and
  game changes with it on; the boot log's "gs split:" lines (items a frame, DRAW thread busy, waits).
- The training profile does not match FlushPrim, FlushWrite, Write and the MTGS loop any more: compare 0 and 2
  in the same build; retrain once the split is kept.
- The DRAW thread's time is not in the overlay's or the test window's GS figure (only the MTGS thread's).

## Render targets without compression by default (2026-11-26): built, to check on a console

- `[EmuCore/GS] PS5RTNoCompression` defaults to on (the owner's decision: the games played ran better with
  it on). Colour render targets get no DCC, so the whole-image decompress on entering the feedback-loop
  layout never runs; the cost is no fast clears and no DCC bandwidth saving. To check: the same games
  with it off and on at 6x and 8x (GPU time, `FD` in the test windows), and a picture without faults.

## The write broker (2026-11-26): built, untested on a console in Crux2

- `ps5/broker` (the payload, `write-broker.elf`) and the Client are taken from OGBox (db63d46), where
  the owner's write test measured it at about 200 MiB/s past 2 GiB on /data while the title's own
  writes fell to a few MiB/s. Crux2's Session and File (`ps5/frontend/ps5_broker.cpp`) send through it,
  with `[PS5] WriteBroker` on (off by default, rule 7): a texture pack moved onto /data (COPY, the
  broker reads and writes the file itself), a pack unpacked onto /data as it downloads, and a disc
  image made on /data (WRITE, 8 MiB pieces, 4 ahead; the FTP offload is not used then). A failure lets
  the broker end, then the title writes what it didn't confirm and the rest (the PC test kills it
  after 1 to 40 requests: the file is whole).
- To check on a console: a pack of several GB moved from a USB drive to the console, downloaded to
  the console, and a disc image of a DVD, with the setting off and on; the `broker:` lines (writing
  through the broker, the files, MiB and MiB/s at the end, or why not). Whether a file the payload
  creates can be renamed and deleted by the title (OGBox's move needed the same) and whether two
  brokers at once (a download and a move) both start.
- A move whose copy the broker failed part-way counts that file's copied bytes twice in the progress
  bar (the title copies it again from its start): cosmetic.

## Known problems

- PCSX2 `f17c4c8` shows **Depth Feedback Mode** (`EmuCore/GS` `DepthFeedbackMode`: None, Auto, Depth,
  Depth as RT) in its own menu; Crux2's settings have no row for it yet (the core has the setting).

- paraLLEl-GS is slower than PCSX2's renderer in the heaviest game tested, and its picture is
  softer by design.
- The free BIOS (OpenPS2BIOS) is early work: many games reach their menus and go no further.
- RetroAchievements' hardcore mode is not offered; its sounds are not played.
- 50 Hz output does not exist on the console: PAL games alternate refreshes.

## Partial or not ported

- From the reverted v1.0.20 work (LRPS2/ARMSX2 studies), never ported: target widening for a
  constant Z, and LRPS2's native taps for an indexed read of a scaled target. The rest of that work
  is in history (2bf2d08 and before) and would have to be brought back.
- From the reverted v1.0.19 work: Settings > Debug > Logs, network shares in Settings > Folders,
  the boot log writer thread, the safety save in memory, the USB headset, CJK names: all out of
  the tree now (`settings-text.yaml` is back, made again from the code: the interface revamp's
  phase 1, [`settings-inventory.md`](settings-inventory.md)). b075fe1's boot log in memory overlaps with the
  v1.0.19 log settings; reconcile before restoring either.
- The interface revamp: phases 1 to 4 are in (1: the inventory, the no-loss test,
  `settings-text.yaml`; 2: every setting's words, PCSX2's word for word where it has the setting and
  in its voice elsewhere; 3: the settings' home, the eight areas, the search and the performance
  overlay's sizes; 4: Home, the top bar of places, the library's filter row and strip, a game's page,
  the quick menu), and phase 5's polish. All of it is checked on the PC only (crawl, shots,
  inventory); nothing of it has run on a console (the items above say what to look at there). 60
  rows' area or section is a guess (`ps5/tools/settings-text.py doubtful`), for the owner. PCSX2's
  own descriptions keep the games they name (the owner's decision of 2026-11-15). The inventory does
  not see rows of states its fakes do not make (a typed cheat, saved controller profiles, backups on
  the drive, multitap ports, a share being changed).
- Porpoise ports in progress (networking, input/aim, texture-pack memory cap, steady 30,
  colour-blind filter, trophy pop-ups, launch arguments, recommended settings, backups,
  screenshots): see the Unreleased changelog as they land.

## Decisions on record

- PCSX2 stays a pinned revision plus `ps5.patch`; nothing goes upstream.
- Behaviour that cannot be checked without a console starts off by default.
- Release assets are the ZIP and `SHA256SUMS`.
- `PS5PX_PGO_SPLIT` stays off: measured worse on the console (2026-10-12).
- The RSQRT.S negative-zero change was not made: a console capture showed the existing behaviour
  was right.

## Found while integrating the Porpoise ports

- `ps5/tests/ps2ftp/run-ps2ftp-tests.sh` (not part of run-host-tests.sh) fails 4 of 13 checks on the
  PC, the same before and after the networking changes: not looked into.
- Before the Light gun choice, nothing in the interface plugged a GunCon 2 in, and
  `PS5InputSource::SetGunCon2Defaults` had no caller: the light gun worked only through a hand-edited
  ini. The choice now does both; check on the console that a light-gun game sees the gun.
- The ugen code (raw USB) comes from Porpoise's open pull requests (pr36, pr37), not its main branch;
  the notices entry names main's revision.
- The recomp kit's three runner files (PS5_Crux2Recomp's `ps5/runner/`, on its `main` since
  2026-11-15) come from kit v1.6.4, older than the
  runner and Vendetta patches in `crux2recomp/ps5/patches/ps2recomp/` (rebased for PS2Recomp e563698).
  The kit is made and its tools start, but no module has been made with it since: check one game on
  Windows and Linux before a release ships it.
- **Mesa is held at `0618bfe0ffb`** (`ps5/patches/mesa-27/HOLD`, the owner's decision of 2026-11-15).
  The refresh to Mesa main `aead7293cfd` (commit 9ad6b51; its 0012 is 38 MB with deleted binaries'
  old contents stripped, `ps5/tools/strip-binary-delete-preimage.py`) hung the GPU on each game's
  first start with a cold shader cache (`radv/ps5: the GPU has not reached submission ...`); the same
  build with this merge played fine. Before refreshing again, bisect 0618bfe0ffb..aead7293cfd (ACO's
  scheduler rework and RADV's push-constant/descriptor rework are the first suspects; the four
  hand-merged files are listed in 9ad6b51's message) and test a game's first start on the console.
  `check-sources.sh` reports Mesa as held while 0012 is the HOLD revision.
- HD texture packs download in 4 MiB pieces over 10 connections by default (a fresh connection per
  piece, slow pieces restarted): untested on the console. Whether the Internet Archive throttles per
  connection or per address shows in the boot log's `texture packs:` lines every 5 s; up to N+3
  pieces (52 MiB at 10) are held in memory. `Download connections` = 1 is the old single stream.
- CI skips the download's end-to-end test: its apt line lacks `libcurl4-openssl-dev libarchive-dev`.
- `ps5/tests/bigpicture/run-bigpicture-tests.sh <dir> shots` reports 21 failed checks (quick menu,
  skin, screen saver) with or without the download bar: not looked into (the crawl passes).

