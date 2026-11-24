# Crux2
<img width="1920" height="1080" alt="Bez nazwy" src="https://github.com/user-attachments/assets/25fe5dd6-4552-4993-bc68-d25dc65e510d" />


[PCSX2](https://github.com/PCSX2/pcsx2), the PlayStation 2 emulator, as a native PS5 homebrew
title (`PPSA99249`). It starts straight into a Big Picture style interface on the TV, is driven
with the DualSense, renders with Vulkan (RADV, Mesa's driver, linked into the title), runs PCSX2's
x86-64 recompilers, and reads games from the title's own folder, from USB drives, from SMB and NFS
network shares, from the console's disc drive and from USB DVD drives. It needs no HEN payload.

**Crux2 by Press5elect.** Copyright (C) 2026 Press5elect. GPL-3.0-or-later, with one additional
term (section 7(b)): whoever passes on Crux2's code, or a work made from it, in whole or in part,
keeps the credit "Crux2 by Press5elect", with the help of an AI tool or not. `NOTICE` has it in
full.

It is built on Mihawk's [PS5 Vulkan Template](https://github.com/mihawk-99/PS5_VulkanTemplate)
and the stack behind it: PS5_Vulkan (the RADV archive, the link recipe, the packaging tool,
`libc.prx`), PS5_Mesa, and the PS5 fork of the payload SDK with its platform layer.

> **No piracy.** This package contains no games, no BIOS, no firmware and no keys, and none
> will be provided or linked to. Use only backups you made of games you own, and a BIOS dumped
> from your own PlayStation 2. See `LEGAL.txt`.

> **An unofficial port.** This is not a build of the PCSX2 project and is not supported by it.
> Please don't report its problems to PCSX2. PCSX2's rules for contributors (`AGENTS.md` in its
> repository) also forbid posting AI-generated pull requests, issues or comments on its GitHub,
> and ask contributors to say when an AI system was used. Parts of this port were written with
> one, so nothing from it goes upstream.

> **Tested on one console, mine.** The releases' development builds ran on my PS5: games from the
> internal storage, USB drives, an SMB share, the disc drive and a USB DVD drive play; the network
> adapter, the hard drive, RetroAchievements, cheats, covers, backups and memory cards work. In the
> heaviest game I test with, at 6x with full blending accuracy and the rasterizer ordered view, a
> six-minute run held 100% speed for all but four short dips (59.2 frames a second on average).
> Not every setting has been tried on the console one by one, and what is marked *new* below has
> not run on a console yet. If it stops: [If something goes wrong](#if-something-goes-wrong).

What changed in each version: [`CHANGELOG.md`](CHANGELOG.md) (also in the title folder, and in
Settings > Network > Updates > What's New); the release notes are in [`docs/releases/`](docs/releases/README.md).
Building Crux2 and working on it: [`docs/`](docs/README.md).

## What you need

- A jailbroken PS5 with kstuff, and ShadowMountPlus (or another launcher that runs titles from
  `/data/homebrew/`).
- A PlayStation 2 BIOS, dumped from your own console.
- A DualSense (a DualShock 4 works the same way).
- For USB drives only: the payload loader (elfldr) listening on port 9021, as kstuff setups
  run it.

## Installing

1. Copy the `PPSA99249` folder to `/data/homebrew/PPSA99249/` (over FTP).
2. Put your BIOS in `bios/`.
3. Put games in `games/`, on a USB drive, or name a network share
   ([Where the games are](#where-the-games-are)).
4. Start Crux2 from the home screen.

To update, copy the new folder over the old one, or let Crux2 do it (Settings > Network > Updates >
Check for Updates: it downloads the release from GitHub, checks it and replaces only its own
files, removing those a new version no longer ships). Your settings, memory cards, save states and
`network.cfg` are files the build does not contain, so they stay as they are. To have them out of
the title folder altogether: [Outside the title folder](#outside-the-title-folder).

The files in the ZIP are all marked readable, writable and runnable for everybody (mode 777):
the console starts a title only when its files are open like that, and an FTP program that
keeps the modes of what it unpacks then uploads them so.

## Where your files go

Everything is in the title's folder, `/data/homebrew/PPSA99249/` (which the title sees as
`/app0`), and everything the title writes there can be fetched and replaced over FTP:

| File or folder | What |
|---|---|
| `bios/` | Your BIOS |
| `games/` | Your games |
| `memcards/` | Memory cards (PCSX2 makes two 8 MB cards the first time a game starts; with "A memory card for each game", one named by each game's serial) |
| `sstates/` | Save states (`<serial> (<CRC>).auto.p2s`: the one saved by itself; `.undo.p2s`: the game before the last load) |
| `snaps/` | GS dumps, the last frame of a test run, and Crux2's screenshots (Options + Square in a game, in `snaps/<serial>/`; the console's Create button still works too) |
| `covers/` | Cover pictures |
| `pictures/` | Each game's title screen and a picture of it being played (`<serial>.title.png`, `<serial>.snap.png`), fetched from libretro's collection |
| `about/` | What is known of each disc, looked up on Wikipedia and Wikidata |
| `backgrounds/` | Yours, if you want them: pictures for behind the screens (PNG, JPEG or WebP; Settings > Interface > Skins > Background > My pictures) |
| `cheats/`, `patches/` | Your own cheat and patch files (`.pnach`); a cheat typed in on the console is in `cheats/<serial>_<CRC>_typed.pnach` |
| `saves/` | Save files to bring in to a memory card, and those written out from one (`.psu`, `.max`, `.cbs`, `.sps`, `.xps`, `.psv`): [The saves on a memory card](#the-saves-on-a-memory-card) |
| `card-undo/` | Each memory card as it was before the card manager last changed it |
| `gamesettings/` | Settings made for one game |
| `inputprofiles/` | Controller presets |
| `textures/` | Texture replacements (also read from a USB drive or a share: [Texture packs](#texture-packs)) |
| `cache/` | The game list, compiled shaders |
| `inis/PCSX2.ini` | All settings |
| `network.cfg` | The network shares to read games from (written on the first start) |
| `startup.wav`, `music/`, `background.wav` / `.mp3` / `.ogg` / `.flac` | Yours, if you want them: a start-up sound in place of the title's own, and the music of the menus |
| `cheat-sources.txt` | More places to fetch cheats from (written on first use) |
| `inis/DEV9hdd.raw` | The PlayStation 2's hard drive, once made in Settings > Network > Network & HDD |
| `crux2-boot.log` | The last run's start-up steps, the emulator's warnings and errors, and a crash report if the title stopped. *New:* by default it is written only when something went wrong or you choose Save the log ([The logs](#the-logs)) |
| `crux2-boot.previous.log` | The same for the run before |
| `logs/emulog.txt` | The emulator's own log |
| `radv-shader-cache/` | The graphics driver's compiled shaders |
| `symbols.txt` | The names of the program's functions, for a profiled start (it comes with the build and belongs to its `eboot.bin`) |
| `resources/` | Shipped with the build: fonts, the game database, the interface's pictures and sounds, shaders, the startup video |
| `README.md`, `CHANGELOG.md`, `LEGAL.txt`, `licenses/` | This guide, the list of changes, the notices: every part, its licence and where its source is |

### Outside the title folder

If there is a folder **`/data/crux2`** when Crux2 starts, and it can be written to, Crux2 keeps its
files there and not in the title folder: `inis/`, `memcards/`, `sstates/`, `bios/`, `covers/`,
`pictures/`, `cheats/`, `patches/`, `gamesettings/`, `inputprofiles/`, `textures/`, `snaps/`,
`cache/`, `logs/`, `saves/` and `card-undo/`. The title folder can then be deleted and replaced by
a new version without losing anything.

- Nothing is moved for you. Make the folder over FTP and copy into it what you want to keep
  (at least `inis/`, `memcards/`, `sstates/` and `bios/`); what is not there starts empty.
  Without a BIOS in `/data/crux2/bios/` no game starts.
- `games/` in `/data/crux2` is searched as well as `games/` in the title folder.
- `network.cfg`, `cheat-sources.txt` and `backgrounds/` are taken from `/data/crux2` when
  they are there, else from the title folder.
- The boot log, `startup.wav` and the background sound stay in the title folder.
- Settings > System > About > **Data Directory** says which folder is in use, and the boot log has a line
  `data folder: ...`. Delete or rename `/data/crux2` and the title folder is used again.

**Settings > Games and saves > Folders** goes further: *Where Crux2 keeps its files* picks a folder of your choice
for the BIOS, memory cards, save states, screenshots and GS dumps, cheats, covers and texture
replacements (PCSX2's own `[Folders]` settings; checked for writing first, used from the next game
started), and *Add a game folder* adds game folders anywhere ([Where the games are](#where-the-games-are)).

## The interface

PCSX2's own Big Picture interface is not in this build. In its place is the front end written
for PSFlyCast, made over for a PlayStation 2 emulator. The top bar's tabs (L1 / R1) are **Home**
(the first screen after the start-up: the game played last with **Resume**, which starts it from
its newest saved state, and **Game page**; what goes on right now, such as a texture pack being
downloaded, which Cross there opens the texture pack manager on, or a disc in a drive; the games
played lately), **Library** and **Disc** (when a disc is in a drive). **Options** opens the
settings from every screen. The library has a filter
row under the bar (Up from the first row of games reaches it): **All**, **Console**, **USB** (when
USB drives are on), **Share**, **Favourites**, **Never played** and **Collections**, and at its
right the number of games, their order and the view; the views without the game's details show
them in a strip at the bottom. There is a game's
page with its tabs (its saved states, its own settings, its patches and cheats), a loading screen with the progress of a disc image being read, a
quick menu over the paused game, and one settings screen that serves every game and, opened from a
game, that game alone (see **The settings** below). The name in the top bar is drawn by the
interface: C, R, U, the mark, 2.

**The library has ten views** (the touch pad goes through them; Square > View chooses one):

- **Shelves**: the games last played, your favourites, each collection, then all of them, with
  the focused game's picture behind its title.
- **Grid**: every cover, eight to a row.
- **List**: titles on the left, the focused game with what is known of it on the right.
- **Flow**: the covers in one row that flows past, the focused one facing you.
- **Bookcase**: the games as books on shelves, their backs out with the titles up the spine;
  the focused one is pulled out and shows its front.
- **Table**: a row for each game with its region, how well it is known to run, when it was
  last played, for how long, and its size.
- **Showcase**: the focused game large, as a console's dashboard shows it: its cover and what is
  known of it over its picture of play, and a strip of covers along the bottom.
- **Wheel**: the titles on a wheel at the right that turns past the focus, the game's art, cover
  and facts at the left.
- **Wall**: every cover, small and close together, twelve to a row; the focused one stands out.
- **Discs**: the games' discs in a row, the focused one large and turning as in a drive.

The discs of one game are one entry. A game closed with "Quit, and save its state" offers to go
on from there the next time it is started, and the game played last carries a **Continue** mark
on the shelf.

**Square in the library** opens the list's menu:

- **Search by name**, typed on a keyboard on the screen.
- **Quick views**: what the list is narrowed to and its order, kept under a name. **R3** in the
  library goes from one to the next and, after the last, back to all games. Three are there to
  begin with (Favourites, Last played, Never played); "Keep what is shown now as a quick view"
  adds your own, up to eight.
- **Order**: by name, last played first, most played first, largest first, newest file first.
- **Show**: all games, favourites only, the hidden ones.
- **Filters**, several at once: a region, played or never played, playable or better (or not
  known to run), and only games with saved states, with cheats, or with a texture pack in the
  title folder.
- **Collection**: only the games of one of your collections; renaming and deleting them is
  here too.
- **View**, **A game at random** (it opens that game's page), the scan for new games, and the
  PlayStation 2 menu.

What is set shows as a chip at the top right; **Circle** shows all games again.

**A game's page** (Triangle) has its cover with **Resume** (from the state it was closed with, or
its newest saved state) and **Start from the beginning** under it (**Play** for a game with no
state), its name with what is known of it (region, serial, how well it runs, the time played, when
it was last played), and tabs, **L1** / **R1**: **Overview** (where you left off, how many of its
own settings differ from the global ones, its memory card, its patches and cheats on, its texture
pack, its file, and what it is, as the Disc tab says it), **Save states** (start from one with
Cross, delete one with Triangle; the picture each was saved with is in the cover's place), **Its
settings**, **Patches and cheats** and **Texture packs** (each opens that game's page of the
settings, or the texture pack manager on it). **Left** and **right** go between the cover's buttons
and the tab's rows; **L2** / **R2** put the game's title screen or a picture of it being played in
the cover's place, where it has them; **Circle** goes back. **More** (Square) has:

- **Start with...**: the game straight away, or the console's own start-up first.
- **Rename**: another title for the lists, typed on the keyboard (PCSX2's own custom title);
  "Its own name again" undoes it.
- **Collections**: a collection is a shelf of your own, a name and the games you put on it.
  The list ticks the ones the game is in; "A new collection" makes one. Up to 24, kept with
  the settings; a game is known by its serial, so it stays in its collections when its file
  moves to another drive.
- **Fetch its pictures**: looks for this game's title screen and picture of play again.
- **A favourite** (a star on its cover, and a shelf of its own) or not any more; **Hide it** from
  the library, after asking (Show > Hidden games brings the hidden ones up, where More > **List it
  again** lists a game again).
- **The next disc** of a game of several discs, **Make an image of the disc** for the disc in a
  drive, and, for a game closed with a state, **Delete the state it was closed with, and start from
  the beginning**.

Favourites and custom titles are PCSX2's own (its game list keeps them); hidden games,
collections and quick views are kept with the settings.

**The games' pictures** come from libretro's collection of thumbnails, by the game's name, as
the covers do: after each scan for every PlayStation 2 game that has none yet (Settings > Games
and saves > Game List > **Download Game Pictures Automatically**), or at once with **Download Game
Pictures** there. They show on a game's page, behind the title in the shelves view (Settings > Interface > Library layouts > **Show
Gameplay Background**), and behind every screen with the background "The game's picture". A
game the collection has nothing for is remembered and not asked about again until you ask. Covers
and pictures are downloaded three at a time, those on the screen first (*new*).

**The keyboard on the screen** is the interface's own: the directions move over its keys, Cross
types one, Square takes the last letter away, Triangle is the space, L1 the capitals, R1 the
signs, Options accepts and Circle leaves. A password is shown as dots; L2 shows and hides it.

**The settings** (Options in the library, Settings in the quick menu) open on a home: a search bar,
then eight areas, **Picture**, **Sound**, **Controls**, **Games and saves**, **Emulation**,
**Network**, **Interface** and **System**, each card saying what is in it, what is set now and how
many of its settings differ from their defaults ("3 changed"). Under them, **Pinned** (the settings
you pinned, with their values) and **Changed lately** (the last three you changed); Cross on one of
them goes to it. Triangle on the home shows only what is changed, everywhere, until pressed again.

- **An area's page** has its sections on the left (L2 / R2 go from one to the next, a dot marks one
  with something changed), its settings in the middle, one line each (a dot when changed, the value
  on the right, in the accent colour when it is not the default; L1 / R1 jump between the groups of
  a long section, such as a controller's buttons), and on the right the focused setting described in
  full: its description, its choices with the current one marked, its default, what it applies to
  (all games, or this game) and when it takes effect (at once, at the next game start, or at the
  next start of Crux2). Left and right change a value, Cross runs a row or opens its list, Circle
  goes back to the home.
- **Levels**: Essentials, More and Everything (the three levels of before, Normal, Advanced and
  Expert, under new names, kept in the same setting). Triangle on an area's page goes through them;
  the top bar shows which is on.
- **Search** (Square on the home): type on the keyboard on the screen; every setting at every level
  is looked through, by its name, its description and the names of its choices, and the library's
  games too. L1 / R1 choose Everything, Settings, Games or Changed from Default. On a result, Cross
  (and left and right) change it where it is, Triangle goes to its page (raising the level when it
  is shown only at a higher one), Square pins it; a game opens its own settings.
- **Pins**: Square on any setting pins it to the home, or unpins it.

Elsewhere in this guide, "Settings > Picture > Graphics" and the like name an area and its section;
the search finds any setting by a word of its name or description.

**Game Specific Settings** (on a game's page, or in the quick menu) are the same
screens for that game, opening on Games and saves, whose first section is the game itself (its
serial, its file, what the database recommends, clearing its settings). A setting the game does not
have uses the global setting and is shown fainter; changing it gives the game a value of its own,
marked with a dot and "This game", and **L3** takes it away again (L3 also clears a button's binding).
The switch at the top (the **touch pad**) goes between this game's settings and those of all games,
where a "This game" tag marks what the game overrides. They are PCSX2's per-game settings files, in
`gamesettings/`, as its desktop builds write them. The game's patches and cheats are under Games and
saves (Patches, Cheats) when `resources/patches.zip` or `cheats/` has any for it.

Every setting's description is PCSX2's own where PCSX2 has the setting, and is written in its
style where it has not.

**The look.** Settings > Interface > Skins has 36 skins (six of the title's own: **Crux**, its warm black,
orange and cream, is the default; Midnight is the look of the first builds; Carbon is black, for an
OLED screen; and the 30 themes of the ps5-homebrew-ui kit), an accent colour, the amount of motion,
interface and text size, high contrast and colours for colour blindness, and the background: still,
alive in one of six ways, the focused game's cover colours or its picture, or **My pictures**. For
those, put PNG, JPEG or WebP files in a folder named `backgrounds` in the title folder: with several,
one follows the other (every 30 seconds to every hour) and fades in over it, and they are made
darker by as much as you choose so that what is written over them can be read. A picture is never
held larger than 1920 by 1080, and two are held at a time.

**The start-up.** There are four animations, each building the name out of its parts in time
with the start-up sound and delivering it to the top bar in its own way: **Strike**, **Unfold**,
**Spin** and **Slash**. A start plays another one than the start before it; Settings > Interface > Interface >
**Startup Animation Style** keeps one. Any button ends it early. *New:* a short startup video of Crux2's own
plays in the animation's place (Settings > Interface > Interface > **Startup Video**; it needs the start-up
animation on and Motion at All). The sound comes with the build (`resources/sounds/silicon-boot.wav`);
`startup.wav` in the title folder, if you put one there, is played in its place. When the start-up
is over, music plays for as long as no game is loaded: the files of a `music` folder in the title
folder (WAV, MP3, Ogg or FLAC, in the order Settings > Sound > Interface sounds > **Music Order** sets) and
`background.wav`, `.mp3`, `.ogg` or `.flac`, or else the music that comes with the build. Settings >
Sound > Interface sounds has a switch and a volume for each, Next track, and says which files were found.

**Statistics** (Settings > System > Statistics) counts what the library holds and what was played: the
games by kind, place, region and how well they are known to run, their size, the time played in
all, and the five games played the longest.

**Circle in the library** asks "Close Crux2? Are you sure?" and closes the title on Yes.

Not in this interface, because a controller cannot drive them or the console has no use for
them: the USB devices of a PlayStation 2 beyond those below, video capture, input profiles and
macro buttons. The settings behind them are still read from `inis/PCSX2.ini` if you write them
there.

## Screen care

An OLED television keeps a trace of what stood still on it for hours, and a menu stands still
whenever nobody touches the controller. Settings > Interface > Screen care:

- **In the menus, and over a paused game under the quick menu** (on by default): the screens go
  dark when nothing was pressed for a while (1 to 10 minutes; 3 to begin with), and after as long
  again they are black with the mark wandering, until a button is pressed (that press does nothing
  else). Everything on a screen moves by a few pixels every other minute. The hint bar's words go
  faint after a quarter of a minute without a button. The skin Carbon is black.
- **While a game is played** (off by default): the picture can be made darker when nothing was
  pressed for 5 to 30 minutes (a film a game plays by itself is darkened too, which is why it is
  off); the bars beside a picture narrower than the screen can be dark grey in place of black; and
  the emulator's own figures (speed, frame rate) can go to another corner every five minutes. The
  game's picture itself is not moved.

None of this has been measured on a television. The television's own screen saver and pixel shift
work as well; leave them on.

## Controls

The bar at the bottom of each screen names what the buttons do there.

**In the library:**

| | DualSense |
|---|---|
| Move | D-pad or left stick |
| The tab before / after (Home, Library, Disc) | **L1** / **R1** |
| The filter row (All, Console, USB, Share, Favourites, Never played, Collections) | **up** from the first row, then **left** / **right** and **Cross**; **down** or **Circle** back |
| The previous / next letter (hold to keep going) | **L2** / **R2** |
| The next view (ten of them, shelves to discs) | click the **touch pad** |
| The next quick view; after the last, all games again | **R3** (press the right stick) |
| The game's page: its tabs, saved states, its own settings, More | **Triangle** |
| The list's menu: search, quick views, order, filters, collections, view, a game at random, scan, the PlayStation 2 menu | **Square** |
| On a game's page: the tab before / after | **L1** / **R1** |
| On a game's page: its cover, its title screen, a picture of play | **L2** / **R2** |
| On a game's page: More (start with, rename, collections, favourite, hide, ...) | **Square** |
| Settings | **Options** |
| Start the game | **Cross** |

**In the settings:** up and down choose, left and right change a value, **Cross** switches or
opens a list of choices, **Circle** goes back. In a game's own options **Square** takes the
game's value away again; on a controller button it removes the button. Settings > Interface > Interface >
**Swap OK/Cancel** exchanges the two in every menu.

**In a game** the DualSense is a DualShock 2, button for button, with pressure-sensitive
buttons reported as fully pressed, and rumble:

| PlayStation 2 | DualSense |
|---|---|
| D-pad, sticks, Cross, Circle, Square, Triangle, L1, R1, L2, R2, L3, R3 | the same |
| Select | click on the **left half** of the touch pad |
| Start | click on the **right half** of the touch pad |
| *Quick menu* | **Options** |

The Create and PS buttons belong to the console and never reach a title, which is why the
touch pad stands in for Select and Start. Every button can be changed: a game's in
Settings > Controls > Controllers, the quick menu's in Settings > Controls > Hotkeys. **Reset Bindings**
on the controllers page brings this layout back. A quick tap is held for the game at least
50 ms, so games that read the pad once a frame see it (*new*).

**The quick menu** says how long the game was played this time and has a row of tiles: **Resume**,
**Save state** and **Load state** (the slot and when it was saved), **Screenshot** (the game's
picture without the menu, as Options + Square takes it), **Change disc** and **Quit game**, which
asks whether to save a state to go on from and returns to the library. Under them, **Your pinned
settings**: the settings pinned with Square in the Settings, changed in place with left and right
(the game's own value, where a game can have one; down from the tiles, or L1). And **More** (R1, or
down from the right-hand tiles): the slot to use, **Load the auto-save** and **Undo the last load**
when there is one, **Achievements** and **Leaderboards** for a game that has them, **Record a GS
dump**, **Save the log**, the game's settings, its **Patches and cheats**, the settings, fast
forward and restart. Triangle chooses what More shows and in which order (a game's own settings can
be put there too), and **Apply changes now**, at the top of More, restarts the game when a setting
changed that only takes effect at a game's start. An exit, a restart or a state load waits for the
writes still under way, with a bar. Settings > System > About > **Exit** ends the title.

**Players 2 to 4** are the controllers of the other signed-in users: the user who started
Crux2 is player 1, the others follow in the order they signed in (sign each one in with the
PS button of their controller). Settings > Controls > Controllers > Players puts them on the PlayStation
2's ports in one step, in the three ways games expect: **One or two players** (ports 1 and 2),
**Up to four players, multitap in port 1** (1A to 1D, what most four-player games want) and
**Up to four players, multitap in port 2** (1, then 2A to 2C). A running game sees a multitap
the next time it starts. (In `inis/PCSX2.ini` the controllers keep the names PCSX2's desktop
builds give them, `SDL-0` to `SDL-3`, so settings carry over.)

**More in Settings > Controls:**

- **Light bar in the player's colour** (off): each controller's light bar says which player it is,
  in the colours the PlayStation 4 used: blue, red, green, pink.
- **Pressure from R2**: the face buttons and directions pressed as hard as R2 is pulled. PCSX2's
  "pressure modifier" (a button held to press the others lightly) is there too.
- **Aim with motion**: the GunCon 2 aimed by moving the DualSense (aim mode, speed, smoothing,
  "Measure the screen").
- **Aim with a PlayStation Move (Move Management)** (off; *new*): player 1 aims the GunCon 2 with a
  PlayStation Move, and a Navigation controller works its D-pad. It needs the Move Management
  payload running; Select on the Move recentres. The boot log's `move:` lines say how it went.
- **USB keyboard**, and the **USB microphones** of karaoke games.
- **USB controllers (experimental**, off**)**: controllers the console does not take itself,
  plugged into its USB ports: a **DualShock 3 or SIXAXIS**, with its pressure-sensitive buttons,
  rumble and player light; **Xbox 360** pads; and **guitars, drums and other USB pads** that
  describe themselves as a game pad or joystick. They are players 5 to 8 (`SDL-4` to `SDL-7`). Each
  port's page has **Map it to a USB controller**; for the rhythm games, set the port's controller
  type to **Guitar** and map it. This has not been tried on a console; the boot log's `usb pads:`
  lines say how it went, and when the console refuses, only its own controllers are read.

**Control from a phone** (Settings > Network > Remote Control): a page on your phone or computer, reached by a QR
code, works as a controller.

**The PlayStation 2 menu:** Settings > Games and saves > BIOS > **Start BIOS**, or the list's menu
(Square in the library), starts the BIOS with no game: the PS2's browser (its memory cards and
discs) and its System Configuration. **Quit game** in the quick menu goes back to the library.
Settings > Games and saves > BIOS also has **Console Language**: the language games are told the PlayStation 2 is
set to ("As the BIOS has it" keeps the BIOS's own, which is never changed).

## Where the games are

Disc images as `.iso`, `.chd`, `.cso`, `.zso`, `.gz`, `.bin`, `.mdf`, `.nrg` and `.img`.

Each place below has its chip in the library's filter row (All shows every place at once). The games are looked for when Crux2 starts and after
each game; **Square** in the library (or Settings > Games and saves > Game List > **Scan For New Games**) looks again.
Settings > Games and saves > Folders > **Add Search Directory** opens a folder browser (USB drives, the M.2 storage, network shares, the
console's storage) and adds a folder anywhere; it and its subfolders are searched, and the games
stay where they are.

### The title's folder

`games/`, with any subfolders you like.

### USB drives

Games are read from a folder named `crux2`, `ps2`, `ps2iso` or `playstation2` at the top of the
drive (capital letters or not; folders named `pspcsx2` and `pcsx2` are read as well), or from a
folder added in Settings > Games and saves > Folders. Only those folders are scanned, not the whole drive.

The console's M.2 storage (`/mnt/ext1`) and the internal storage's `/mnt/ext0` are read the same way:
the same folder names at their top, or a folder added in the folder browser. Games found there are
under the library's Internal chip.

A title cannot see USB drives or the M.2 storage from inside its sandbox. When Crux2 starts, it sends the bundled
`sandbox-elevator.elf` to the payload loader on port 9021, which lets this one title (by its ID)
out of its sandbox. If the loader is not running, Crux2 says so in the boot log and carries on
without USB drives; nothing else changes.

This is on by default. Settings > Games and saves > Game List > **Search USB Drives** turns it off (it takes effect at the
next start); so does an empty file named `usb-off.txt` next to `eboot.bin`, should the interface
not come up.

### Network shares (SMB and NFS)

Edit `network.cfg` in the title's folder. Crux2 writes this template on its first start:

```
# path = 192.168.1.10/Games/PS2
user = guest
password =
```

- `path` is server/share/folder. Give the server by its **IP address**; do not rely on a name
  being looked up on the console. Remove the `#` and write one `path` line for each folder of
  games.
- For an open share leave `user = guest` and the password empty; otherwise give the account on
  the NAS or PC. One account serves all paths.
- Each folder is shown as `/smb/<the folder's name>` and is added to the game list by itself.
- SMB 2 and 3 are spoken (not the old SMB 1). The share is only ever read for games.
- Restart Crux2 after changing the file.

A game on a share is read from it as it plays, so the link to the NAS matters while playing: a
wired connection is the safe choice. Settings > Emulation > Emulation > **Enable CDVD Precaching**
loads the whole image into memory before the game starts instead; the start then takes as long as
copying the image does, with a progress bar, and the NAS is not needed again. A NAS that closes a
connection that was idle is connected to again the next time anything is asked of the share.
Memory cards, save states and settings are never on the share: they stay on the console.

*New:* the shares are woken when Crux2 starts. A game on a share that does not answer waits for
it, with a count on the screen, and a read stuck for 2 seconds while a game runs is said on the
screen (the picture keeps being shown meanwhile).

## A disc in a drive: the console's own, or a USB DVD drive

A PlayStation 2 game in a disc drive is shown on a fourth tab, **Disc**, with a turning disc:
gold for a DVD, blue for a CD. Play starts it from the drive. A disc that is not a PlayStation 2
game is named for what it is and not played.

- **The console's own drive** reads PlayStation 2 DVDs (silver underside). It does not read CDs.
- **A USB DVD drive** plugged into the console reads PlayStation 2 DVDs and also the games that
  came on CDs (blue underside). It works on a console without a drive of its own too. Plug it in
  before or while Crux2 runs: within two seconds its disc is on the Disc tab, and pulling it out
  takes it away. With a game in each drive, both are on the tab. Audio tracks are silence
  (PlayStation 2 games on CD keep their music in their data).
- **A locked tray.** Started without a jailbreak, the console locks a USB drive's tray when a
  disc is in it. Crux2 unlocks every drive's tray when it first sees it, and Settings > Games and saves > Game
  List > **Eject Disc** unlocks a drive and opens it (not one a game plays from).
- **Make an image of the disc** (Settings > Games and saves > Game List > **Create Disc Image**, Square on the Disc tab, or "Make an image"
  in More on the disc's page) asks which kind: ISO, CHD smallest (LZMA), CHD quickest to load
  (Zstandard) or CSO; then where it goes: the internal storage, a USB drive, or a network share. It
  is named "Title (SLUS-12345).chd" (or .iso, .cso) and is in the library after the next look for
  games.
- **The Disc tab** shows the disc with its cover, title screen and a picture of it being played, a
  few sentences of the game from Wikipedia, and its developer, publisher, year and genre from
  Wikidata, kept in the data folder's `about/`; Settings > Games and saves > Game List > **Show Game
  Descriptions** switches that off.

**Experimental.** It ran on one console (mine). Settings > Games and saves > Game List > **Search Disc Drives** (or
`DiscDrive = false` under `[PS5]` in `inis/PCSX2.ini`) leaves the drives alone. The boot log says
what happened, in lines beginning `disc:`.

**Read into memory while you play.** Played straight from the drive, a game stuttered a lot on my
console. So a disc in the drive is read 8 MB ahead of the game, and behind that all of it, from
where the game is onwards, into memory; once the whole disc is there (a line on the screen says
so) the drive is not used at all. It takes as much memory as the disc is large and is done only
when that much can be spared with 2.5 GB left over. Settings > Games and saves > Game List > **Cache Disc in
Memory** switches it off (512 MB of what was read last is kept then), and **Keep Disc
Spinning** reads a little every 20 seconds so that the drive does not stop.

## The network adapter and the hard drive

Settings > Network > Network & HDD > **Enable Network Adapter** switches on the PlayStation 2's network adapter (PCSX2's "Sockets" one, the
only kind a console can have): the game gets a network of its own and its traffic goes out
through the console's connection. The page shows the console's address, what the game is doing
with the adapter, and takes the two name servers. The **first name server is PS2 Online's**
(`45.7.228.197`, ps2online.com) unless you chose one yourself. The row offers the services Crux2
knows by name (PS2 Online, and PS Rewired: `67.222.156.250`), the console's own name server, and
an address entered number by number. I have not played online with it. The hard drive is one
file, `inis/DEV9hdd.raw`, made on that page as an empty drive of 40 or 120 GB; it takes room only
as it is written to.

On my console the adapter and the hard drive worked. A game's pings are not expected to work (the
console refuses the socket for them); that does not stop TCP or UDP. The game's own address, mask
and gateway cannot be set (the Sockets adapter answers its DHCP itself).

## Settings worth knowing

- **Graphics:** the Vulkan renderer at 6x the PlayStation 2's resolution by default, with
  Upscaling and sharpening at FSR 1 from 4x: while FSR is chosen, the game is drawn at 4x and FSR
  enlarges it to the output, so the 6x takes effect when FSR is switched off. Automatic quality
  lowers the resolution by itself where the graphics chip falls short. The software renderer and
  **paraLLEl-GS** (the whole GS drawn on the graphics chip in compute shaders, super-sampled 1 to
  16 times a pixel) are the other choices of Settings > Picture > Graphics > **Graphics API**.
- **Memory cards are held in memory.** A game reads and writes a copy of the card, and what
  changed is written to the card's file half a second after the game stops writing, every two
  seconds while it keeps writing, when the game is paused, and when it is closed. "Memory Card ...
  was saved to storage" appears over the game once the file has the data. Closing the title from
  the console's menu before that message can lose up to the last two seconds of a save: wait for
  it, or pause first. Settings are written when the menus close, and at a game's start, quit and
  crash (*new*).
- **Speed hacks are as PCSX2 ships them.** **Enable Multithreaded VU1 (MTVU)** (Settings > Emulation >
  Emulation, at More) is on, as PCSX2 sets it on a machine with enough cores: safe for most games, wrong
  for a few. If a game hangs, try it off for that game.
- **The performance overlay** (Settings > Picture > On-Screen Display; off until a figure is switched on).
  **Performance Overlay Size** picks how much of it there is: *Minimal*, a small pill with the frame rate;
  *Compact* (the default when you switch the FPS on), one bar with the frame rate and the GPU, EE,
  GS and VU loads; *Full*, a card with the frame rate large, the frame time and speed, the 1% and
  0.1% lows, a graph of the last frame times, the loads as bars, and the internal resolution,
  processor temperature and memory at the foot. A size switches the figures under Show for you;
  change any of them and the size becomes *Custom* (the card, with what you switched on). Colour
  means something only: white is fine, amber close to a limit (a thread or the GPU at 80% or more,
  speed under 98%, a frame a little late), red over it (95% or more, speed under 90%, a frame half
  as long again as it should be: the graph's spikes). With frame generation running the rate reads
  "30 → 60" (the game's, then the screen's). Style still has PCSX2's own lines of text; the
  overlay's place (any corner, top centre...), OSD scale and margin, and moving it from corner to
  corner work as before. Its readability on a television has not been checked on a console yet.
- **When a game is slow, the overlay says which part is the limit.** Settings > Picture > On-Screen Display:
  turn on **Show Speed Percentages**, **Show FPS**, **Show CPU Usage** and **Show GPU Usage** (or Performance Overlay Size: Full). Then:
  - *Speed at 100% but the game's own frame rate low:* the game itself is slow, as it would be on
    a PlayStation 2 (a 60 fps patch can ask more of the emulated processor than it has). Settings >
    Emulation > Emulation > **EE Cycle Rate** above 100% gives the game a faster emulated processor.
  - *Speed below 100% and the EE thread near 100%:* the emulated processor is the limit. Try
    Multi-threaded VU1; undo any EE cycle rate above 100%.
  - *Speed below 100% and the GS thread near 100%:* the graphics thread is the limit. Settings >
    Picture > Graphics > **GS Thread Split** (on, two threads, by default, at More) puts the drawing on a
    thread of its own; the GS figure is then the thread that reads the game's graphics commands, and the
    boot log's `gs split:` lines say how busy the drawing thread is. Settings >
    System > PlayStation 5 > **Threaded Graphics Driver** (on by default, at More) takes the driver's share
    off it; with the higher blending accuracy levels, Settings > Picture > Graphics > **Rasterizer Ordered
    View** matters too.
  - *Speed below 100% and the GPU near 100%:* a lower internal resolution.

  The EE, GS and VU percentages are each thread's own processor time. When it is the graphics
  thread, Settings > System > PlayStation 5 > **Profile Next Start**, then quit and start Crux2 again,
  play the slow part for half a minute or more, and quit Crux2 from its own menu (Settings >
  System > About > **Exit**): the boot log then says what the graphics thread was busy with. That is the file to send
  with a report of a slow game.
- **Shaders compile as a game first needs them**, so a game can stutter the first time an effect
  appears. They are kept (`cache/`, `radv-shader-cache/`) and the next run has them.
- **Covers:** a picture named like the game's serial or title in `covers/` (`.jpg`, `.png` or
  `.webp`) is used as its cover. Missing covers are fetched after every scan from libretro's
  collection of box arts (`libretro-thumbnails/Sony_-_PlayStation_2`), its region's picture first,
  and saved as `covers/<serial>.png`. A game it has no box art for is asked for by its serial in a
  second collection (`xlenore/ps2-covers`) and saved as `covers/<serial>.jpg`. Settings > Games and saves > Game List has **Download Covers
  Automatically** and **Download Missing Covers** (which also asks again for the games nothing was found
  for, remembered in `covers/no-cover.txt`). A `CoverURL` of your own under `[PS5]` in
  `inis/PCSX2.ini` is used instead. A game without a cover is shown as a card with its title.
- **Cheats:** a game's Cheats page (its page > Patches and cheats > Cheats) has **Download cheats for this
  game**, and Settings > Games and saves > Cheats has **Download Cheats for All Games**. They fetch the game's
  `.pnach` files from four collections on GitHub: `xs1l3n7x/pcsx2_cheats_collection`,
  `complicatiion/pcsx2_europe_cheats_collection`, `heavyjam-code/pcsx2-cheats-n-patches` and
  `TheAdmiester/PCSX2-Cheats`. Cheats go to `cheats/`; a collection's patches go to `patches/` and
  show on the game's Patches page beside PCSX2's own. Each is saved as
  `<serial>_<CRC>_github-....pnach`; your own files are never changed. More places can be named in
  `cheat-sources.txt`. Cheats and patches stay off until you switch them on in the game's options.
- **A cheat typed in**: the same page has **Type a cheat in**: its name, then its lines one at a
  time as the lists print them (`2034A5B0 00000001`); a line with nothing typed ends it. It has to
  be a raw code. **Take a typed cheat out** removes one.
- **Game fixes from GitHub** (Settings > Network > Updates > **Update Game Fixes on Startup**, on; *new*): at each start, the game
  database entries and game patches that changed since this version was made are fetched from
  this project's GitHub, checked, and applied to the games started after they came.
- **Save states and the console's write limit:** a title writes about 1.3 GiB at full speed in
  one run and is slowed to about 2 MiB/s after that. A long session with dozens of save states can
  reach it, and a save state then takes several seconds.
- **Accurate floating point** (Settings > Emulation > Emulation > **Accurate FPU Floating Point** and its VU0 and VU1 rows,
  for all games or for one; FPU, VU0, VU1; off): the PS2's exact floating-point results, done in the recompilers, for
  games whose AI or physics depend on them. FPU and VU0 cost some speed; VU1 a lot. They are on for
  the FPU and VU0 in the two games known to need them.
- **Resetting:** delete `inis/PCSX2.ini` to start again from the defaults.

### RetroAchievements

Settings > Network > **Achievements** (on my console it signed in and found a game's achievements).
**Sign in** asks for the name and the password of your account at retroachievements.org and hands
them to PCSX2's own sign-in; what is kept is the key RetroAchievements answers with
(`inis/secrets.ini`), never the password. A game you start is then looked up; an achievement earned
is announced in a corner, and the quick menu has **Achievements** and **Leaderboards**. Hardcore
mode is not offered: saved states, cheats and slow motion stay usable. The switches for
Achievements, Encore, Spectator and Unofficial achievements can only be changed with no game
loaded (changing them under a running game from a Big Picture interface is reported to freeze
PCSX2: its issue 14899). The achievement sounds are not played: the console gives a title one
sound output, and the game has it.

### More of PCSX2's settings

Shown for all games; not tried on the console one by one.

- **Emulation:** "Fast disc access" (PCSX2's fast CDVD) for all games; the seconds of a clock set
  by hand; under advanced settings, a processor core for each emulator thread (thread pinning),
  and **Graphics driver on a core of its own** (on).
- **Graphics:** under advanced settings, internal resolutions above 12x, and three switches for a
  graphics driver that misbehaves. A game's Renderer fixes have **Target page rearranging**.
- **On-screen display:** the performance overlay in three sizes and Custom (frames with 1% and 0.1%
  lows and a frame-time graph, the loads as bars; Style has PCSX2's own lines back), the GS
  statistics, the state of an input recording.
- **Audio:** **Output latency** (5 ms up to 43) and **Surround** (PCSX2's expansion to 5.1 or 7.1;
  which of the console's channels each ends up in has not been heard, and if the console refuses
  the port the sound stays stereo).
- **Backup:** "Each backup replaces the one before it".

### 120 Hz and variable refresh

The picture is 60 Hz by default. Settings > Picture > Output and HDR > **Refresh Rate** > "120 Hz when the
television has it" (from the next start; holding L1 and R1 while Crux2 starts uses 60 Hz once) has
the title tell the console that it can show 119.88 pictures a second, and take the display's
fastest mode; it ran at 119.88 Hz on my console. Where the television does not take 120 Hz at 4K,
the graphics driver falls back to 59.94 by itself. The boot log says what came of it: `display:
120 Hz is asked for`, then either `display: 3840x2160 at 119.88 Hz` or the driver's own line about
why not (`wsi/videoout: ...`).

What it is for: the menus move at 120; a 60 Hz game's frames each stay for two refreshes; a PAL
game's 50 frames are spread over two and three refreshes, not one and two; frame generation can
double 60 to 120. Without the menus: `OutputRefresh = 120` (or `60`) under `[PS5]` in
`inis/PCSX2.ini`.

**Variable Refresh Rate (VRR)** (Settings > Picture > Output and HDR, off; experimental): at 120 Hz on a TV with VRR,
each frame is shown as soon as it is ready. Not tried on a TV that has it.

### FSR 1

Settings > Picture > Graphics > **Upscaling and Sharpening** > **FSR 1 from 4x** (the default), **from 3x**
or **from 2x**. AMD's FidelityFX Super Resolution 1.0 enlarges a picture drawn smaller than it is
shown with its edge-following filter, then sharpens it. It is for games that do not hold full
speed at 6x: close to a 4K picture for much less work. Each choice sets the resolution the game is
drawn at, whatever "Internal resolution" says; the resolution rows are greyed out meanwhile. The
boot log says once what it does: `picture: FSR 1 enlarges 1920x1344 to 3086x2160`. The same row
has NIS (enlarge and sharpen) and PCSX2's CAS. The filter is AMD's, unchanged
(`resources/shaders/common/fsr1/`, MIT).

### Anti-aliasing: FXAA, SMAA and SSAA (new)

Settings > Picture > Graphics > **Anti-Aliasing** is one choice for all of them:

- **FXAA**: quick, a little soft.
- **SMAA**: sharper edges for a little GPU time.
- **SSAA**: the picture of the internal resolution you chose made screen-sized from every texel each
  pixel covers, the least shimmer. It works when that picture is larger than the screen (6x and up at
  4K); the resolution is left as you set it. FSR 1, which would enlarge a smaller picture, is off while
  SSAA is chosen and comes back after. **SSAA filter**: **Soft** (each pixel's texels averaged, no halo) or **Sharp** (a Lanczos
  filter, crisper edges with a halo of a few percent).
- **SSAA + SMAA**: SSAA, then SMAA over the screen-sized picture for the edges it leaves.

CAS sharpening, frame generation and the look then work on the screen-sized picture. The boot log
says what it does: `picture: downsampled (Sharp) from 3840x2688 to 2880x2160`, `picture: SMAA runs at ...`.

**SMAA** (the same section, off) smooths jagged edges on the picture as it is shown: SMAA 1x
(Jimenez et al., MIT; `resources/shaders/common/smaa/`), High quality, after downsampling when
there is one, otherwise at the picture's own size. It is sharper than FXAA; it does not add
samples, so it cannot replace a higher internal resolution. Neither has run on a console yet.

### Frame generation

Settings > Picture > Frame generation > **Frame Generation** (off by default) makes pictures between the game's
own:

- **30 to 60.** Most PlayStation 2 games draw 30 (or 25, or 20) pictures a second and hand each
  one over twice or three times. The repeats are replaced by pictures made between the last two new
  ones. A game that already draws 60 is left as it is. This works on a 60 Hz screen.
- **30 to 60, and 60 to 120 at 120 Hz.** At 119.88 Hz, a made picture is also shown between every
  two pictures of a 60 a second game.

**Frame generation method:** **Crux2's own** (its motion search: the pictures searched for where
each part went, small copies first, and each pixel fetched from where it was and where it will
be), **AMD FSR 3 optical flow** (handles small, fast movement better and keeps a score or a map
still; "Optical flow quality"), or **MihawkGen** (Mihawk's small trained network, which looks at
the two pictures only and leaves what did not move exactly as it was). None of them is FSR 3's or
FSR 4's frame generation: those need the game's motion vectors and depth, which no PlayStation 2
game has. "No frame generation during videos" is on by default. **Show the movement** (advanced)
is for checking FSR 3 optical flow, not for playing. When the picture reaches the screen by a plain
stretch (no FSR or NIS), the pictures are made at the game picture's own size, which is lighter.
*New:* the on-screen display shows the pictures a second given to the screen.

**What to expect.** A turn of the camera comes out well. Where the search cannot follow, the two
pictures are mixed in place, which shows as a faint double image (around something fast in front
of something else, on a ball that spins). After a cut nothing is made up. A repeating pattern can
be matched one repeat off in places. And it costs time: with a 30 a second game everything is on
the screen one refresh later than without. Fast forward, slow motion and a paused game are left
alone. The boot log says what it does (`picture: frame generation ...`, and how many pictures it
made when the game closes).

### HDR (new)

Settings > Picture > Output and HDR > **HDR** (off) sends the picture to the TV as HDR10 (BT.2020, PQ), with the
game's brightest parts brighter: **HDR Peak Brightness** and **HDR White Level** (203 nits, the reference,
by default) set how bright, and **Adjust HDR brightness** shows a test picture to set them by. It
needs the console's own HDR setting on and a TV that takes HDR; otherwise the picture stays SDR and
a message says why. **Debanding** (Settings > Picture > Looks: Off, Low, High) smooths the colour bands in
skies, fog and dark gradients. Neither has run on a console yet.

### The saves on a memory card

Settings > Games and saves > Memory Cards > **Memory Card Manager** lists the saves on a card, each with the name
it gives itself, its size and its date, and how much room is left. From there:

- **Bring a save in from a file.** Save files are looked for in a folder named `saves` on every
  USB drive and every share, and in `saves/` of Crux2's own folder. Read: EMS (`.psu`), Max Drive
  (`.max`), CodeBreaker (`.cbs`), SharkPort and X-Port (`.sps`, `.xps`) and the PlayStation 3's
  (`.psv`, of a PlayStation 2 save). Saves also come in from a PlayStation 2 on the network.
- **Copy a save out** as a `.psu` into one of those `saves` folders, **copy it to another card**,
  or **delete** it.
- The card itself: **rename**, **make a copy**, **delete**.

A card is only changed while no game is loaded. Before a card is changed, the whole card as it was
is copied to `card-undo/`, and **Put the card back as it was** brings it back (one step). A card
that does not read as sound is never written to; folder cards and PlayStation 1 cards are not
looked into. Keep a backup (Settings > Games and saves > Backups) before bringing saves in to a card you care about.
Memory cards can also be kept in sync with a share, both ways.

### A memory card for each game

Settings > Games and saves > Memory Cards > **Memory Card per Game** (on by default). Slot 1 holds a card of
the game's own: 8 MB, named by the game's serial (`memcards/SLUS-12345.ps2`), made the first time
the game starts. Games that read each other's saves, as PCSX2's game database lists them, share one
card, named by the smallest serial among them. Slot 2 stays as it is set. Switching the option off
puts every game back on the card of the Memory Cards page, with nothing to undo. A game whose own
options name a card for slot 1 keeps that one; a game the database does not know, a PlayStation 1
game and a program without a disc use the card of every game. Saves made before are still on the
card they were made on: put that card in slot 2 to copy them over in the PlayStation 2's browser.

### A state saved by itself, and undoing a load

Settings > Games and saves > Save states:

- **Auto Save State** (off, or every 2, 5, 10 or 15 minutes of play): the running game's
  state is saved to a place of its own beside the slots, each time over the one before; it is
  **Load the auto-save** in the quick menu. The game may stand still for a moment each time; the
  boot log says for how long (`auto-save:`). Never while a memory card is being written to.
- **Undo Load State** (on): before a state is loaded into a running game, the game as it
  is, is kept, and **Undo the last load** in the quick menu brings it back.
- **Save State for Rest Mode** (on): the game's state is saved when you open the PS5 menu and every five
  minutes, so you can go on after the console went to rest mode.

### Texture packs

A game's replacement textures go in `textures/<serial>/replacements`, as PCSX2 has it, and that
folder may be in the title folder, on a USB drive or on a network share:

    /data/homebrew/PPSA99249/textures/SLUS-12345/replacements/...     the title folder, looked in first
    <USB drive>/Crux2/textures/SLUS-12345/replacements/...            or PSPCSX2/, PS2/, or textures/... at the top
    <share>/Crux2/textures/SLUS-12345/replacements/...                the same four, on each share of network.cfg

The first of them that holds anything is the one used; Settings > Games and saves > Texture packs > **Load Textures**
switches packs on (for every game, or in one game's options), and **Precache Textures** reads
all of a pack before the game begins. A game's options say where its pack was found. Texture dumps
always go to the title folder.

**HD texture packs** (the library's Square menu; Settings > Games and saves > Texture packs > **HD Texture
Packs**; or a game's page > Texture packs, where it opens on that game) is the texture pack manager:

- **Installed**: every pack on the console, each USB drive and each share, by game (its cover and
  name from your library, or its serial), with its size, its number of files, where it is, and
  whether the game reads it (the first copy in the order above) or another copy comes first.
  Sizes are measured in the background and kept; **Look again** (Square) measures everything anew.
- **Move** a pack to another place (the console, a USB drive, a share): it is copied, every file
  checked against the original, and only then is the original deleted. A move that is stopped or
  fails leaves the pack where it was; moving it again goes on from what was copied. On the console
  and a USB drive the copy is made beside its place and put there when checked, so a game never
  reads a half copy.
- **Delete** a pack (asked twice). The game stops reading it at once; a delete that is stopped
  leaves the rest unread, and deleting it again finishes it.
- **In the collection**: the games of your library the Internet Archive's collection has a pack
  for, and for each game its packs and their sizes. **Download** one to the console or a USB drive
  (not to a share); several wait their turn and come one after another. **Download again** brings
  an installed pack anew.
- **Texture settings of the game** (Triangle): the game's own Graphics > Load textures.
- What runs (a download, a move, a delete) is under **Under way**, where Cross stops it, and on
  the bar along the bottom of the menus.

The console tells a title nothing of the free space on its storage or its drives, so the manager
cannot warn that a pack will not fit there; a pack too large stops when the place is full (a move
leaves the original as it was). Where the free space is known it says so before a download or a
move that may not fit. Moving or deleting the pack of the game that runs waits until it is closed.

The packs are their makers' work and often several GB; nothing of them comes with Crux2. A pack
comes in pieces over several connections at once (**Download connections**, in a game's options
beside the manager's row: 10, from 1 to 16; 1 is a single stream) and is unpacked as it comes, with no copy of the
archive kept; the bar shows how far it is, its speed, the connections and the time left.

### Game patches

PCSX2's desktop builds come with a collection of game patches (`patches.zip`: widescreen,
no-interlacing, 60 fps and others). It is not in this package, because the repository it comes
from states no licence. Settings > Games and saves > Patches > **Download Game Patches** takes `patches.zip`
from the latest release of [PCSX2/pcsx2_patches](https://github.com/PCSX2/pcsx2_patches/releases)
and puts it in the title folder's `resources/` (copying the file there by hand does the same). A
game's patches are then in its options, under Patches. The first download switches **Apply Widescreen
Patches** on (Settings > Picture > Graphics) once. The fixes PCSX2's game database applies by itself do not
need the collection. It worked on my console.

### What the database recommends for a game

A game's options have **Recommended settings** on their first page. PCSX2's game database applies
most of what it knows about a game by itself; three things it only recommends: a blending accuracy,
accurate alpha test and AA1. The button sets those for that game alone, switches the database's own
fixes back on where they were turned off, and says what it changed. It only ever raises the
blending accuracy; "Back to the Settings for everything" on the same page undoes it.

## Backups of memory cards and saved states

Settings > Games and saves > Backups > **Back Up Now** copies the memory cards and the saved states to a USB drive or to a network
share. Each backup is a folder of its own, named by its date and time, in `Crux2-backup` at the top
of the drive or share:

    Crux2-backup/2026-10-06_14-32-07/memcards/...    every memory card (a folder card: its tree)
    Crux2-backup/2026-10-06_14-32-07/sstates/...     every saved state
    Crux2-backup/2026-10-06_14-32-07/backup.txt      written last: what the backup holds

- **Back up now** makes one. "Saved states too" off makes it of the memory cards only.
- **Back up the memory cards when a game is closed** makes one of the cards by itself whenever a
  card was written to; these end in `_auto`, and only the newest 10 are kept.
- **Each backup replaces the one before it** (off) keeps at most two: the last made when a game was
  closed and the last you made yourself. Older ones go only after the new one is complete.
- The backups of the place are listed under it; Cross on one copies its files back to the console
  in place of the ones of the same name. What they replace is moved to `backup-before-restore` in
  Crux2's folder first. A backup whose `backup.txt` is missing was cut short and is not restored.
- Neither runs while a game is loaded.
- Backups go to `Crux2-backup` (older ones in `PSPCSX2-backup` are listed too).

A share has to let the account of `network.cfg` write to it (the games' share can be read-only).
On my console, backups to a share worked.

## The logs

Settings > System > **Log** shows what Crux2 wrote down, on the TV: the log of this run, of the run before,
and PCSX2's own log, each opening at its end (the directions move a line, L1 / R1 a page, L2 / R2
to the start and the end; errors and warnings stand out). **Copy the logs to...** puts the files in
`Crux2-logs/<date and time>/` on a USB drive or a share. When the run before ended in a crash,
Crux2 says so when the library opens, with the game that was loaded. A title closed from the
console's own menu is not a crash.

*New:* **Boot log** on the same page is "Only when something goes wrong" by default: the session's
log is kept in memory and written to `crux2-boot.log` after a crash, a game that did not start, 15
seconds without a new picture while a game runs, or when you choose **Save the log** (here or in
the quick menu). "Always written" writes every line as it comes, as before (from the next start).

## What does not work, or is not there

- **No 50 Hz output.** The console has no 50 or 100 Hz mode for a title. A PAL game is shown on the
  119.88 Hz output with two and three refreshes a frame in turn (or on 59.94 Hz with one and two).
- **No FSR 3 or FSR 4 upscaling or frame generation.** They need the motion of every pixel and the
  depth, which a PlayStation 2 game's picture does not come with; FSR 4 is a trained model that is
  not published. What needs only the finished picture is in: FSR 1, NIS, CAS, and three frame
  generation methods.
- The keyboard on the screen serves the search, names, the RetroAchievements sign-in, network
  share details and a cheat's lines; an address to fetch a cover from can't be typed in the
  interface.
- The memory card manager does not look into folder cards or PlayStation 1 cards, writes `.psu`
  files only, and keeps one step of undo for each card.
- A game's picture is not moved by a few pixels as the menus are (Screen care).
- RetroAchievements' hardcore mode and sounds.
- No video capture, no USB wheels or cameras passed to games, no network adapter emulation through
  a packet capture library.
- English only: the translations are part of the desktop interface and are not in this build.
- The free BIOS (OpenPS2BIOS, used only when you have none) is early work: many games reach their
  menus and go no further. Use a BIOS dumped from your own PlayStation 2.
- paraLLEl-GS is slower than PCSX2's renderer in the heaviest game I test with, and its picture is
  softer by design.

## If something goes wrong

Settings > System > Log shows the logs and copies them to a USB drive or a share ([The logs](#the-logs));
choose **Save the log** first if the run did not crash. By hand: copy `crux2-boot.log` (after the
next start it is `crux2-boot.previous.log`; the one before that is gone) and `logs/emulog.txt` from
`/data/homebrew/PPSA99249/`. Never send game files, BIOS files or `network.cfg` with them.

- **It closes at once, or the console shows an error:** the last lines of the boot log are the
  step it reached, down to the graphics driver's own start-up steps (`radv: ...`). A crash writes a
  report there. The log's second line, `build identity`, says which build that is.
- **A black screen after leaving the sandbox / "title folder was not found":** create an empty
  `usb-off.txt` next to `eboot.bin` and start again.
- **"files are missing from the title folder's resources":** the copy was cut short; copy the
  whole folder again.
- **No BIOS found:** the file has to be in `bios/` and be a real dump; Settings > Games and saves > BIOS lists what
  PCSX2 recognises.
- **USB drive not found:** the boot log has a line "USB drives: ..." with the reason, and one line
  per drive with the folders it found.
- **Share not found:** the boot log has a line per share and the server's answer. Check the address
  with another device first; `path` is `ip/share/folder` with forward slashes.
- **Settings file damaged:** it is renamed to `PCSX2.ini.broken` and Crux2 starts from defaults.
- **It stops responding and has to be closed:** create an empty `watchdog.txt` next to
  `eboot.bin`, reproduce it, and send the boot log: with that file the title reports where each
  thread was when it stopped answering.

## Native code modules (experimental)

A game's code converted ahead of time on a PC and run natively: [`docs/native-modules.md`](docs/native-modules.md).

## Building it, and working on it

[`docs/README.md`](docs/README.md) indexes everything: building
([`docs/dev/building.md`](docs/dev/building.md)), the layout of the port, the patches to PCSX2 and
Mesa, the tests, releasing, debugging and open work.

## Licences and credits

**Crux2 by Press5elect.** Crux2's own parts (the port layer, the interface, the tools, and the
changes to PCSX2 in `ps5/patches/pcsx2/ps5.patch`) are Copyright (C) 2026 Press5elect.

PCSX2 is GPL-3.0-or-later, and so is this port as a whole. It may be used, changed and passed
on under that licence, with its source: this repository, and the revisions that
`licenses/components.json` in the title folder names for every part.

Crux2's own parts carry one additional term, which section 7(b) of the licence allows: whoever
passes them on, or a work based on them, in source or binary form, whole or in part, keeps the
attribution "Crux2 by Press5elect" in the source files' copyright notices and in the work's legal
notices and About page. That holds however the work is made, including with an AI system or any
other automated tool. `NOTICE` (`NOTICE.txt` in the title folder) has the term in full. Running
Crux2, and changing it for your own use, need nothing: the licence does not let that be limited.

| Part | Author | Licence |
|---|---|---|
| PCSX2 | PCSX2 Dev Team and contributors | GPL-3.0-or-later |
| Crux2: the port layer, the host, the tools, the tests, the changes to PCSX2 | Press5elect | GPL-3.0-or-later, with the attribution term of `NOTICE` |
| The interface's screens (from PSFlyCast's Big Picture front end) | Press5elect (PSFlyCast and Crux2), and the other authors of the code PSFlyCast's front end came from | GPL-2.0-or-later, used under GPL-3.0-or-later |
| Features ported from OGBox (covers, game fixes fetch, boot log in memory, HDR10, the startup video's player and others) | Press5elect (OGBox) | GPL-3.0-or-later |
| PS5 Vulkan Template (the PS5 layer, the tools this build is made from) | Mihawk; Sascha Willems | MIT |
| PS5_Vulkan (link recipe, packaging tool, `libc.prx`) and PS5_Mesa (RADV for the console) | Mihawk; Mesa's authors | GPL-3.0; MIT |
| PS5 platform layer (payload SDK fork) | Mihawk; John Tornblom and ps5-payload-dev | GPL-3.0-or-later |
| USB drive access (`sandbox-elevator.elf` and its client) | BlackBearReloaded, ps5-native-app-boilerplate | GPL-3.0-or-later |
| paraLLEl-GS; Granite; volk | Arntzen Software; Hans-Kristian Arntzen; Arseny Kapoulkine | LGPL-3.0-or-later; MIT; MIT |
| PS2Recomp (the converter the recomp kit fetches and patches; its macros in `ps5/recomp/runtime/upstream/`) | ran-j and the PS2Recomp contributors | GPL-3.0 |
| def-jam-vendetta-recomp's work on PS2Recomp | jcsix694 | GPL-3.0 |
| What the recomp kit's Windows converter (`recomp/bin/windows-x64`) is built with: ELFIO, toml11, fmt, rabbitizer, nlohmann/json; libdwarf; mingw-w64's runtime and GCC's libstdc++ | their authors | MIT; LGPL-2.1; the mingw-w64 licences and the GCC Runtime Library Exception |
| MihawkGen (frame generation) | Mihawk | GPL-3.0-or-later |
| AMD FidelityFX (FSR 1, FSR 3 optical flow); NVIDIA Image Scaling | Advanced Micro Devices; NVIDIA | MIT |
| libvpx (the startup video's decoder) | The WebM Project authors | BSD-3-Clause |
| Move Management's loopback input protocol and reference client (the payload itself is not in the package) | REXCRAFT88 and the Move Management contributors | GPL-3.0 |
| Project Porpoise's aim fusion, for the PlayStation Move light gun | Ruben (Project Porpoise) | GPL-3.0-or-later |
| libsmb2 | Ronnie Sahlberg | LGPL-2.1-or-later |
| curl; OpenSSL | Daniel Stenberg and contributors; the OpenSSL Project | curl licence; Apache-2.0 |
| The libraries PCSX2 bundles and depends on (Dear ImGui, fmt, libchdr, LZMA SDK, libzip, rcheevos, SoundTouch, Zydis, FreeType, libpng, libjpeg-turbo, libwebp, zstd, LZ4, shaderc, glslang, SPIRV-Tools, PlutoSVG, rapidyaml and others) | their authors | their own; all compatible with GPL-3.0 (`licenses/`) |
| Fonts (Roboto, Twemoji, Font Awesome Free, PromptFont) | Google; Twitter and Mozilla; Fonticons; Shinmera | Apache-2.0; CC-BY-4.0; SIL OFL 1.1 |
| Roboto Bold (the interface's headings) | The Roboto Project Authors | SIL OFL 1.1 |
| Saira ExtraBold (the letters of the title's name) | The Saira Project Authors (Omnibus-Type) | SIL OFL 1.1 |
| The start-up sound (`resources/sounds/silicon-boot.wav`) | supplied by me for this title; it is mine to pass on with it | the title's own |
| The startup video (`resources/intro/`, made from `ps5/art/intro/src`) | Press5elect | GPL-3.0-or-later |
| The layout of memory cards and save files, for the card manager (`ps5/frontend/ps5_mc.h`) | written anew after the source of mymc+ (Florian Märkl and contributors, based on Ross Ridge's mymc); the unpacking of `.max` files after Haruhiko Okumura's LZARI | GPL-3.0-or-later; LZARI is free to use |
| Certificate bundle (`resources/cacert.pem`) | Mozilla, via certifi | MPL-2.0 |
| The title's mark and name (Crux2) and its pictures in the console's menu | Press5elect: drawn by `ps5/frontend/bigpicture/bp_draw.cpp` and `ps5/tools/make-title-art.py`; the line of text is set in Roboto | GPL-3.0-or-later; Apache-2.0 |

The complete list, with every revision, is `ps5/notices/components.json` (`licenses/` in the title
folder); [`ps5/ASSETS.md`](ps5/ASSETS.md) lists the non-code assets.

"PlayStation", "PS2", "PS5" and "DualSense" are trademarks of Sony Interactive Entertainment.
This project is not affiliated with or endorsed by Sony Interactive Entertainment or the PCSX2
project. Vulkan is a registered trademark of the Khronos Group; the RADV driver in this build
is not a conformant product.
