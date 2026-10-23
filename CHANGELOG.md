# Crux2 - what changed

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
