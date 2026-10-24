# Crux2 - what changed

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
