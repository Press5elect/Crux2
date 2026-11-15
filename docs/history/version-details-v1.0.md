# What each version changed, in detail (v1.0.0 to v1.0.18)

The README carried these sections until the documentation was restructured (after v1.0.18). They
say more than the [changelog](../../CHANGELOG.md) and the [release notes](../releases/README.md):
often why a change was made and what was measured. They are kept as written; a setting path in them may not match
today's settings pages (the README has those).

## What v1.0.18 changed

Added:
- **Settings > Folders**: games and files where you keep them.
  - *Add a game folder* opens a folder browser (USB drives, network shares, the console's
    storage). The folder and the folders inside it are searched for games, which stay where they
    are. Each folder added is listed there and can be taken off the list again; the folders Crux2
    finds by itself (a `PS2`, `ps2iso`, `PCSX2`... folder at the top of a USB drive) are shown too.
  - *Where Crux2 keeps its files*: a folder of your choice, or the default, for the BIOS, memory
    cards, save states, screenshots and GS dumps, cheats, covers and texture replacements. Folders
    Crux2 writes to are checked for writing first. These are PCSX2's own folder settings
    (`[Folders]` in `inis/PCSX2.ini`), and take effect for the next game started.
- **Accurate floating point** (Settings > Advanced, expert level, for all games; and a game's own
  Settings > Emulation, for that game: *Accurate floating point (FPU)*, *(VU0)* and *(VU1)*, off). The PS2's floating-point units do
  not round like a PC's or the PS5's, and a few games' AI or physics depend on the PS2's exact
  results. These switches do the PS2's arithmetic exactly, inside the recompilers (from PCSX2
  Reliquary). FPU and VU0 cost some speed; VU1 costs a lot and is only for games that need it.
- **MihawkGen, a third frame generation method** (Settings > Graphics > *Frame generation method*),
  as PSSwanStation and PSFlyCast have it: Mihawk's small trained network (432,308 parameters,
  GPL-3.0-or-later), which looks at the game's two pictures only, estimates the movement and a mask
  at four scales, warps both pictures and mixes them, and leaves what did not move exactly as it
  was. If it cannot start, Crux2's own method makes the frames and the boot log says why.
- **Show the movement** (Settings > Graphics, advanced): with AMD FSR 3 optical flow, the made
  frames show what was decided for each pixel instead of the picture (red both frames, green the
  newer alone, blue the older alone, grey the two mixed). For checking frame generation, not for
  playing.

Changed:
- **Frame generation is lighter without an upscaler**: when the game's picture reaches the screen by
  a plain stretch (Upscaling off, or a picture already near the screen's size), the pictures are
  kept, searched and made at the game picture's own size instead of the screen's. With FSR or NIS
  (the default is FSR) the upscaled picture is interpolated as before: making the pictures before
  the upscaler would mean upscaling every made picture as well, which costs more than it saves.
- **Accurate floating point is switched on for the FPU and VU0** in the two games known to need
  it, unless a game's own settings name the switch. The boot log's `game fixes:` line says when it
  is on.
- **Save states keep their format**: states from v1.0.17 load in v1.0.18 and the other way round.
- **The content version is 01.018.000.**

Fixed:
- **Frame generation sped up and slowed down every few seconds** in games at 30 frames a second.
  It took the game's rate from the last frame alone, so a single late or early frame (which games
  have when they load or drop a frame) changed how every following frame was made: halves, then
  none, then thirds, several times a second. It now settles on the game's rate from the last eight
  frames, the average stay rounded, and changes it only when that average moves three quarters of
  a frame away from it.
- **Frame generation at 120 Hz made one picture between the game's instead of three** when a game
  below full speed handed its frames over unevenly (one vsync, then three). It took that as a game
  at 60 and doubled it, so the screen showed 60 a second. The average of the last eight frames now
  takes it at its real rate, 30, and three pictures are made between each of its own.
- **Frame generation at 120 Hz held games at 67% or 50% speed**, even standing still, with the
  emulator and the graphics chip each busy under half the time. Each frame was given two pictures,
  and only two pictures were let wait for the screen, so the next frame was begun only once the last
  one was on screen: the recording and the drawing came one after the other. With two pictures a
  frame, four may now wait, the same two frames as without frame generation.
- **What's new** (Settings > System) only said whether a newer Crux2 was out. It now shows what was
  added, changed and fixed in each version, newest first, from the `CHANGELOG.md` the title folder
  now holds. After an update it opens by itself once, with the changes since the version that ran
  before.

## What v1.0.17 changed

Changed:
- **Less work on the GS thread**, the thread that limits the heaviest scenes:
  - the graphics driver's recording thread runs on a core of its own instead of sharing the GS
    thread's core (Settings > Emulation, *Graphics driver on a core of its own*, on; it needs
    *Thread pinning* at *Cores of their own*, the default);
  - the auto-flush test, already worked out once per strip in PCSX2's two packed vertex formats,
    is worked out once per strip in every format that carries only vertex data;
  - a texture upload skips mipmapped textures whose levels it does not touch, instead of checking
    each of their levels.
- **The boot log names each vertex format a game draws in** (lines starting `GS: PS5: packed format`).
- **The content version is 01.017.000.**

## What v1.0.16 changed

Added:
- **Variable refresh (VRR)** (Settings > Interface > PlayStation 5, off; experimental): at 120 Hz on
  a TV with VRR, each frame is shown as soon as it is ready.

Changed:
- **Built on the newest Mesa main** (27.0.0-devel of 2026-10-31; was 2026-10-23): fixes to the
  shader compiler and to how graphics pipelines are hashed for the shader cache.
- **Built on PCSX2 v2.9.150** (was v2.9.142): game database fixes, empty per-game settings files
  removed when saved, vibration shown in the inputs display.
- **GS dumps record 1 second by default** (*GS dump length*: 1, 2 or 5 seconds), and PCSX2's
  dumps are compressed on worker threads, so recording slows the game less. The "dump saved"
  message says the file's size.
- **The content version is 01.016.000.**

Fixed:
- **120 Hz fell back to 60 Hz on TVs with VRR**: before the first frame such a TV refreshes at
  48 Hz, which was taken for a TV still at 60 Hz. The boot log now says which rate was chosen
  and why.
- **The README's list of what does not work** still named texture pack downloads, microphones and
  trigger pressure, which Crux2 has.

## What v1.0.15 changed

Added:
- **The version, WIP, the build number and "by Press5elect"** in the top bar, under the clock.

Changed:
- **Every setting's description is rewritten** in short, plain English. Settings that PCSX2 also
  has use PCSX2's own description.
- **The content version goes up with each release** (`01.015.000`), so the console and the
  homebrew catalog see updates.

Fixed:
- **A game could freeze a few seconds after it started** (v1.0.14; to be confirmed on the console): the emulation thread could
  wait for a wake-up the GS thread had already given up on when the frame queue changed size.

## What v1.0.14 changed

Changed:
- **Frames queue while a game is behind**: with no frames queued (Crux2's default), the
  emulation waited at every frame for the GS thread to finish the one before, so the two took
  turns once a scene got heavy. While a game runs below full speed up to 2 frames may now wait,
  and the two work at the same time; none again after 10 seconds at full speed (*Queue frames
  while the game is behind*, on; Graphics > Advanced).

Added:
- **Frames queued** at the end of each `test: window` line of the boot log.

## What v1.0.13 changed

Changed:
- **Draws through the rasterizer ordered view keep their render pass** (accurate blending at
  light sources): a scene that started a render pass for nearly every draw starts a few hundred,
  far less work for the GS thread (*Keep the ordered view's render passes*, on).
- **Built on Mesa 27.0-devel's RADV** (Mesa's main branch; was 26.2.5): less work for the
  processor at each draw, fewer cache flushes, fixes to compressed depth and colour buffers.

Fixed:
- **The graphics chip's own hardware bugs** are worked around as for its real generation: the
  shader compiler's hazard workarounds and two geometry hang guards were off.
- **Graphics switches that take effect at the next start** took effect only when Crux2 started;
  they are read at each game's start now.
- **The boot log's settings line** showed the settings from before a game's database fixes were
  applied; a `game fixes:` line now lists them.

## What v1.0.12 changed

Added:
- **Render targets without compression (experimental, off; Graphics > Advanced)**: nothing is
  ever decompressed when a glow reads the picture; every other draw moves more data.
- **Barriers skipped and copies reused** on the on-screen Statistics and in the boot log's
  `slow:` lines.

Changed:
- **Fewer waits at light sources**: a draw that reads the picture it draws on waits for the
  graphics chip only when something was drawn there since the last wait (*Leave out barriers
  with nothing to wait for*, on).
- **Fewer copies at light sources**: a copy of the picture that a glow or blur reads is used
  again while nothing was drawn into it since (*Reuse copies of the picture*, on).

## What v1.0.11 changed

Added:
- **Record a GS dump** in the quick menu: 5 seconds of what the game sends the GS, to `snaps/`,
  with either renderer (for reproducing a problem on a PC).
- **Slow moments in the boot log**: whenever a game stays under full speed for 2 seconds, a
  `slow:` line with the threads' and the graphics chip's load and what a frame drew.
- **Keep read render targets uncompressed (experimental, off)**: a render target a glow or flare
  reads from itself is not decompressed by the graphics chip each time.

Changed:
- **paraLLEl-GS compiles its shaders ahead**, in the background, the ones the game used last time
  first, and keeps them between sessions (`pgs-pipeline-cache.bin` in the title folder): far
  fewer pauses when a game starts or a scene is new.

Fixed:
- **The GS dump hotkeys** recorded nothing while paraLLEl-GS drew, and their messages were not
  shown.
- **The graphics driver's shader cache could switch itself off** on the console since v1.0.8.

## What v1.0.10 changed

Added:
- **Statistics on the on-screen display**: with Statistics on, draws, barriers, render passes,
  copies, uploads, read-backs and render-target copies a frame (what a scene at a light source
  costs); GPU statistics shows the graphics card's time a frame by kind of pass.
- **paraLLEl-GS pictures shared with the graphics device (experimental)**: its pictures reach
  PCSX2's graphics device through shared memory instead of a copy by the processor on the GS
  thread. Off by default; takes effect at the next start.

Changed:
- **Glow and flare draws keep their render pass** on the PS5: PCSX2 v2.9.142 ends and starts the
  render pass around every draw that samples the screen; on this graphics chip a barrier inside the
  pass does the same, as before.
- **Automatic quality keeps paraLLEl-GS at 4 samples or more** while its picture is at twice the
  size (below 4 the picture fell to the PS2's own size).
- **NIS enlarges up to 2x and FSR 1 takes over past that**, instead of the screen's plain filter.

Fixed:
- **paraLLEl-GS dithered 32-bit frame buffers** (a fine pattern and darker glows); it dithers 16-bit
  ones only, as the PS2 does.
- **paraLLEl-GS blended alpha masks at polygon edges** at 4 and 8 samples, giving fringes around
  shadows and glows; alpha now stays exact while colours stay smooth.

## What v1.0.9 changed

Added:
- **Dithering works with paraLLEl-GS**: Off and Force 32 bit draw without the PS2's dot pattern
  (Force 32 bit also shows the 32-bit picture a game copies into a 16-bit screen at the end);
  Scaled and Unscaled dither as the PS2.

Changed:
- **paraLLEl-GS's samples and its picture at twice the size** change while a game runs.
- **paraLLEl-GS handing work over as it goes** is a setting (*paraLLEl-GS hands work over as it
  goes (experimental)*), off until it is proven on the console.

Fixed:
- **paraLLEl-GS drew a picture of dots, or froze with the sound going on**, in some games since
  v1.0.8: handing work over early could upload a texture before what decides its contents, and
  recycled memory the frame still used.
- **The D-pad's Up and Down stopped working** after stepping Renderer through Software with
  Left/Right: a release during the graphics device's restart was lost.

## What v1.0.8 changed

Added:
- The boot log's `paraLLEl-GS:` line says how much of the reads' cost is the graphics card
  finishing what a read needs, and how often work was handed to it early.

Changed:
- **Built on PCSX2 v2.9.142** (was v2.9.138): Vulkan barriers where a draw samples its own render
  target, and accuracy fixes in the hardware renderer and its texture cache.
- **Built on Mesa 26.2.5's RADV** (was 26.2.0): the stable fixes since, among them binning on this
  graphics chip's generation, sRGB clears, depth/stencil resolves and shader compiler fixes.
- **paraLLEl-GS hands the graphics card its work as it goes** instead of a frame at a time, so a
  game that reads its picture back waits for what is left, not for the whole frame; and its thread
  keeps taking the game's next frame while a finished picture comes back, instead of stopping.

Fixed:
- Hardware download mode *Off* still made paraLLEl-GS wait for the graphics card at each read.

## What v1.0.7 changed

Added:
- **paraLLEl-GS counts the game's reads of GS memory** and what they cost. A read waits for
  everything drawn before it, so a game that reads its picture back every frame runs the
  emulator and the graphics card in turn, with nothing at 100%: the on-screen display's processor
  section gets a *reads* row (the GS thread's time a frame in those waits) while a game reads, and
  the boot log's paraLLEl-GS status line says how many reads a frame and what they cost.

Changed:
- **Hardware download mode works with paraLLEl-GS**: *Unsynchronised* answers a read of what the
  graphics card has not finished with zeros instead of waiting for it, *Off* reads nothing; both
  faster, both wrong where the game uses what it read. The other modes wait and are right, as
  before.

## What v1.0.6 changed

Added:
- **paraLLEl-GS's figures**: the on-screen display's GPU section shows paraLLEl-GS's own graphics
  card time a frame (from its timestamps; PCSX2's GPU figure sees only PCSX2's device, which then
  does little), its shading part and the samples a pixel; the processor section its thread and how
  long the GS thread waited for it. The boot log has the same line every 30 seconds while a game
  runs.
- **Automatic quality with paraLLEl-GS**: when the graphics card cannot keep the game at full speed,
  the samples a pixel go down a step (16, 8, 4, 2, 1) for as long as it needs to, and back up when
  there is room again, said on the screen as the resolution steps are.

Changed:
- **Renderer** is one choice: Hardware (Vulkan), Software, paraLLEl-GS; paraLLEl-GS's own rows
  follow it.

## What v1.0.5 changed

Changed:
- **paraLLEl-GS takes the display settings**: Deinterlacing (Automatic: a progressive picture where
  the game allows it; any other choice: the fields as the PS2's own scan-out blended them), Anti-blur,
  Screen offsets, Show overscan and Mipmapping, changed while a game runs. What it has no equivalent
  of (the internal resolution, texture filtering, dithering, blending accuracy, the hardware and
  upscaling fixes, texture packs, Disable interlace offset) is greyed out while it is on; the
  paraLLEl-GS row says which is which.

## What v1.0.4 changed

Added:
- **Console language** (Settings > BIOS, and in a game's own options): the language games are told
  the PlayStation 2 is set to, for the many European games that show theirs in it. "As the BIOS has
  it" keeps the BIOS's own; the BIOS's setting is never changed. Takes effect at the game's next start.

## What v1.0.3 changed

Fixed:
- **Opening Settings stopped Crux2** (since v1.0.1): the controllers page read a setting while it
  already held the settings' lock.

## What v1.0.2 changed

Changed:
- **The on-screen display is one panel**, in sections as PC overlays show them: the graphics chip
  (its load and time a frame), the processor (the emulator's threads, and with Hardware info its
  temperature and clock where the console tells them, and the memory in use), and the frames (FPS,
  frame time, max, average, min, 1% and 0.1% lows) with a graph of the frame times, in the title's
  lettering. Settings > On-Screen Display > Style has PCSX2's own lines back. The graphics chip's
  clock, temperature and power are not told to a title, so they are not shown.

Fixed:
- **Switching on an on-screen figure (FPS, speed and the rest) showed nothing:** the first
  release's defaults left the figures' place at "Not shown". It is the top right again (no figure
  is on by default, so nothing appears until you switch one on), and switching a figure on while
  the place is "Not shown" moves it to the top right. While a game is paused under the menus, a
  figure appears when the game goes on.
- **Games played one after another slowed down and hitched:** the render targets and textures of
  the game before (hundreds of MB at 6x) stayed in the graphics device's pool, which only a running
  game ages. They are let go when a game closes. The boot log says how much, and how much memory
  there is at each game's start and end, so a game that keeps something can be found.

## What v1.0.1 changed

Added:
- **USB controllers (experimental):** a DualShock 3 or SIXAXIS with its pressure-sensitive buttons,
  Xbox 360 pads, and guitars, drums and other USB pads (see Controls). Off by default; the boot log
  says how it went on the console.
- **The PlayStation 2 menu:** Settings > BIOS > Start the PlayStation 2 menu, or the library's
  Square menu: the PS2's browser and its System Configuration.

Fixed:
- **The PS2's start-up ran at double speed at 120 Hz** (frames paced by the screen) until Crux2
  noticed four seconds later. The clock now keeps a frame from coming sooner than the game's own
  rate while the screen paces, from the first frame; the boot log says once when it had to.

## What Crux2 v1.0.0 has

The first release. Built on PCSX2 v2.9.138.

Added:
- **PCSX2 on the PS5**: its x86-64 recompilers and Vulkan hardware renderer (through RADV, Mesa's
  driver, linked into the title), the software renderer, save states, cheats and patches (PCSX2's
  patch collection, widescreen and no-interlacing patches downloaded and applied), the network
  adapter and hard drive for online games, RetroAchievements (without hardcore mode).
- **paraLLEl-GS, a second GS renderer** (Graphics > Display, or a game's own options): the whole
  GS drawn on the graphics chip in compute shaders, super-sampled 1 to 16 times a pixel, optionally
  at twice the size; save states of its own.
- **Games from everywhere**: the title folder, USB drives, SMB and NFS network shares, the
  console's disc drive and USB DVD drives (a disc can be read into memory, and made into an ISO,
  CHD or CSO image); CHD, ISO, CSO, ZSO and the other usual formats.
- **A Big Picture interface** for the television and the DualSense: ten library views (Shelves,
  Grid, List, Flow, Bookcase, Table, Showcase, Wheel, Wall, Discs), search, filters, orders, quick
  views, favourites, hidden games and collections; covers, title screens and pictures of play from
  libretro's thumbnails; a game's details with its saved states; a quick menu over a paused game;
  four start-up animations; an animated mark; 36 skins, accent colours and living backgrounds or
  pictures of your own; interface size and text size; music of your own.
- **Settings at three levels**: Normal, Advanced and Expert (Triangle in the Settings), grouped as
  PCSX2's settings window groups them, for all games or one game alone.
- **Picture**: internal resolution up to 12x, FSR 1, NVIDIA Image Scaling and CAS, Automatic
  quality (the resolution lowered by itself while the graphics chip falls short), frame generation
  (Crux2's own or AMD FSR 3's optical flow), black frame insertion, 4K output at 60 or 120 Hz, frame
  pacing by the screen, CRT, composite, S-Video and film-grain looks or a `.slangp` preset of your
  own, HD texture packs (from the title folder, a USB drive or a share, or downloaded from the
  Internet Archive's collection).
- **Saves**: a memory card for each game, a card manager (saves brought in from a PlayStation 2 on
  the network, USB drives, shares or card images; copied, deleted, undone), sleep-safe saving,
  states saved by themselves with "Undo the last load", backups to a share or a USB drive, and
  memory cards kept in sync both ways.
- **Controllers**: up to four players with the multitap, controller profiles, pressure from R2,
  GunCon 2 aimed by moving the controller, the USB microphones of karaoke games, a USB keyboard, light bars in the
  players' colours, and control from a phone.
- **Care for an OLED screen**: dark after a while with nothing pressed, then a black screen with the
  mark, the hints fading, everything moved a few pixels now and then, dark grey bars beside a 4:3
  picture.
- **Updates**: Crux2 looks for a newer release on GitHub at each start (or when asked), shows what
  is new, downloads it, checks it and replaces only its own files.
- **A free BIOS when you have none** (OpenPS2BIOS; early work), your own BIOS always first.
- **Native code modules** (experimental, below).
- **For finding problems**: the boot log on the screen and copied to a USB drive, a notice after a
  crash, a profiled start, and a summary of each session in the boot log.

Default settings: 6x internal resolution with FSR 1 from 4x and Automatic quality, the widescreen
and no-interlacing patches, anti-blur, disable interlace offset, rasterizer ordered view, texture
packs loaded and precached, 4K output at 60 Hz paced by the screen with no frame queued, no
on-screen display, fast boot, game fixes, compatibility patches and recommended picture settings,
the main threads on cores of their own, a memory card for each game, sleep-safe saving, the screens
dark after 3 minutes and then the black screen with the mark, shaders built quickly first and the
graphics driver on its own thread. Everything can be changed in Settings.

## What was checked (as the README had it at v1.0.18)

On the PC it is built on, for this release:

- `ps5/tests/run-host-tests.sh`, with the sanitizers: the memory code (guest memory, the code area,
  the fastmem window, page protection and the fault handler), the libc stand-ins against the PC's
  own libc, the game list's letter jumps, the threads' processor times, the profiler, the memory
  cards, the card manager, the file layer and the updater. All pass.
- `ps5/tests/bigpicture/run-bigpicture-tests.sh`: the interface's screens built for the PC; with
  the address and undefined-behaviour sanitizers every settings row changed in every way and two
  runs of 60,000 random button presses across all screens; then a picture of each screen, looked
  at after each change. All pass.
- `ps5/tests/uiaudio/run-uiaudio-tests.sh`: the interface's sounds and music. All pass.
- `ps5/tools/check-imports.py`: what the title imports, by provider (428 functions); the libc
  functions known to be missing or faulting on the console are bound to the platform layer's.
- The release check (`ps5/tools/make-release-zip.py`): every part's licence and published source
  are in the folder, and no game, BIOS, key or save.

On my console (one PS5), with the development builds before this release: games start and play
from the internal storage, USB drives, an SMB share, the disc drive and a USB DVD drive; the
network adapter, the hard drive, RetroAchievements, downloaded cheats and patches, covers,
backups to a share and memory cards work. The heaviest scene I test with (a 60 fps patch, full
blending accuracy, the rasterizer ordered view, 6x) held 100% speed for all but four dips of about
three seconds in a six-minute run, 59.2 frames a second on average, with the time stretcher at
its nominal tempo 95-100% of the time. The test windows of the boot log ([test runs](../dev/debugging.md#test-runs)) are
where such figures come from. Not tried on the console one by one: every setting, paraLLEl-GS in
every game, the native code modules, and many of the PS2 accessories.
