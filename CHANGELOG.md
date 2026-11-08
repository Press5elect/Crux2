# Crux2 - what changed

## v1.0.20 (2026-11-06)

- Changed: SMAA is a switch beside FXAA (Settings > Picture > Post-processing) instead of an entry of
  the Look list, and runs before a Look, so the two can be used together.
- Fixed: every look, and SMAA, left the screen black with no overlay: the look's passes bound their own
  quad at vertex binding 0, PCSX2 binds its vertex buffer only once per command buffer, so the picture
  on the screen and the overlay were drawn from the quad's 4 vertices: nothing. It is bound again after
  the look. The look's check (that its picture is not black) now reads back once every 30 frames at most.
- Added: a look, or SMAA, that cannot draw on the console is switched off with a message: its first
  pictures are checked, and one that is black while the game's own is not is switched off.
- Fixed: accurate floating point on the VUs kept the game's own VU rounding mode after one rare
  add, instead of the PS2's; in games with a VU rounding fix, later results came out slightly
  wrong.
- Fixed: accurate floating point on VU0 ran in the EE's rounding mode for the VU0 instructions
  the EE runs itself (COP2); in games with an EE rounding fix their results were wrong.
- Fixed: the EE interpreter compared floats differently from the recompiler when accurate
  floating point was off (infinities and NaNs).
- Fixed: accurate floating point's SQRT.S cleared the FPU's overflow and underflow flags; the
  PS2's only touches invalid and divide.
- Fixed: under a menu or paused, a picture FSR enlarged long ago could be shown instead of the
  game's own after FSR had stopped enlarging it.
- Changed: less work in the EE's recompiled ADDA, MADDA, MSUBA and MULA (a flag nothing read).
- Changed: accurate floating point keeps more of the EE's registers around its calls, compares
  without branches, and VU adds take their fast path.
- Changed: the pipelines a game needed last time are made on the optimiser's thread, not the
  GS thread: no stalls in a game's first frames.
- Changed: a picture shown twice (frame generation not making one) gets its look and side
  picture once. The side picture builds only its own pass, not frame generation's.
- Changed: smaller fixes: the game's pipeline list is merged without a quadratic search, the
  pipeline cache's dirty flag is atomic, and draws that leave colour or depth alone no longer
  invalidate its copies.
- Changed: the content version in param.json is 01.020.000.
- Changed: the build first checks every source against what it follows (ps5/tools/check-sources.sh):
  PCSX2's master, Mesa main for the 27.0-devel merge, the PS5 Mesa revision PS5_Vulkan pins,
  PS5_Vulkan, the payload SDK, paraLLEl-GS, the Granite and volk revisions paraLLEl-GS pins, and
  PS2Recomp; it stops when one is behind (PS5PX_ALLOW_STALE=1 builds anyway).
- Changed: the payload SDK fork is at `e105cf6` (was `b5efad5`): memory for the heap, thread stacks
  and code is taken as plain direct memory first, the heap grows in 2 MiB steps instead of 16 (up to
  8192 segments), and more file descriptor groups. Needs a frame-rate check on the console.
- Changed: PS5_Vulkan is pinned (`ace98a9`, was `f410e67`, named only in the README):
  ps5/tools/bootstrap.sh says when the checkout beside Crux2 is elsewhere. Its new commits are
  gpu-broker and Windows work; the link recipe, libc.prx, the packaging and its Mesa pin are as they
  were.
- Changed: the graphics driver (RADV) is on Mesa main of 2026-11-08 (`6b41b56dea2`; was 2026-10-31):
  531 commits, 94 of them in RADV, ACO, NIR and the Vulkan runtime, among them ACO keeping SSBO
  scalar loads behind barriers, binning (PBB) fixes for GFX10, a fix for barycentric inputs at an
  offset in divergent code, and cheaper multiplies. Seven files conflicted with the PS5 port and
  were merged by hand (ps5/patches/mesa-27/README.md lists them); it compiles and links on a PC
  and has not run on the console yet.
- Added: the Logitech USB headset of games with voice chat (Settings > Controllers > USB
  microphones, "Headset", USB port 2): player 1's microphone is its microphone, and what the
  game plays to its earpiece is played on player 1's headset (the console's personal port for that
  player), or with the game's sound when that does not open. Needs testing on the console.

## v1.0.19 (2026-11-06)

- Added: SMAA anti-aliasing as a Look (Settings > Picture), beside FXAA; the FXAA row now points to
  the internal resolution and paraLLEl-GS's samples for supersampling.
- Added: Settings > Debug > Logs: logs on or off, where they go (the console or a USB drive), and a
  switch for each (Crux2's boot log, the graphics driver's messages, the emulator's log, the klog copy).
- Added: network shares in Settings > Folders: added, changed and taken away with the on-screen
  keyboard, servers found on the network, connected at once; network.cfg is written for you.
- Changed: games on a network share are read faster (large buffers on the share's connection).
- Changed: the boot log is written by a thread of its own, so no line waits for the file system;
  it is synced to the drive only when a game starts or closes and at the end.
- Changed: the five-minute safety save is kept in memory and written to the drive only if Crux2
  crashes; the PS5 menu's save is made at most once a minute; automatic states are compressed harder.
- Changed: PCSX2's own pipeline cache file is no longer written (the graphics driver's cache keeps
  the same pipelines).
- Changed: the graphics driver backs a large buffer with several pieces of memory when no single
  free piece is big enough.
- Changed: built on PCSX2 v2.9.158 (was v2.9.150): out-of-bounds reads fixed when replacement
  textures load, a Vulkan feedback-loop fix (a texture is marked changed when it switches layout),
  a guard against out-of-bounds 4-bit texture reads from bad GS registers, more Japanese serials and corrected titles in the game database, a newer controller database,
  and LZMA 26.05.
- Changed: the content version in param.json is 01.019.000.
- Fixed: the console could go down after Crux2 was closed many times with USB drives on.
- Fixed: SingStar's microphones heard nothing: the console's microphone library is now imported
  by the title instead of looked for while it runs, which a title cannot do.

## v1.0.18 (2026-10-31)

- Added: Settings > Folders: game folders anywhere (USB drives, network shares, the console's
  storage), picked with a folder browser and searched with their sub-folders; and a folder of your
  choice for the BIOS, memory cards, save states, screenshots, cheats, covers and texture packs.

- Added: Accurate floating point for the EE's FPU, VU0 and VU1 (Settings > Advanced for all games,
  a game's own Emulation page for one; three switches, off), from PCSX2 Reliquary: the PS2's exact floating-point results, done in
  the recompilers.
- Changed: accurate floating point is on for the FPU and VU0 in the two games known to need it
  (their AI or physics go wrong without it), unless the game's own settings say otherwise.
- Changed: x86 emitter encoding fixes from PCSX2 Reliquary.
- Changed: the content version in param.json is 01.018.000.
- Fixed: frame generation sped up and slowed down every few seconds in games at 30 frames a
  second: one late or early frame changed how every following frame was made. It now keeps the
  game's rate until the game really changes it.
- Fixed: frame generation at 120 Hz made one picture between the game's instead of three when a
  game handed its frames over unevenly; it now takes the game's average rate.
- Fixed: frame generation at 120 Hz held games at 67% or 50% speed even standing still: with two
  pictures for each frame, as many frames may now wait for the screen as without it.
- Added: MihawkGen as a third frame generation method (Settings > Graphics, Frame generation
  method): Mihawk's small trained network, from PSSwanStation and PSFlyCast.
- Added: Show the movement (Settings > Graphics, advanced): AMD FSR 3 optical flow's made frames
  show what it decided for each pixel, for checking frame generation.
- Changed: frame generation works at the game picture's own size when the screen only stretches
  it (no FSR or NIS): less work for each picture it makes.
- Fixed: What's new (Settings > System) only said whether a newer version was out. It now shows
  what was added, changed and fixed in each version, newest first, and opens by itself once after
  an update with the changes since the version that ran before.

## v1.0.17 (2026-10-31)

- Changed: less work on the GS thread: the graphics driver's thread gets a core of its own
  (Settings > Emulation, "Graphics driver on a core of its own", on), the auto-flush test is no
  longer repeated at every vertex in vertex formats other than the two PCSX2 handles in one
  piece, and a texture upload no longer checks every mipmapped texture level by level.
- Changed: the boot log names the vertex formats each game draws in.
- Changed: the content version in param.json is 01.017.000.

## v1.0.16 (2026-10-31)

- Added: Variable refresh (VRR) at 120 Hz (a setting, off; experimental).
- Changed: built on the newest Mesa main (27.0.0-devel, 2026-10-31) and PCSX2 v2.9.150.
- Changed: GS dumps record 1 second by default (a setting: 1, 2 or 5), are compressed on worker
  threads, and the saved message gives their size.
- Changed: the content version in param.json is 01.016.000.
- Fixed: 120 Hz fell back to 60 Hz on TVs with VRR.
- Fixed: the README's list of what does not work was out of date.

## v1.0.15 (2026-10-25)

- Changed: every setting's description rewritten in plain English; settings PCSX2 also has use
  PCSX2's own description.
- Added: the version, WIP, the build number and the author in the top bar, under the clock.
- Fixed (to be confirmed on the console): a game could freeze a few seconds after it started (a lost wake-up between the emulation
  and GS threads when the frame queue changed; v1.0.14).
- Changed: the content version in param.json goes up with each release (01.015.000), so the
  console and the homebrew catalog see updates.

## v1.0.14 (2026-10-24)

- Changed: while a game runs below full speed, up to 2 frames may wait for the GS thread, so the
  emulation and the GS thread no longer take turns in heavy scenes (a setting, on).
- Added: frames queued in the boot log's test window lines.

## v1.0.13 (2026-10-24)

- Changed: built on Mesa 27.0-devel's RADV (Mesa's main branch; was 26.2.5).
- Changed: draws through the rasterizer ordered view (accurate blending) keep their render pass:
  a scene that started one for nearly every draw (light sources) starts a few hundred.
- Fixed: the graphics chip's own hardware bugs are worked around as for its real generation
  (shader compiler hazards, two geometry hang guards).
- Fixed: the graphics switches that take effect at the next start only took effect when Crux2
  started.
- Fixed: the boot log showed the settings from before a game's database fixes were applied; it
  now lists those fixes.

## v1.0.12 (2026-10-24)

- Changed: a draw that reads the picture it draws on waits for the graphics chip only when
  something was drawn there since the last wait (light sources; a setting).
- Changed: copies of the picture that glows and blurs read are reused while nothing was drawn
  into them since (a setting).
- Added: Render targets without compression (experimental setting, off).
- Added: barriers skipped and copies reused on the on-screen Statistics and in the `slow:` lines.

## v1.0.11 (2026-10-24)

- Added: Record a GS dump in the quick menu (5 seconds, to snaps/), with either renderer.
- Added: the boot log says what the emulator was doing whenever a game stays under full speed
  for 2 seconds.
- Added: Keep read render targets uncompressed (experimental setting, off): no decompression of
  the picture each time a glow or flare reads it.
- Changed: paraLLEl-GS compiles the shaders a game needs in the background, those it used last
  time first, and keeps them between sessions.
- Fixed: the GS dump hotkeys recorded nothing while paraLLEl-GS drew, and said nothing on screen.
- Fixed: the graphics driver's shader cache could switch itself off on the console (Mesa 26.2.5).

## v1.0.10 (2026-10-24)

- Added: with Statistics on, the on-screen display shows draws, barriers, render passes, copies,
  read-backs and render-target copies a frame; GPU statistics shows the graphics card's time by kind.
- Added: paraLLEl-GS pictures shared with the graphics device (experimental setting, off).
- Changed: glow and flare draws that sample the screen keep their render pass (one barrier inside
  it, as before v1.0.8's PCSX2 update), on the PS5 only.
- Changed: automatic quality keeps paraLLEl-GS at 4 samples or more while its picture is at twice
  the size; NIS hands enlargements past 2x to FSR instead of the screen's filter.
- Fixed: paraLLEl-GS dithered 32-bit frame buffers; it dithers 16-bit ones only, as the PS2.
- Fixed: paraLLEl-GS blended alpha masks at polygon edges at 4 and 8 samples (fringes around
  shadows and glows); alpha stays exact, colours stay smooth.

## v1.0.9 (2026-10-24)

- Fixed: paraLLEl-GS drew a picture of dots or froze in some games (since v1.0.8).
- Fixed: the D-pad's Up and Down stopped working after stepping Renderer through Software.
- Added: Dithering works with paraLLEl-GS (Off and Force 32 bit draw without the dot pattern).
- Changed: paraLLEl-GS's samples and picture at twice the size change while a game runs.
- Changed: paraLLEl-GS handing work over as it goes is a setting, off until proven on the console.

## v1.0.8 (2026-10-24)

- Changed: built on PCSX2 v2.9.142 (was v2.9.138) and Mesa 26.2.5's RADV (was 26.2.0).
- Changed: paraLLEl-GS hands the graphics card its work as it goes, so a game's read of GS memory
  waits for less; its thread no longer stops while a finished picture comes back.
- Added: the boot log's paraLLEl-GS line says how much of a read's cost is the graphics card.
- Fixed: Hardware download mode Off still made paraLLEl-GS wait for the graphics card at each read.

## v1.0.7 (2026-10-24)

- Added: paraLLEl-GS counts the game's reads of GS memory and what they cost (the on-screen display's
  *reads* row, the boot log's status line).
- Changed: Hardware download mode works with paraLLEl-GS: Unsynchronised answers a read of what the
  graphics card has not finished with zeros instead of waiting; Off reads nothing.

## v1.0.6 (2026-10-24)

- Added: paraLLEl-GS's graphics card time, thread and wait on the on-screen display and in the boot
  log every 30 s.
- Added: automatic quality with paraLLEl-GS steps the samples a pixel.
- Changed: Renderer is one choice: Hardware (Vulkan), Software, paraLLEl-GS.

## v1.0.5 (2026-10-23)

- Changed: paraLLEl-GS takes the display settings (deinterlacing, anti-blur, screen offsets,
  overscan, mipmapping), live; the settings it has no equivalent of are greyed out while it is on.

## v1.0.4 (2026-10-23)

- Added: Console language (Settings > BIOS, and per game): the language games are told the PS2 has,
  over the BIOS's own, without changing the BIOS's setting.

## v1.0.3 (2026-10-23)

- Fixed: opening Settings stopped Crux2 (the controllers page, since v1.0.1).

## v1.0.2 (2026-10-23)

- Changed: the on-screen display is one panel in sections (graphics chip, processor, frames with
  1% / 0.1% lows and a frame-time graph); Style under On-Screen Display has PCSX2's lines back.
- Fixed: switching on an on-screen figure showed nothing (their place was "Not shown" by default).
- Fixed: games played one after another slowed down: the game before's textures stayed in the
  graphics device's pool; they go when a game closes, and the boot log reports memory at each
  game's start and end.

## v1.0.1 (2026-10-23)

- Added: USB controllers (experimental): DualShock 3 / SIXAXIS with pressure-sensitive buttons,
  Xbox 360 pads, guitars, drums and other USB pads.
- Added: Start the PlayStation 2 menu (Settings > BIOS, and the library's Square menu).
- Fixed: the PS2's start-up ran at double speed at 120 Hz with frames paced by the screen.

## v1.0.0 (2026-10-23)

The first release, built on PCSX2 v2.9.138.

- Added: PCSX2's x86-64 recompilers and Vulkan hardware renderer on the PS5 (RADV), the software
  renderer, save states, cheats, PCSX2's patch collection, the network adapter and hard drive,
  RetroAchievements.
- Added: paraLLEl-GS as a second GS renderer.
- Added: games from the title folder, USB drives, SMB and NFS shares, the console's disc drive and
  USB DVD drives; discs read into memory, or made into ISO, CHD or CSO images.
- Added: a Big Picture interface with ten library views, search, filters, quick views, favourites
  and collections, covers and pictures, skins, an animated mark, interface and text size, and music
  of your own.
- Added: settings at three levels (Normal, Advanced, Expert), for all games or one game.
- Added: FSR 1, NIS and CAS, Automatic quality, frame generation, black frame insertion, 60 or
  120 Hz, CRT and signal looks, HD texture packs (also downloaded).
- Added: a memory card for each game, a card manager with save import, sleep-safe saving, states
  saved by themselves with undo, backups, and memory cards in sync with a share.
- Added: four players, controller profiles, pressure from R2, GunCon 2 by motion, USB microphones,
  a USB keyboard, light bars, control from a phone.
- Added: care for an OLED screen, updates from GitHub, a free BIOS fallback, native code modules
  (experimental), the boot log on the screen and profiled starts.
