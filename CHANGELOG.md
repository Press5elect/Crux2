# Crux2 - what changed

A short entry for each version, newest first. The build copies this file into the title folder,
where Settings > Network > Updates > What's New reads it: keep the `## vX.Y.Z (date)` headings and the
`- Added:` / `- Changed:` / `- Fixed:` bullets (a heading without a version, such as Unreleased, is
not shown). The full notes of each published release are in
[`docs/releases/`](docs/releases/README.md).

## Unreleased

- Added: **Free Unused Targets When Memory Is Short** (Settings > Picture > Graphics, ADVANCED; off by
  default): at very high resolutions a heavy game could fill the console's graphics memory, and draws then
  went without their textures. Render targets not drawn to for two frames, whose picture is kept elsewhere,
  are freed when that happens or when targets hold more than 6 GB. The resolution stays as you chose.
- Fixed: a `profile` test run with GS Thread Split on samples the drawing thread too (it was left out).
- Added: **Fast Writes to Console Storage** (Settings > PlayStation 5, off by default): large files
  written to the console's storage (a texture pack moved or downloaded there, a disc image) are written
  by a helper the title starts (write-broker.elf, from OGBox), which the console does not slow down
  after the first 1.3 GB as it slows the title. If the helper fails, the title writes the rest itself.
- Changed: **Disable Render Target Compression** (Settings > Picture > Graphics, ADVANCED) is on by
  default: colour render targets are made without surface compression, which is never decompressed
  when a draw reads the target it draws to. A game set to off keeps its choice.

## v1.0.24 (2026-11-24)

- Added: **GS Thread Split** (Settings > Picture > Graphics, ADVANCED; on, two threads, by default): the GS
  thread in two, one reading the game's graphics commands and one drawing, so games held back by the GS
  thread run faster. "Check (One Thread)" does the same work on one thread, to compare the picture.
- Fixed: the graphics switches under Settings > Picture > Graphics > Advanced (Uncompressed Feedback
  Targets, Disable Render Target Compression, Leave out barriers with nothing to wait for, Reuse
  copies of the picture) were read only when the title started for most games: a game's own choice,
  or a change made since the start, was not used. They are read again at each game's start.
- Changed: the profile the title is compiled with is trained again, with the GS thread split on.

## v1.0.23 (2026-11-22)

- Changed: faster paths, each giving the same results as before (on a console no frame rate changed
  in the games measured, which are held back by the GPU, not by these):
  the VU thread's ring keeps the EE's and the VU thread's positions on cache lines of their own; a
  VU1 microprogram upload clears only the program entries in use (it cleared all 2048, 32 KB, each
  time); the audio's time stretch tries four positions at a time (twice as fast on a PC, the same
  sound); an IOP store to memory with no code skips the clearing walk; the counters' power-of-two
  rates are shifted, not divided; a graphics draw no longer zeroes 2 KB of descriptor writes, nor
  asks to flush memory that needs none; a release build no longer checks every IOP block on each
  clear.
- Changed: the graphics thread draws a common sprite format (UV then XYZ2) in a loop of its own
  and tests the 24-bit depth setting before the rest; one store less per primitive; the texture
  cache's block address functions are inlined; the event test divides the clocks only when they
  change. The profile is trained again, with MTVU on and off.
- Changed: the texture cache skips pages with no textures without reading their lists; the ordered
  view's overlap scan runs only when its answer can change something; an event test with no
  interrupt pending skips the interrupt-enable tests; GS image writes read the memory base once.
- Added: a profiled run names the busiest guest blocks of each recompiler, and writes the code of
  the busiest EE blocks into the log once; the test windows count kilobytes of textures uploaded
  (UK), palette textures (PU), texture sources made (SC) and GPU submissions (SB). **Driver Core
  Kept Free** (Settings, the Everything level, on): the other threads off the driver's core.
- Fixed: the graphics thread (and with it the interface) could stop for tens of seconds or minutes
  when texture replacements were loaded again soon after a game with a pack started (a game
  closed, a texture setting changed, a pack download ended): it waited for the whole pack's
  precache, which was then thrown away. It is given up at once now.
- Fixed: a file written to a network share (a card sync, a backup, a pack moved there) held every
  other call to that share until the whole file was written, the graphics thread's look for a
  game's texture pack among them: the picture and the interface stopped meanwhile.
- Fixed: the Memory cards page listed the cards folder and opened every card (a folder card's saves
  all read) each frame it was shown, on the graphics thread: choosing a slot's card or changing a
  switch there could stop the interface and the game. The list is made at most every 3 seconds.
- Fixed: the Settings home showed the four oldest pins only, so a fifth one pinned never appeared.
  It shows as many as its card holds, in the quick menu's order, and scrolls through the rest.
- Changed: **Anti-Aliasing**, **SSAA Filter** and **Debanding** can be set for one game.
- Changed: SSAA scales the picture of the internal resolution chosen down to the screen; it no longer raises
  the resolution (to 8x at 4K). FSR 1 is off while SSAA is chosen and comes back when another choice is
  made. A resolution an earlier build raised for SSAA is put back too when SSAA is left, unless changed
  since (one raised before this version is not known: lower it by hand).
- Added: a test run's GPU line names what the conversion passes did (each conversion shader, and
  clears), the costliest five with their time a frame.
- Added: a test run's GPU line counts the pixels shaded by each kind of pass (and the time a million of
  them took; with the overlay's GPU statistics off), and names the merge, deinterlacing,
  post-processing and in-pass clear passes.
- Changed: the game page's settings tab is **Game Specific Settings** (as are the quick menu's tile,
  the home's hint and the page's card); the page's file card is **Game File**.
- Fixed: the console's M.2 storage (/mnt/ext1) and the internal storage's /mnt/ext0 were never offered:
  game folders named Crux2, PS2 and the like at their top are found as on a USB drive, and both are
  among the places to choose a folder from, to copy a disc to, to export saves to and to look for
  shader presets in.
- Added: the driver counts why the graphics thread waited for an image (the display's pace, the
  GPU, a full queue): a test run's `acquire:` line (Mesa patch 0022).
- Added: **IOP Stores Inline** (Settings, Emulation, the Everything level, off): the IOP
  recompiler writes stores to IOP memory itself.
- Changed: **Enable Multithreaded VU1 (MTVU)** shows on as its default, which is what a new install
  has always set.
- Added: Settings, the Everything level under PlayStation 5, all off or as before: **Skip Unchanged
  Microprograms**, **Coarse Thread Times**, **Wake-Up Spin Time** (50 us, PCSX2's). The boot log
  says what the console's clocks cost (`clock:`); a test or profiled run prints where the threads
  waited for each other (`counters:`: VU wake-ups, XGKICK waits, VU ring, code writes and
  compiles), and the profile splits the time in generated code by recompiler and stub.

## v1.0.22 (2026-11-17)

- Added: two switches against the graphics thread's time spent creating textures (Settings, the
  Everything level; both on): **Shared Render Target Memory** (render targets share memory
  blocks; they had one allocation each) and **Streamed Memory Clearing** (the driver clears new
  memory in one pass, without flushing it line by line). The boot log names what creates surfaces
  and at what sizes (`graphics: N surfaces created ...`), and a profile lists 40 functions on the
  stack (12 before). On a console the graphics thread went from 100% busy to 30-60% in a heavy
  game at 6x, with no fault seen in the picture.
- Changed: built on PCSX2 master of 2026-11-17 (`8407746`: three commits after v1.0.21's: a newer
  controller database and Japanese titles in the game database). Save states are not affected.
- Fixed: about fifty bugs found by a review of the interface's code. Among them: the quick
  menu's Screenshot and a texture pack download ending during a game could stop the picture;
  without a connection every game was noted as having no cover and never asked for again; a
  texture pack moved onto a share that had one for the game was mixed with it; a memory card
  could be lost to a power cut while it was written; Delete on the keyboard took two characters
  after an accented letter; controller ports 2 and up shared port 1's rows; "Internal resolution
  above 12x" jumped to 25x; quick views kept in four of the views came back as Table. The
  test build with them was played on a console; they were not checked there one by one.
- Added: the recomp kit makes Crux2's native modules on Windows too (drop a game on
  "Crux2 Native.cmd"; they were made on Linux only). Checked on Linux with the same compiler
  switches; not yet run on Windows or a console.

## v1.0.21 (2026-11-16)

- Changed: the interface asks the emulator for less when a screen comes up. The quick menu with
  pinned settings made the settings' pages five times as it opened (two now), and the Settings and
  the quick menu read the whole game list, with a look into the saved states, cheats and textures
  folders, each time (the library's list is used now). A long title is cut to its width in one
  pass; discs off the screen's side are not drawn. Measured on a PC
  (`run-bigpicture-tests.sh <folder> bench`).
- Fixed: a game's own settings (Square on Home, a game's page) could take seconds to open with many
  cheat files on the console (Download Cheats for All Games): looking for one game's files asked
  the console about every file in the folder, four times. The name is looked at first now. The boot
  log says when opening them still takes 50 ms or more.
- Fixed: the recomp kit could not link a large game on Windows ("The filename or extension is too
  long"): the object files are given to the linker in a file.
- Added: a second place covers are downloaded from. A game libretro's collection has no box art
  for is asked for by its serial in the ps2-covers collection (xlenore/ps2-covers on GitHub). Games
  found missing from libretro's before are tried there too.

## v1.0.20 (2026-11-16)

- Changed: the interface polished: the button hints in one order on every screen (Circle at the
  left, Cross at the right); messages in the top right corner, over the clock, never over the hints
  or what a screen shows; the settings' home cards show what is set on two lines, at every text
  size; the same corners on every card (skins with rounder or squarer corners had them doubled);
  screens change quicker, and only fade with Motion: Less. Change disc offers the disc in a drive
  again (L1 / R1 and the filter row); L3 on a pinned setting in the quick menu uses the global
  setting again. Not yet run on a console.
- Changed: the top bar's tabs are Home, Library and Disc (with a disc in a drive), L1 / R1; Options
  opens the settings as before. Home is the first screen after the start-up: the game played last
  with Resume (from its newest saved state) and its game page, what goes on right now (texture pack
  downloads, opening the texture pack manager; a disc in a drive; the share, the console's and USB
  drives' games) and the games played lately. The
  library's places are a filter row under the bar (All, Console, USB, Share, then Favourites, Never
  played and Collections; Up from the first row of games), with the number of games, the order and
  the view at its right; the views that did not show the focused game's details have a strip with
  them at the bottom. All ten views, the quick views, the list's menu and choosing a disc for a
  running game are as before. Not yet run on a console.
- Changed: a game's details are its page: the cover with Resume (from the state it was closed with,
  or its newest) and Start from the beginning, a line of what is known of it, and tabs (L1 / R1):
  Overview (where you left off, its own settings, memory card, patches and cheats, texture pack, its
  file, what it is), Save states, Its settings, Patches and cheats, Texture packs. Square is More,
  which now also has A favourite, Hide it, the next disc, Make an image of the disc and starting
  over without the state it was closed with (they were L1, R1, Square and a button before); L2 / R2
  show its other pictures. Not yet run on a console.
- Changed: the quick menu laid out anew: how long the game was played this time; tiles for Resume,
  Save state, Load state, Screenshot (new: the game's picture without the menu), Change disc and
  Quit game; the settings pinned in the Settings, changed in place (the game's own value where a
  game can have one); More with every other entry, chosen and ordered with Triangle as before. Not
  yet run on a console.
- Changed: the settings rebuilt: a home with a search bar, eight areas (Picture, Sound, Controls,
  Games and saves, Emulation, Network, Interface, System) that say what is set and how many settings
  are changed, the pinned settings and the ones changed lately; an area's page with its sections, one
  line a setting and the focused one described in full (choices, default, what it applies to, when it
  takes effect); a search through every setting at every level and the library's games; pins (Square);
  the levels named Essentials, More and Everything; a game's own settings with a switch to those of
  all games (the touch pad) and L3 to use the global setting again. Every option of before is still
  there (the settings inventory). Not yet run on a console.
- Changed: every setting's description is taken word for word from PCSX2 where PCSX2 has the
  setting, and written in PCSX2's style where it has not; labels and choice names likewise.
- Changed: the performance overlay (Settings > On-Screen Display) comes in three sizes (Performance
  Overlay Size): Minimal (a pill with the frame rate), Compact (one bar: frame rate, GPU, EE, GS and VU) and
  Full (a card with the frame time, speed, 1% and 0.1% lows, a frame-time graph, the loads as bars,
  resolution, temperature and memory), plus Custom (what the switches under Show give). A size sets
  those switches; changing one makes it Custom. Digits of one width, colour only for meaning (amber
  close to a limit, red over it), "30 → 60" with frame generation. The overlay stays off until a
  figure is switched on; switching the FPS on gives Compact. Not yet run on a console.
- Added: one Anti-aliasing row (Settings > Graphics > Post-processing): Off, FXAA, SMAA, SSAA or
  SSAA + SMAA. SSAA draws the game larger than the screen (choosing it raises the internal resolution
  to about 1.5 times the screen's and turns FSR 1 off) and makes it screen-sized from every texel each
  pixel covers (SSAA filter: Soft or Sharp), instead of one bilinear tap; SSAA + SMAA runs SMAA over
  that. Not yet run on a console.
- Added: the HD texture pack manager (the library's Square menu, Settings > Graphics, a game's
  options): every pack on the console, the USB drives and the shares by game, with its size and
  files; move a pack between them (copied and checked before the original goes), delete it, download
  the collection's packs for your games one after another. Not yet run on a console.
- Fixed: a black screen with a look (Settings > Graphics > Look): the look's picture is put in the
  layout the screen's pass reads, and PCSX2's vertex buffer is bound again after it. Not yet run on
  a console.
- Fixed: a click on the touch pad's left half could be taken for Start (and the right half for
  Select) when another finger or a thumb was also on the pad, or when the click came a moment before
  the finger's position: the finger that touched last decides now.

## v1.0.19 (2026-11-15)

Since v1.0.18. (An earlier v1.0.19 was built but not published; its work and v1.0.20's are not in
this version: [`docs/history/v1.0.19-v1.0.20-reverted.md`](docs/history/v1.0.19-v1.0.20-reverted.md).)

- Added: HDR10 output (Settings > Graphics, HDR; off), with highlight and white levels and Adjust
  HDR brightness; it needs the console's HDR setting and a TV that takes HDR.
- Added: Debanding (Settings > Graphics; off, Low, High).
- Added: a startup video of Crux2's own in the start-up animation's place (Settings > Interface,
  Startup video).
- Added: a PlayStation Move as player 1's GunCon 2, through the Move Management payload (Settings >
  Controllers; off).
- Added: game fixes fetched from GitHub at start (game database entries and patches, checked
  against ps5/game-fixes.txt; Settings > PlayStation 5, on).
- Added: the boot log is kept in memory and written when something goes wrong (Settings > Log,
  Boot log: or always written); Save the log in Settings > Log and in the quick menu.
- Added: the quick menu's entries can be shown, hidden and ordered (Triangle), the game's options
  are in it, and Apply changes now restarts the game for settings that need it.
- Added: frame generation's pictures a second on the on-screen display, and a log line every 10 s.
- Added: test builds say "developer build" and their updater offers the release; the updater
  removes files a new version no longer ships.
- Added: a light gun choice (Settings > Controllers > Light gun): one GunCon 2 in USB port 1, or two
  in ports 1 and 2, with their buttons set up. Before, a GunCon 2 could only be plugged in by
  editing the settings file.
- Added: steady motion aim for the GunCon 2 (from Porpoise): the same pointing gives the same place,
  no drift, R3 held a moment recentres, the screen measured by pointing at two corners, a second
  gun aimed by player 2's controller. The aim as before is a choice ("Like a mouse").
- Added: R2 clicks like a trigger for the light gun (Off / Light / Firm), and stick response curves
  per stick (Linear, as before, by default).
- Added: USB pads through the console's raw USB devices when its USB library can't be used.
- Added: network shares set up on the console again (Settings > Folders): servers found on the
  network (SMB and NFS), a server's shares or exports listed, an address on a number pad, and a
  test that says what went wrong. SMB checks the server's port before connecting, and the shares'
  sockets now really don't block on the PS5.
- Added: a test for the name server set by hand, and public DNS asked when the console's answers
  0.0.0.0. A UDP port the console refuses a game is bound lower.
- Added: achievements unlocked as the console's trophy pop-ups, with the trophy sound (on by
  default; PCSX2's own notice when the console's can't be shown).
- Added: Crux2's own screenshots with Options + Square (the game's picture, no menus), in snaps/;
  the first becomes the game's picture of play when it has none.
- Added: launch arguments `--rom <file>` and `--exit-after-game`.
- Added: settings tested on a PS5 for a game, downloaded from the repository once a day (none yet),
  in the game's own options.
- Added: Steady 30 for frame generation (off), and a colour filter on the game's picture for colour
  blindness (off).
- Added: the recomp kit (`Crux2-recomp-kit-<version>.zip`) comes with every release, beside Crux2's
  ZIP: it makes a game's native modules and Crux2 Recomp's on a PC (Windows or Linux).
- Changed: covers and pictures are downloaded three at a time; the covers on screen load first.
- Changed: quick taps are held for the game at least 50 ms.
- Changed: network shares are woken at start; a game on a share that does not answer waits for it
  with a count on the screen, and a read stuck for 2 s is said on the screen.
- Changed: an exit, a restart or a state load waits for the writes still under way, with a bar.
- Changed: settings are written when the menus close, and at a game's start, quit and crash.
- Changed: texture packs are held within a memory budget (at most 2 GiB); the least recently used
  textures are let go and read again when needed.
- Changed: HD texture packs download over 10 connections at once (a game's options > Graphics,
  Download connections: 1 to 16; 1 is one stream as before), each piece on a new connection, with a
  progress bar over the menus (size, percent, speed, connections, time left).
- Changed: the payload SDK fork is at `3345cde` (was `b5efad5`): built over the SDK's v0.45 release
  instead of v0.42, so the C++ runtime linked into the title is a newer build; the memory heap grows
  in 2 MiB steps instead of 16, keeps its pieces apart so emptied ones can be given back, and keeps
  32 MiB spare before giving any back; more files can stay open at once. Needs a start, a game and a
  frame-rate check on the console.
- Changed: PS5_Vulkan is pinned at `7f492b4` (was `f410e67`): its new commits are Wine/Proton
  graphics work and a driver test; the link recipe, libc.prx, the packaging and its Mesa pin are as
  they were.
- Changed: PS2Recomp is at `e563698` (was `eb8688b`) for native modules: GIF packets with the
  FLG=3 format are taken as image data, as on the PS2; def-jam-vendetta-recomp's patch rebased.
- Changed: built on PCSX2 v2.9.179 (was v2.9.150; master `0ab00c4`): the PS1 Analog Controller
  (DualShock 1) as a controller type, depth drawn as a colour target for the Vulkan renderer's
  depth feedback (not used by default on the PS5: the default depth feedback mode keeps the old
  path), texture cache and VU cycle fixes, bounds checks in VIF, CDVD and PS1 memory, game database
  fixes. Save states made before do not load (PCSX2 changed their version).
- Changed: the graphics driver (RADV) stays on Mesa main of 2026-10-31 (`0618bfe0ffb`, as v1.0.18):
  the newer Mesa of 2026-11-15 stopped the GPU the first time each game started.
- Fixed: a backup that could not read a file was written as complete, and with "replace older"
  the previous good backup was deleted. Images over 8192 pixels on a side are refused before they
  are decoded. The sandbox helper for USB drives is asked again for a few seconds when it doesn't
  answer at once.

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
- Fixed: What's new (Settings > PlayStation 5) only said whether a newer version was out. It now shows
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
