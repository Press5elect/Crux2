# Crux2

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
> one, so nothing from it goes upstream as it stands.

> **Tested on one console, mine.** Its development builds ran on my PS5 throughout: games from
> the internal storage, USB drives, an SMB share and the disc drive play; the network adapter, the
> hard drive, RetroAchievements, cheats, covers, backups and memory cards work. In the heaviest
> game I test with, at 6x with full blending accuracy and the rasterizer ordered view, a six-minute
> run held 100% speed for all but four short dips (59.2 frames a second on average). Not every
> setting has been tried on the console one by one. If it stops:
> [If something goes wrong](#if-something-goes-wrong).

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

To update, copy the new folder over the old one. Your settings, memory cards, save states and
`network.cfg` are files the build does not contain, so they stay as they are. To have them
out of the title folder altogether, so that the folder can be deleted and replaced:
[Outside the title folder](#outside-the-title-folder).

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
| `snaps/` | The last frame of a test run (screenshots are the console's own: the Create button) |
| `covers/` | Cover pictures |
| `pictures/` | Each game's title screen and a picture of it being played (`<serial>.title.png`, `<serial>.snap.png`), fetched from libretro's collection |
| `backgrounds/` | Yours, if you want them: pictures for behind the screens (PNG, JPEG or WebP; Settings > Interface > Background > My pictures) |
| `cheats/`, `patches/` | Your own cheat and patch files (`.pnach`); a cheat typed in on the console is in `cheats/<serial>_<CRC>_typed.pnach` |
| `saves/` | Save files to bring in to a memory card, and those written out from one (`.psu`, `.max`, `.cbs`, `.sps`, `.xps`, `.psv`): [The saves on a memory card](#the-saves-on-a-memory-card) |
| `card-undo/` | Each memory card as it was before the card manager last changed it |
| `gamesettings/` | Settings made for one game |
| `inputprofiles/` | Controller presets |
| `textures/` | Texture replacements (also read from a USB drive or a share: [Texture packs](#texture-packs)) |
| `cache/` | The game list, compiled shaders |
| `inis/PCSX2.ini` | All settings |
| `network.cfg` | The network shares to read games from (written on the first start) |
| `startup.wav`, `background.wav` / `.mp3` / `.ogg` / `.flac` | Yours, if you want them: a start-up sound in place of the title's own, and the sound that loops in the menus |
| `cheat-sources.txt` | More places to fetch cheats from (written on first use) |
| `inis/DEV9hdd.raw` | The PlayStation 2's hard drive, once made in Settings > Network |
| `crux2-boot.log` | Each start-up step of the last run, the emulator's warnings and errors, and a crash report if the title stopped |
| `crux2-boot.previous.log` | The same for the run before |
| `logs/emulog.txt` | The emulator's own log |
| `radv-shader-cache/` | The graphics driver's compiled shaders |
| `symbols.txt` | The names of the program's functions, for a profiled start (it comes with the build and belongs to its `eboot.bin`) |
| `resources/` | Shipped with the build: fonts, the game database, the interface's pictures and sounds, shaders |
| `LEGAL.txt`, `licenses/` | The notices: every part, its licence and where its source is |

### Outside the title folder

If there is a folder **`/data/crux2`** when Crux2
starts, and it can be written to, Crux2 keeps its files there and not in the title folder:
`inis/`, `memcards/`, `sstates/`, `bios/`, `covers/`, `pictures/`, `cheats/`, `patches/`,
`gamesettings/`, `inputprofiles/`, `textures/`, `snaps/`, `cache/`, `logs/`, `saves/` and
`card-undo/`. The title folder can then be deleted and replaced by a new version without
losing anything.

- Nothing is moved for you. Make the folder over FTP and copy into it what you want to keep
  (at least `inis/`, `memcards/`, `sstates/` and `bios/`); what is not there starts empty.
  Without a BIOS in `/data/crux2/bios/` no game starts.
- `games/` in `/data/crux2` is searched as well as `games/` in the title folder.
- `network.cfg`, `cheat-sources.txt` and `backgrounds/` are taken from `/data/crux2` when
  they are there, else from the title folder.
- The boot log, `startup.wav` and the background sound stay in the title folder.
- Settings > About > "Crux2's files" says which folder is in use, and the boot log has a line
  `data folder: ...`. Delete or rename `/data/crux2` and the title folder is used again.

## The interface

PCSX2's own Big Picture interface is not in this build. In its place is the front end written
for PSFlyCast, made over for a PlayStation 2 emulator: a library with a tab for each place games
come from (**Internal**, **USB**, **Network**), a game's details with its saved states, a loading
screen with the progress of a disc image being read, a quick menu over the paused game, and one
settings screen that serves every game and, opened from a game, that game alone. The name in
the top bar is drawn by the interface: C, R, U, the mark, 2.

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
- **View**, **A game at random** (it opens that game's details), and the scan for new games.

What is set shows as a chip at the top right; **Circle** shows all games again.

**In a game's details** (Triangle): **L1** makes it a favourite (a star on its cover, and a
shelf of its own) and **R1** hides it from the library after asking; Show > Hidden games brings
the hidden ones up, where R1 lists a game again. **Up and down** put the game's title screen or
a picture of it being played in the cover's place, where it has them. **More** has:

- **Start with...**: the game straight away, or the console's own start-up first.
- **Rename**: another title for the lists, typed on the keyboard (PCSX2's own custom title, so
  PCSX2 reads the game's file again for it); "Its own name again" undoes it.
- **Collections**: a collection is a shelf of your own, a name and the games you put on it.
  The list ticks the ones the game is in; "A new collection" makes one. Up to 24, kept with
  the settings; a game is known by its serial, so it stays in its collections when its file
  moves to another drive.
- **Fetch its pictures**: looks for this game's title screen and picture of play again.

Favourites and custom titles are PCSX2's own (its game list keeps them); hidden games,
collections and quick views are kept with the settings.

**The games' pictures** come from libretro's collection of thumbnails, by the game's name, as
the covers do: after each scan for every PlayStation 2 game that has none yet (Settings >
Interface > "Fetch game pictures automatically"), or at once with Settings > Library > "Fetch
the games' pictures". They show in a game's details, behind the title in the shelves view
(Settings > Library > "A picture of play behind the shelves"), and behind every screen with the
background "The game's picture". A game the collection has nothing for is remembered and not
asked about again until you ask.

**The keyboard on the screen** is the interface's own: the directions move over its keys, Cross
types one, Square takes the last letter away, Triangle is the space, L1 the capitals, R1 the
signs, Options accepts and Circle leaves. A password is shown as dots; L2 shows and hides it.

**A game's own options** (Details > Options, or Game options in the quick menu) show what that
game follows as "As in Settings"; changing an option gives the game a value of its own, marked
with a dot, and Square takes it away again. They are PCSX2's per-game settings files, in
`gamesettings/`, as its desktop builds write them. The same screen lists the game's patches and
cheats when `resources/patches.zip` or `cheats/` has any for it.

**The look.** Settings > Interface has six skins (**Crux**, the title's own warm black, orange
and cream, is the default; Midnight is the look the builds before had; Carbon is black, for an
OLED screen), an accent colour, the amount of motion, and the background: still, alive in one
of six ways, the focused game's cover colours or its picture, or **My pictures**. For those,
put PNG, JPEG or WebP files in a folder named `backgrounds` in the title folder: with several,
one follows the other (every 30 seconds to every hour) and fades in over it, and they are made
darker by as much as you choose so that what is written over them can be read. A picture is
never held larger than 1920 by 1080, and two are held at a time.

**The start-up.** There are four animations, each building the name out of its parts in time
with the start-up sound and delivering it to the top bar in its own way: **Strike** (the bars
slide in and the broad one strikes on the beat, the name glides to the bar), **Unfold** (the
bars grow in place, the letters fold into the mark, the mark flies to the bar and the letters
unfold from it), **Spin** (the bars turn like a propeller and lock, the parts hop to the bar
one after another) and **Slash** (two lines and an orange streak across the screen become the
mark, and a diagonal wipe opens the library). A start plays another one than the start before
it; Settings > Interface > "Which start-up" keeps one. Any button ends it early. The sound comes
with the build (`resources/sounds/silicon-boot.wav`); `startup.wav` in the title folder, if you
put one there, is played in its place. When the start-up is over, `background.wav`, `.mp3`,
`.ogg` or `.flac` from the title folder plays round and round for as long as no game is loaded;
it goes when a game starts and comes back after it. Settings > Interface > Sound of the
interface has a switch and a volume for each, and says which files were found.

**Statistics** (Settings > Statistics) counts what the library holds and what was played: the
games by kind, place, region and how well they are known to run, their size, the time played in
all, and the five games played the longest.

**Circle in the library** asks "Close Crux2? Are you sure?" and closes the title on Yes.

Not in this interface, because a controller cannot drive them or the console has no use for
them: the USB
devices of a PlayStation 2, video capture, input profiles and macro buttons, and the choice of
folders (they are fixed in the title folder). The settings behind them are still read from
`inis/PCSX2.ini` if you write them there.

## Screen care

An OLED television keeps a trace of what stood still on it for hours, and a menu stands still
whenever nobody touches the controller. Settings > **Screen care**:

- **In the menus, and over a paused game under the quick menu** (all on by default): the
  screens go dark when nothing was pressed for a while (1 to 10 minutes; 3 to begin with), and
  after as long again they are black with the mark wandering, until a button is pressed (that
  press does nothing else). Everything on a screen moves by a few pixels every other minute.
  The hint bar's words go faint after a quarter of a minute without a button. The skin Carbon
  is black.
- **While a game is played** (all off by default): the picture can be made darker when nothing
  was pressed for 5 to 30 minutes (a film a game plays by itself is darkened too, which is why
  it is off); the bars beside a picture narrower than the screen can be dark grey in place of
  black, so that they age with the rest of the screen; and the emulator's own figures (speed,
  frame rate) can go to another corner every five minutes. The game's picture itself is not
  moved.

None of this has been measured on a television: how fast a panel ages is not something the
title can know. The television's own screen saver and pixel shift work as well; leave them on.

## Controls

The bar at the bottom of each screen names what the buttons do there.

**In the library:**

| | DualSense |
|---|---|
| Move | D-pad or left stick |
| The tab before / after (Internal, USB, Network) | **L1** / **R1** |
| The previous / next letter (hold to keep going) | **L2** / **R2** |
| The next view (ten of them, shelves to discs) | click the **touch pad** |
| The next quick view; after the last, all games again | **R3** (press the right stick) |
| The game's details: saved states, its own options, More | **Triangle** |
| The list's menu: search, quick views, order, filters, collections, view, a game at random, scan, the PlayStation 2 menu | **Square** |
| In a game's details: favourite or not / hide or list again | **L1** / **R1** |
| In a game's details: its cover, its title screen, a picture of play | **up** / **down** |
| Settings | **Options** |
| Start the game | **Cross** |

**In the settings:** up and down choose, left and right change a value, **Cross** switches or
opens a list of choices, **Circle** goes back. In a game's own options **Square** takes the
game's value away again; on a controller button it removes the button.

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
Settings > Controllers, the quick menu's in Settings > Hotkeys. "Put the buttons back as they
were" on the controllers page brings this layout back.

The quick menu has resume, save and load state with the slot to use, **Load the auto-save** and
**Undo the last load** when there is one, **Achievements** and **Leaderboards** for a game that
has them, the disc change, the
game's options, its **Cheats** and its **Patches**, the settings, fast forward, restart, and **Quit game**, which asks
whether to save a state to go on from and returns to the library. Settings > About >
**Quit Crux2** ends the title.

**Players 2 to 4** are the controllers of the other signed-in users: the user who started
Crux2 is player 1, the others follow in the order they signed in (sign each one in with the
PS button of their controller). Settings > Controllers > Players puts them on the PlayStation
2's ports in one step, in the three ways games expect: **One or two players** (ports 1 and 2),
**Up to four players, multitap in port 1** (1A to 1D, what most four-player games want) and
**Up to four players, multitap in port 2** (1, then 2A to 2C). A running game sees a multitap
the next time it starts. (In `inis/PCSX2.ini` the controllers keep the names PCSX2's desktop
builds give them, `SDL-0` to `SDL-3`, so settings carry over.)

**Light bars in the players' colours** (Settings > Controllers, off by default): each controller's light bar says which player it is, in the
colours the PlayStation 4 used: blue, red, green, pink. Switched off again, a bar keeps its
colour until its controller is next connected.

**USB controllers (experimental;** Settings > Controllers, off by default**):** controllers the
console does not take itself, plugged into its USB ports: a **DualShock 3 or SIXAXIS**, with its
pressure-sensitive buttons (the d-pad, L1/R1/L2/R2 and the four face buttons, as the DualShock 2
had them), its rumble and its player light; **Xbox 360** pads; and **guitars, drums and other USB
pads** that describe themselves as a game pad or joystick. They are players 5 to 8 (`SDL-4` to
`SDL-7`). Each port's page under Controllers has **Map it to a USB controller** (the first one
for port 1, the second for port 2), and any of their buttons can be bound by hand. For the rhythm
games, set the port's controller type to **Guitar** and map it. Crux2 reads them through the
console's USB library, found while the title runs; that had not been tried on a console when this
was written. The boot log says how each step went, in lines beginning `usb pads:`, and when the
console refuses, only its own controllers are read as before.

**The PlayStation 2 menu:** Settings > BIOS > **Start the PlayStation 2 menu**, or the list's menu
(Square in the library), starts the BIOS with no game: the PS2's browser (its memory cards and
discs) and its System Configuration (clock, language, screen). **Quit game** in the quick menu goes
back to the library.

## Where the games are

Disc images as `.iso`, `.chd`, `.cso`, `.zso`, `.gz`, `.bin`, `.mdf`, `.nrg` and `.img`.

Each of the three places below has its tab in the library. The games are looked for when Crux2
starts and after each game; **Square** in the library (or Settings > Library > Scan for games)
looks again.

### The title's folder

`games/`, with any subfolders you like.

### USB drives

Games are read from a folder named `crux2`, `ps2`, `ps2iso` or `playstation2` at the top of
the drive (capital letters or not; folders named `pspcsx2` and `pcsx2` are read as before). Only those folders are scanned, not the whole drive.

A title cannot see USB drives from inside its sandbox. When Crux2 starts, it sends the bundled
`sandbox-elevator.elf` to the payload loader on port 9021, which lets this one title (by its
ID) out of its sandbox. If the loader is not running, Crux2 says so in the boot log and carries
on without USB drives; nothing else changes.

This is on by default. Settings > Interface > PlayStation 5 > **USB drives** turns it off (it
takes effect at the next start); so does an empty file named `usb-off.txt` next to `eboot.bin`,
should the interface not come up.

### Network shares (SMB)

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
- SMB 2 and 3 are spoken (not the old SMB 1). The share is only ever read.
- Restart Crux2 after changing the file.

A game on a share is read from it as it plays, so the link to the NAS matters while playing:
a wired connection is the safe choice. Settings > Speed > **Load the whole disc into memory** loads
the whole disc image into memory before the game starts instead (it worked from a share on the
console it was tried on); the start then takes as long as copying the image does, with a
progress bar on the loading screen, and the NAS is not needed again. A NAS that is off or asleep when Crux2 starts is given 8 seconds to
answer; without an answer its games are missing from the list for that run. A NAS that closes
a connection that was idle (many do, after some minutes) is connected to again the next time
anything is asked of the share. Memory cards, save
states and settings are never on the share: they stay on the console.

## A disc in a drive: the console's own, or a USB DVD drive

A PlayStation 2 game in a disc drive is shown on a fourth tab, **Disc**, with a turning disc:
gold for a DVD, blue for a CD. Play starts it from the drive. A disc that is not a PlayStation 2
game is named for what it is and not played.

- **The console's own drive** reads PlayStation 2 DVDs (silver underside). It does not read CDs.
- **A USB DVD drive** plugged into the console reads PlayStation 2 DVDs and also the games that
  came on CDs (blue underside). It works on a console without a drive of its own too. Plug it in
  before or while Crux2 runs: within two seconds its disc is on the Disc tab, and pulling it out
  takes it away. With a game in each drive, both are on the tab, and **Make an image of the disc**
  asks which one to copy. While a game plays from one drive, the other is still watched.
- **A locked tray.** Started without a jailbreak, the console locks a USB drive's tray when a
  disc is in it, and the drive's eject button does nothing until homebrew runs. Crux2 unlocks every
  drive's tray when it first sees it, and Settings > Library > **Eject a disc** unlocks a drive
  and opens it (not one a game plays from). The boot log
  says how each unlock and eject went (`disc: ... tray unlocked`, `disc: ... eject`).
- **Make an image of the disc** (Settings > Library, Square on the Disc tab, or "Make an image" in
  the disc's details) asks which kind: ISO, CHD smallest (LZMA), CHD quickest to load (Zstandard)
  or CSO; then where it goes: the internal storage, a USB drive (its games folder, or a folder
  `crux2` made at its top), or a network share, SMB or NFS (an ISO is written piece by piece; a
  compressed image is made on the internal storage, then copied). It is named
  "Title (SLUS-12345).chd" (or .iso, .cso) and is in the library after the next look for games.
- **The Disc tab** shows the disc in the drive with its cover, title screen and a picture of it
  being played (fetched for the disc), a few sentences of the game from Wikipedia, and its
  developer, publisher, year and genre from Wikidata. They are looked up the first time a disc is
  shown and kept in the data folder's `about/`; Settings > Library > "Look up what each game is"
  switches that off.

**USB DVD drives** (one ran on my console, as `/dev/cd1`). Each drive is a node of FreeBSD's
cd(4) driver: `/dev/cd0` is the console's own drive (that one ran here), and a USB drive is
expected beside it as `/dev/cd1`. A CD is told from a DVD by the drive itself (it answers no DVD
structure for a CD, and has a table of contents), and its sectors are read as 2048 bytes, the
way a DVD's are; PCSX2 is then given each data sector as the 2352 bytes of a CD sector built
around them. Audio tracks are silence, because the driver does not read audio (PlayStation 2
games on CD keep their music in their data). The boot log says what each drive did, in lines
beginning `disc:`.

**Experimental.** It ran on one console (mine): a game in the drive was found
and played. One console and one report is what stands behind it, so the switch says so:
Settings > Interface > PlayStation 5 > **Disc drives** (or `DiscDrive = false` under `[PS5]` in
`inis/PCSX2.ini`) leaves the drives alone, and nothing of this runs. The boot
log says what happened, in lines beginning `disc:`: which names `/dev` has, whether the drive
opens (and the error if not), the size it reports, and what the disc was taken for.

**Read into memory while you play.** Played straight from the drive, a game stuttered a lot on
my console. PCSX2 reads a disc a little
ahead of the game and makes the game wait whenever it asks for something that is not there
yet; a drive that has to move its lens, or start turning again, makes that wait long. So a
disc in the drive is now read 8 MB ahead of the game, and behind that all of it, from where
the game is onwards, into memory; what is in memory is never asked of the drive again, and
once the whole disc is there (a line on the screen says so) the drive is not used at all. It
takes as much memory as the disc is large and is done only when that much can be spared with
2.5 GB left over; otherwise as much as fits is kept. Settings > Interface > PlayStation 5 >
**Read the disc into memory while playing** switches it off (512 MB of what was read last is
kept then), and **Keep the disc turning** reads a little every 20 seconds so that the drive
does not stop. The first visit to each part of a disc still waits for the drive: on a model
of three made-up drives (`ps5/tests/disc/disc_cache_test.cpp`) the gain before the disc is in
memory is moderate, and nothing of it is measured on a console. The boot log measures it
there: every 30 seconds a line beginning `disc: 30 s:` says how much was read, how fast, the
slowest read, how often and how long the game waited for the drive, and how much of the disc
is in memory.

## The network adapter and the hard drive

Settings > Network switches on the PlayStation 2's network adapter (PCSX2's "Sockets" one, the
only kind a console can have): the game gets a network of its own and its traffic goes out
through the console's connection. The page shows the console's address, what the game is
doing with the adapter, and takes the two name servers. The **first name server
is PS2 Online's** (`45.7.228.197`, ps2online.com: one of the services that keep PlayStation 2
games online today, which a game reaches by asking that server for its addresses) unless you
chose one yourself. The row offers the services Crux2 knows by name (PS2 Online, and
PS Rewired: `67.222.156.250`, from psrewired.com/guides/pcsx2), the console's own name
server, and an address entered number by number for any other service. I have not played
online with it: that the address is PS2 Online's is what its site says, and whether a game
connects is for a console to show. The
hard drive is one file, `inis/DEV9hdd.raw`, made on that page as an empty drive of 40 or
120 GB; it takes room only as it is written to.

On my console the adapter and the hard drive worked. The lines beginning `net:` in the boot log say what the console
answered about its network, and count the adapter's name lookups and connections. A game's
pings are not expected to work (the console refuses the socket for them); that does not stop
TCP or UDP.

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

## Native code modules (experimental)

1. On your PC (Linux Mint or Ubuntu), unpack the newest recomp kit (`Crux2-recomp-kit-<version>.zip`) and install
   `git cmake ninja-build clang lld python3`.
2. `recomp/tools/recomp-game.sh GAME.chd` (or the game's `.iso`, `.cue`, `.cso`, a folder of disc
   images, or the executable itself: the file `BOOT2` names in its `SYSTEM.CNF`, read out of the disc
   by itself). Add `--for both` for Crux2 Recomp's folder too, and `--ghidra <Ghidra's folder>` to have
   Ghidra find the functions (better on retail games). The first run fetches PS2Recomp and builds its
   converter; compiling a large game takes long.
3. Copy `SLUS_123.45.crux2native` to `/data/crux2/native/` on the console, and start the game.

The kit makes Crux2 Recomp's folders on Windows too: with Python 3 and LLVM 18 or newer installed,
drop a game's `.chd` (or `.iso`, `.cue`, `.cso`, or a folder of them) on `Crux2 Recomp.cmd`, and
copy the folder it makes in `output` (the executable, its module and the disc as an `.iso`) into
`/data/crux2recomp/`. The kit brings PS2Recomp's converter built for Windows. Crux2's native modules
are made on Linux.

Or, with the games on a network share: start the game once in Crux2, and it puts its executable
in `Crux2-native/executables` on that share. Then, with the share mounted on the PC,
`recomp/tools/native-batch.sh /mnt/ps2/Crux2-native` (add `--watch` to keep it running) makes every
waiting game's module into `Crux2-native/modules`. The game's next start copies its module in by itself.

The log says `native: SLUS_123.45.crux2native loaded` when the game starts, and when it closes how
many runs and instructions went natively, how many times events or the emulator's instructions
took over, and how many functions were found changed. What runs natively: the main processor's
integer, multimedia and floating-point code between the instructions only the emulator can run
(system calls, COP0, the vector unit's COP2 and its loads and stores, cache operations). Cycles are
counted as 9 eighths of a cycle per instruction, PCSX2's figure for most instructions, so timing
can differ from PCSX2's own in places. The MMI, FPU and SA instructions run PCSX2's own interpreter code
(`crux2_ee_interp.h` in the kit): PS2Recomp's translations of about 60 of them gave other results
than the processor. A module made with an older kit is refused (module ABI 3); make it again. A module
is the game's code: keep it to yourself.


## Settings worth knowing

- **Graphics:** the Vulkan renderer at 6x the PlayStation 2's resolution by default, with Upscaling and sharpening at FSR 1 from 4x: while FSR is chosen, the game is drawn
  at 4x and FSR enlarges it to the output, so the 6x takes effect when FSR is switched off.
  Automatic quality lowers the resolution by itself where the graphics chip falls short.
  Settings > Graphics > Internal resolution changes it. The software renderer is there too.
- **Memory cards are held in memory.** A game reads and writes a copy of the card, and what
  changed is written to the card's file half a second after the game stops writing, every two
  seconds while it keeps writing, when the game is paused, and when it is closed. (PCSX2 serves
  a card straight from its file, page by page, which on the console slowed a save to about 5
  frames a second.) "Memory Card ... was saved to storage" appears over the game once the file has the data.
  Closing the title from the console's menu before that message can lose up to the last two
  seconds of a save: wait for it, or pause first.
- **Speed hacks are as PCSX2 ships them.** Nothing that trades accuracy for speed is switched
  on by this port. If a game falls short of full speed, the first thing to try is
  Settings > Speed > **Multi-threaded VU1 (MTVU)**, which is a speed hack: safe for most games,
  wrong for a few.
- **When a game is slow, the overlay says which part is the limit.** Settings > On-screen
  display: turn on Speed, Frames a second, Processor usage and Graphics chip usage. Then:
  - *Speed at 100% but the game's own frame rate low:* the emulator keeps up; the game itself
    is slow, as it would be on a PlayStation 2. A 60 fps patch does this when it asks more of
    the emulated processor than it has; the patch's description usually says so. Settings >
    Speed > EE cycle rate above 100% gives the game a faster emulated processor, at the
    cost of more work for the console's.
  - *Speed below 100% and the EE thread near 100%:* the emulated processor is the limit. Try
    Multi-threaded VU1; undo any EE cycle rate above 100%.
  - *Speed below 100% and the GS thread near 100%:* the graphics thread is the limit: the game
    asks for more draws a second than one of the console's processor cores gets through.
    Settings > System > **Graphics driver on its own thread** (advanced) takes the driver's share of
    that work off the thread (in the scene measured on a console, 40 frames a second became 48,
    a sixth more draws a second); it is on by default. The
    EE, GS and VU figures count a thread's short look for more work as busy. With the higher blending accuracy
    levels, Settings > Rendering > **Rasterizer ordered view** matters too: the console's
    driver has no framebuffer fetch, and without it accurate blending draws in many small
    steps with a wait for the GPU before each. After that, what is left is what the game asks
    for: a 60 fps patch doubles it.
  - *Speed below 100% and the GPU near 100%:* a lower internal resolution.

  The EE, GS and VU percentages are each thread's own processor time.

  When it is the graphics thread, the title can say what that thread is busy with: Settings >
  Interface > PlayStation 5 > **Profile the next start**, then quit and start Crux2 again, play
  the slow part for half a minute or more, and quit Crux2 from its own menu (Settings > About).
  `crux2-boot.log` then has, for every ten seconds, the functions the graphics thread spent
  its time in, how much of it was the emulator's code and how much the graphics driver's, and
  the instructions it was interrupted at most (`profile: ...` lines), and the frame rate and
  the draws of a frame (`test: ...` lines). That is the file to send with a report of a slow
  game.
- **Shaders compile as a game first needs them**, so a game can stutter the first time an
  effect appears. The compiled shaders are kept (`cache/`, `radv-shader-cache/`) and the
  next run has them.
- **50 Hz (PAL) games** run on a 59.94 Hz output, the only rate the title asks the console
  for, so their frames alternate between one and two refreshes. NTSC versions are smooth.
- **Covers:** a picture named like the game's serial or title in `covers/` (`.jpg`, `.png` or
  `.webp`) is used as its cover. Missing covers are fetched after every scan from libretro's
  collection of box arts on GitHub (`libretro-thumbnails/Sony_-_PlayStation_2`): the game is
  looked up by its name in a list the build carries, its region's picture first, and saved as
  `covers/<serial>.png`. Settings > Interface > PlayStation 5 has the switch (**Download covers
  automatically**) and **Download the missing covers now**, which also asks again for the games
  nothing was found for before (`covers/no-cover.txt` remembers them). Measured on the PC
  against the emulator's whole game database: a picture is found for 75% of all games, 86% of
  the NTSC-U ones. A `CoverURL` of your own under `[PS5]` in `inis/PCSX2.ini`, with
  **DownloadCovers** set, is used instead. A game without a cover is shown as a card with its title.
- **Cheats:** a game's Cheats page (Details > Options > Cheats) has **Download cheats for this
  game**, and Settings > Interface has **Download cheats for all games**. They fetch the game's
  `.pnach` files from four collections on GitHub (the build carries a list of where their files
  are, not the cheats): `xs1l3n7x/pcsx2_cheats_collection`,
  `complicatiion/pcsx2_europe_cheats_collection`, `heavyjam-code/pcsx2-cheats-n-patches` and
  `TheAdmiester/PCSX2-Cheats`. Cheats go to `cheats/`; a collection's patches (heavyjam-code's
  60 fps, no-interlacing and no-blur patches) go to `patches/`, and show on the game's Patches
  page beside PCSX2's own. Each is saved as `<serial>_<CRC>_github-....pnach`; your own files are
  never changed. A file whose name lost its CRC in its collection is found by the game's serial.
  On the way in, a cheat's name broken over several lines is joined into one, so its codes are
  not added to the cheat before it, and a byte order mark is taken off. More places can be named
  in `cheat-sources.txt` in the title folder (written on first use, with how). Cheats and patches
  stay off until you switch them on in the game's options.
- **A cheat typed in**: the same page has **Type a cheat in**. The keyboard on
  the screen asks for the cheat's name, then for its lines one at a time, each as the lists
  print them (`2034A5B0 00000001`: two groups of eight digits and letters A to F; the space
  may be left out); a line with nothing typed ends it. It has to be a raw code: one scrambled
  for a cheat device does nothing. The cheat is kept in `cheats/<serial>_<CRC>_typed.pnach`
  and appears in the list to be switched on; **Take a typed cheat out** removes one.
- **Save states and the console's write limit:** a title writes about 1.3 GiB at full speed
  in one run and is slowed to about 2 MiB/s after that (the platform layer's measurement).
  Memory cards and settings are far below that; a long session with dozens of save states
  can reach it, and a save state then takes several seconds.
- **Resetting:** delete `inis/PCSX2.ini` to start again from the defaults.

### RetroAchievements

Settings > **Achievements** (on my console it signed in and found a game's achievements).
**Sign in** asks for the
name and the password of your account at retroachievements.org on the keyboard on the screen
and hands them to PCSX2's own sign-in; what is kept is the key RetroAchievements answers with
(`inis/secrets.ini`), never the password. Signing in switches **Achievements** on. From then on
a game you start is looked up there; an achievement earned is announced in a corner of the
picture, the challenges under way and a leaderboard's running time are shown small in another,
and the quick menu has **Achievements** and **Leaderboards**: PCSX2's own lists, over the paused
game (the directions move, Cross opens, Circle goes back). Hardcore mode is not offered: saved
states, cheats and slow motion stay usable.

The switches for Achievements, Encore mode, Spectator mode and Unofficial
achievements can only be changed with no game loaded: changing them under a running game from
a Big Picture interface is reported to freeze PCSX2 (its issue 14899). The sounds PCSX2 plays for an
achievement on a PC are not played: the console gives a title one sound output, and the game
has it.

### More of PCSX2's settings

Settings PCSX2 has that Crux2 shows too, for all games; not tried on the console one by one.

- **Emulation:** "Fast disc access" (PCSX2's fast CDVD) is in the settings of all games,
  not only in a game's own. The seconds of a clock set by hand. Under advanced settings: a
  processor core for each emulator thread (thread pinning).
- **Graphics:** under advanced settings, internal resolutions above 12x, and three switches
  for a graphics driver that misbehaves (no framebuffer fetch, no mailbox presentation, blit
  to the display). A game's Renderer fixes have **Target page rearranging**, which PCSX2
  added in v2.9.103.
- **Picture:** FSR 1 (below).
- **On-screen display:** the graphics chip's statistics, and the state of an input recording.
- **Audio:** **Output latency** (how much sound the console is handed at a time: 5 ms
  up to 43) and **Surround** (PCSX2's expansion of a game's stereo to 5.1 or 7.1,
  sent to a port of eight channels; which of the console's channels each ends up in has not
  been heard, and if the console refuses the port the sound stays stereo).
- **RetroAchievements:** how long a leaderboard's notice stays, and where the notices and
  the small pictures are shown.
- **Interface:** "Circle chooses, Cross goes back" exchanges the two buttons in every menu;
  "Refresh rate of the picture" (120 Hz, below).
- **Backup:** "Each backup replaces the one before it".

Also in the interface: **Cheats** and **Patches** are in the quick menu, straight to that
page of the game's options. A label, a description, a value or a dialog's title that does not
fit slides sideways by itself while the focus is on it, so that all of it can be read; a long
notice at the bottom of the screen goes over several lines. The two lines of Screen care that
were no settings are gone.

### 120 Hz

The picture is 60 Hz by default. Settings > Motion > **Refresh rate** > 120 Hz (from the next
start) has the title tell the console that it can show 119.88 pictures a second (a flag in
`sce_sys/param.json`), and take the display's fastest mode; it ran at 119.88 Hz on my console.
Where the television does not take 120 Hz at 4K, the graphics driver falls back to 59.94 by
itself. The graphics driver reads the flag from the title's `param.json` under `/app0`, which a
title that has left its sandbox for USB drives does not see, so Crux2 points it
(`PS5_VIDEOOUT_PARAM_JSON`) at the same file in the title folder. The boot log says what came of
it: `display: 120 Hz is asked for`, then either `display: 3840x2160 at 119.88 Hz` or the driver's
own line about why not (`wsi/videoout: 119.88 Hz not supported` or `refused`: the console said
no; `accepted, but a vblank comes every 16.7 ms`: the television stayed at 60).

What it is for: the menus move at 120; a 60 Hz game's frames each stay for two refreshes; a
PAL game's 50 frames are spread over two and three refreshes, not one and two. "Sync to the
display's refresh rate" (Settings > Emulation) keeps a 59.94 Hz game in step with the
119.88 Hz screen as well. Without the menus: `OutputRefresh = 120` (or `60`) under `[PS5]` in
`inis/PCSX2.ini`.

### FSR 1

Settings > Picture > **Upscaling and sharpening** > **FSR 1 from 4x**, **from 3x** or **from
2x**. AMD's FidelityFX Super Resolution 1.0 works on a picture that is drawn smaller than it
is shown: its edge-following filter enlarges the picture to the size it has on the screen,
then its sharpening pass runs at that size ("Sharpness" sets how strong). It is for games
that do not hold full speed at 6x: close to a 4K picture for much less work, from 4x for the
finest picture, from 2x for the most speed.

**Choosing it is one step.** Each choice sets the resolution the game is drawn
at, whatever "Internal resolution" says and also for a game with a resolution of its own;
the resolution rows under Graphics are greyed out meanwhile. The boot log says once what it does:
`picture: FSR 1 enlarges 1920x1344 to 3086x2160`.

The filter is AMD's, unchanged (`resources/shaders/common/fsr1/`, MIT); PCSX2's CAS, which
was there before, is the same row's other choices. The console's driver compiles both of
its passes (`VK: FSR 1 is available.` in `logs/emulog.txt`). On the PC the two passes were run on a
software Vulkan driver against a picture known at every size
([What was checked](#what-was-checked)): they came closer to the true picture than a bilinear
enlargement by 1.2 to 1.4 dB. What it costs on the console is for a console to show.

### Frame generation

Settings > Picture > **Frame generation** (off by default)
makes pictures between the game's own:

- **Fill in the pictures a game repeats (30 to 60).** Most PlayStation 2 games draw 30 (or
  25, or 20) pictures a second and hand each one over twice or three times. The repeats are
  replaced by pictures made between the last two new ones, so the screen gets a different
  picture at every refresh. A game that already draws 60 is left as it is. This works on a
  60 Hz screen.
- **That, and double 60 to 120 on a 120 Hz screen.** Where the output runs at 119.88 Hz (see
  [120 Hz](#120-hz), itself not seen working yet), a made picture is also shown between every
  two pictures of a 60 a second game. This path shows two pictures for each of the game's
  vsyncs, which nothing in PCSX2 did before; it is the less tried of the two.

**What it is, and what it is not.** A PlayStation 2 game hands over finished pictures and
nothing else, so the picture between two is made from those two alone, the way a
television's motion smoothing or a program like Lossless Scaling does it: the pictures are
searched for where each part went (in small copies first, where long moves are short, then
in larger ones), and each pixel of the new picture is fetched from where it was and where it
will be. It is this port's own (`resources/shaders/vulkan/framegen.glsl`, eight passes, GPL),
not Lossless Scaling's code, and **not FSR 3 or FSR 4**: AMD's frame generation needs the
game's motion vectors and depth, which no PlayStation 2 game has, and FSR 4's is a trained
model that is not published.

**What to expect.** A turn of the camera, which is most of what moves in a game, comes out
well. Where it cannot follow, the two pictures are mixed in place, which shows as a faint
double image: around something that moves fast in front of something else (a halo where
the background was uncovered), on a ball that spins, and in whatever is too fast or too
small for the search. After a cut to another scene nothing is made up; the older picture
stays one refresh longer. A repeating pattern (a tiled floor, a fence) can still be matched
one repeat off in places, which shows as a patch that wobbles. And it costs time: to make a
picture between two, the second has to be there, so with a 30 a second game everything is
on the screen one refresh (a sixtieth of a second) later than without. Fast forward, slow
motion and a paused game are left alone.

The boot log says what it does: `picture: frame generation's eight passes were made by the
graphics driver` when it is first switched on, `picture: frame generation: the game shows a
new picture every 2 vsync(s); 1 made between each two` when it starts, and, when the game
closes, how many pictures it made. What it costs the graphics chip is not known; the
performance overlay's GPU figures include it. How it was checked is in
[What was checked](#what-was-checked).

### The saves on a memory card

Settings > Memory cards > **Look into a card**
lists the saves on a card, each with the name it gives itself, its size and its date, and
how much room is left. From there:

- **Bring a save in from a file.** Save files are looked for in a folder named `saves` on
  every USB drive and every share, and in `saves/` of Crux2's own folder (which FTP reaches).
  Read: EMS (`.psu`), Max Drive (`.max`), CodeBreaker (`.cbs`), SharkPort and X-Port (`.sps`,
  `.xps`) and the PlayStation 3's (`.psv`, of a PlayStation 2 save).
- **Copy a save out** as a `.psu` into one of those `saves` folders, **copy it to another
  card**, or **delete** it.
- The card itself: **rename**, **make a copy**, **delete**.

A card that was never formatted (as a new one is made) is made ready by the first save put
on it. A card is only changed while no game is loaded: a running PlayStation 2 holds the
card's layout in its own memory and would write over the change. Before a card is changed,
the whole card as it was is copied to `card-undo/`, and **Put the card back as it was**
brings it back (one step, the last change). A card that does not read as sound is never
written to; a card that is a folder, and a PlayStation 1 card, are not looked into.

How it was checked is in [What was checked](#what-was-checked): against mymc+, another
program for the same cards, on a PC. What a save does once a game reads it is for a console
to show; keep a backup (Settings > Backup) before bringing saves in to a card you care about.

### A memory card for each game

Settings > Memory cards > **A memory card for each game** (on by default). With it on, slot 1 holds a card of the game's own: 8 MB, named by the
game's serial (`memcards/SLUS-12345.ps2`), made the first time the game starts. Games that read
each other's saves, as PCSX2's game database lists them (a sequel that imports the first
part's save, the discs of one game, its releases in other regions), share one card, named by
the smallest serial among them. Slot 2 stays as it is set. The card is named in the game's
settings as the emulator holds them in memory and never written to the game's settings file,
so switching the option off puts every game back on the card of the Memory cards page, with
nothing to undo. A game whose own options name a card for slot 1 keeps that one; a game the
database does not know, a PlayStation 1 game and a program without a disc use the card of
every game. Saves made before are still on the card they were made on: put that card in slot 2
to copy them over in the PlayStation 2's browser. A game's own Memory cards page says which
card it gets.

### A state saved by itself, and undoing a load

Settings > Interface > Behaviour:

- **Save a state by itself** (off, or every 2, 5, 10 or 15 minutes of play): the running game's
  state is saved to a place of its own beside the slots, each time over the one before. It is
  listed with the game's saved states ("Saved by itself while playing") and is **Load the
  auto-save** in the quick menu. Only time the game ran is counted. The game may stand still
  for a moment each time; the boot log says for how long (`auto-save:`). Not in hardcore mode,
  and never while a memory card is being written to.
- **Undo for a loaded state** (on): before a state is loaded into a running game, the game as
  it is, is kept, and **Undo the last load** in the quick menu brings it back: a wrong slot is
  quickly chosen with a controller. It is offered until the game is closed, and is not part
  of a backup.

### Texture packs

A game's replacement textures go in `textures/<serial>/replacements`, as PCSX2 has it, and
that folder may be in the title folder, on a USB drive or on a network share:

    /data/homebrew/PPSA99249/textures/SLUS-12345/replacements/...     the title folder, looked in first
    <USB drive>/Crux2/textures/SLUS-12345/replacements/...            or PSPCSX2/, PS2/, or textures/... at the top
    <share>/Crux2/textures/SLUS-12345/replacements/...                the same four, on each share of network.cfg

The first of them that holds anything is the one used; Settings > Rendering > **Load
texture packs** switches packs on (for every game, or in one game's options). A game's
options say where its pack was found (the title folder and the drives; a share is asked only
when the game starts). Texture dumps always go to the title folder. Reading a pack from a
share is as fast as the network, and "Load them all at the start" reads all of it before the
game begins.

A game's options > Rendering > **Download an HD texture pack** fetches a pack from the
Internet Archive's collection into the title folder's `textures/` or `<USB drive>/Crux2/textures/`. The packs are
their makers' work and often several GB; nothing of them comes with Crux2.

### Game patches

PCSX2's desktop builds come with a collection of game patches (`patches.zip`: widescreen,
no-interlacing, 60 fps and others). It is not in this package, because the repository it
comes from states no licence. The console fetches it for you: Settings > Interface >
PlayStation 5 > **Download PCSX2's patches** takes `patches.zip` from the latest release of
[PCSX2/pcsx2_patches](https://github.com/PCSX2/pcsx2_patches/releases) and puts it in the title
folder's `resources/` (copying the file there by hand does the same). A game's patches are
then in its options, under Patches. The first download switches **Widescreen patches** on
(Settings > Picture), so every game that has a 16:9 patch starts with it; it does that once,
and a switch turned off afterwards stays off through later downloads. The row says how many games the collection knows and
when it was fetched; pressing it again fetches the newest one. The fixes PCSX2's game database
applies by itself do not need the collection. It worked on my console.

### What the database recommends for a game

A game's options (Triangle on a game, then its options) have **Recommended settings** on their
first page. PCSX2's game database applies most of what it knows about a game by itself; three
things it only recommends, with a note on the screen when the game starts: a blending accuracy
(for 709 of its games), accurate alpha test (10) and AA1 (2). The button sets those for that
game alone, switches the database's own fixes back on where they were turned off, and says
what it changed and what PCSX2's compatibility list says of the game. It only ever raises the
blending accuracy; the row shows beforehand how many changes it would make. Higher blending
accuracy costs speed: "Back to the Settings for everything" on the same page undoes it.

## Backups of memory cards and saved states

Settings > **Backup** copies the memory cards and the saved states to a USB drive or to a
network share. Each backup is a folder of its own, named by its date and time, in
`Crux2-backup` at the top of the drive or share:

    Crux2-backup/2026-10-06_14-32-07/memcards/...    every memory card (a folder card: its tree)
    Crux2-backup/2026-10-06_14-32-07/sstates/...     every saved state
    Crux2-backup/2026-10-06_14-32-07/backup.txt      written last: what the backup holds

- **Back up now** makes one. "Saved states too" off makes it of the memory cards only.
- **Back up the memory cards when a game is closed** makes one of the cards by itself whenever
  a card was written to; these end in `_auto`, and only the newest 10 are kept. A backup you
  made yourself is never taken away, unless the next switch is on.
- **Each backup replaces the one before it** (off by default) keeps the place
  from filling up: a backup that was made whole takes away the older ones of its kind, so the
  place holds two at most, the last made when a game was closed and the last you made
  yourself. The older ones go only after the new one is complete; one that was cut short
  takes nothing away.
- The backups of the place are listed under it; Cross on one asks, and then copies its files
  back to the console in place of the ones of the same name. What they replace is moved to
  `backup-before-restore` in Crux2's folder first (until the next restore). Cards and states
  the backup does not have are left alone. A backup whose `backup.txt` is missing was cut
  short and is not restored.
- Neither runs while a game is loaded: a memory card may be written to at any moment then.
- Backups go to `Crux2-backup` (older ones in `PSPCSX2-backup` are listed too).

A share has to let the account of `network.cfg` write to it (the games' share can be read-only
and still serve games; the message says so when the server refuses). Tested on a PC against a
Samba server: as a guest and with an account, into a folder inside a share, over a connection
the server closes in between, and against a share nobody may write to. On my console, backups to a share
worked.

## The log on the screen

Settings > **Log** shows what Crux2 wrote down, on the TV: the log of this run, of the run
before, and PCSX2's own log, each opening at its end (the directions move a line, L1 / R1 a
page, L2 / R2 to the start and the end; errors and warnings stand out). **Copy the logs to...**
puts the three files in `Crux2-logs/<date and time>/` on a USB drive or a share. When the run
before ended in a crash (a crash report, or Crux2 stopping itself with a reason, is the last
thing in its log), Crux2 says so when the library opens, with the game that was loaded. A
title closed from the console's own menu is not a crash and is not reported as one.

## What does not work, or is not there

- The keyboard on the screen serves the search, names (a game's, a collection's, a quick
  view's, a memory card's), the RetroAchievements sign-in and a cheat's lines; an address to
  fetch a cover from can't be typed in the interface.
- **No 50 Hz output.** The console has no 50 or 100 Hz mode for a title. A PAL game is shown
  on the 119.88 Hz output with two and three refreshes a frame in turn (or on 59.94 Hz with
  one and two).
- **No HDR.** PCSX2 renders and presents in standard range; there is nothing for an HDR output
  to carry.
- **VRR** is the console's own setting (Screen and Video > Video Output). Nothing in the title
  switches it on or off, and I have not looked at whether it takes effect for this title.
- **No FSR 3 or FSR 4, neither their upscaling nor their frame generation.** They work from
  the motion of every pixel between two frames and from their depth, which a PlayStation 2
  game's picture does not come with; FSR 4 is a trained model that is not published, and its
  PS5 build (BlackBearReloaded's ps5-fsr4) runs on another Vulkan driver than this title's.
  What needs only the finished picture is in: FSR 1, and a frame generation of this port's
  own (Settings > Picture).
- **The game's network address, mask and gateway cannot be set.** PCSX2's Sockets adapter,
  the only kind a console can have, gives the game its address itself and always answers its
  DHCP; PCSX2's own settings grey those rows out for it. The two name servers can be set.
- **Pressure-sensitive buttons:** PCSX2's "pressure modifier" is on the Controllers page (a
  button held to press the others lightly). Pressure from how far a trigger is pulled is not
  built.
- **Texture packs are not downloaded.** They are read from `textures/`, a USB drive or a
  share ([Texture packs](#texture-packs)); there is no collection of them the title could
  fetch from by a game's serial as it does covers and cheats.
- The memory card manager does not look into folder cards or PlayStation 1 cards, writes
  `.psu` files only, and keeps one step of undo for each card.
- A game's picture is not moved by a few pixels as the menus are (Screen care): that would
  blur it or cut its edge.
- RetroAchievements' sounds are not played (the notices are shown).
- No video capture, no microphone, no USB devices passed to games (wheels, guns, cameras), no
  network adapter emulation through a packet capture library.
- English only: the translations are part of the desktop interface and are not in this build.
- The performance overlay's EE, GS and VU percentages are what each thread reports of itself
  (whether a title may read another thread's processor time is not yet measured on the
  console); the software renderer's worker threads read 0.

## If something goes wrong

Settings > Log shows the logs on the screen and copies them to a USB drive or a share
([The log on the screen](#the-log-on-the-screen)). By hand: copy `crux2-boot.log` (after
the next start it is `crux2-boot.previous.log`; the one before that is gone) and
`logs/emulog.txt` from `/data/homebrew/PPSA99249/`. Never send game files, BIOS files or
`network.cfg` with them.

- **It closes at once, or the console shows an error:** the last lines of the boot log are the
  step it reached, down to the graphics driver's own start-up steps (`radv: ...`). A crash
  writes a report there: the signal, the registers and the return addresses, as offsets into
  the program that can be looked up against this build's ELF. The log's second line, `build
  identity`, says which build that is.
- **A black screen after leaving the sandbox / "title folder was not found":** create an empty
  `usb-off.txt` next to `eboot.bin` and start again.
- **"files are missing from the title folder's resources":** the copy was cut short; copy the
  whole folder again.
- **No BIOS found:** the file has to be in `bios/` and be a real dump; Settings > BIOS lists
  what PCSX2 recognises.
- **USB drive not found:** the boot log has a line "USB drives: sandbox elevation ..." with the
  reason, and one line per drive with the folders it found.
- **Share not found:** the boot log has a line per share and the server's answer. Check the
  address with another device first; `path` is `ip/share/folder` with forward slashes.
- **Settings file damaged:** it is renamed to `PCSX2.ini.broken` and Crux2 starts from defaults.
- **It stops responding and has to be closed:** create an empty `watchdog.txt` next to
  `eboot.bin`, reproduce it, and send the boot log: with that file the title reports where each
  thread was when it stopped answering.

## What was checked

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
its nominal tempo 95-100% of the time. The test windows of the boot log (`Test runs`, below) are
where such figures come from. Not tried on the console one by one: every setting, paraLLEl-GS in
every game, the native code modules, and many of the PS2 accessories.

## Test runs

A console run needs nobody at the controller. A launch that finds `test-run.txt` in the title
folder deletes it, does what it says, writes what it measured as `test:` lines to the boot log
and klog, and ends through the same shutdown as Quit Crux2:

```
seconds 60
game games/my-test-disc.iso
screenshot
```

Every 5 seconds it reports the frame rate, the emulation speed, the game's own frame rate, the
longest gap between two frames, the number of gaps over 40 ms (each one a visible stutter) and
how many audio writes ran short; at the end, the same figures over everything after the first
window, a screenshot of the last frame (`snaps/test-run.png`), and `test: end`.
`ps5/tools/run.sh [game]` does all of it with PS5_Vulkan's console tools (ps5vkctl, klog, FTP):
it writes the file, launches the title, captures klog until `test: end`, a crash or the
title's exit, symbolises a crash, and fetches the boot log and the screenshot.

Two more lines, for finding out where the time goes:

```
profile
threaded-recording
```

- `profile` samples the graphics thread every 2 ms (a signal, whose handler notes the
  interrupted instruction and return addresses from the stack) and every ten seconds writes
  `profile: ...` lines: how much of the thread's time was the emulator's code, the graphics
  driver's, the platform layer's, other libraries' and outside the program, then the functions
  it ran in and those on its stack, by name. The names come from `symbols.txt` in the title
  folder, which the link writes from the same program; it also says which functions begin by
  setting up a frame (a capital letter), and a stack is read by that: by the frames where the
  interrupted function has one, by the first address found on the stack where it has none (the
  compiler leaves the frame out of functions that call nothing), and by every address found
  where the thread was outside the program. `profile all` samples the emulation and VU threads
  as well. Windows in which a thread was under 15% busy are not written, and a
  thread is written up at most 60 times.
- `threaded-recording` sets `RADV_THREADED_RECORDING=1` for that launch, whatever the setting
  under PlayStation 5 says: the graphics driver records the renderer's commands on a thread
  of its own, which `profile` then samples too.
- After the lists, for the functions a thread ran in most: the instructions it was interrupted
  at most, as offsets into the function, to be read against the build's disassembly
  (`build/ps5/link/llvm-pie.elf`, the build named by the log's `build identity` line).
- Each window of a running game also gets a second `test:` line: the EE, GS, VU and GPU
  percentages of the overlay, and the renderer's counts for a frame (draws, draw calls,
  barriers, render passes, readbacks, texture copies and uploads).

A file with `profile` or `threaded-recording` and neither `seconds` nor `game` is a run played
by hand: the windows are measured while a game runs (not in the menus, not while paused), and
the run ends when the title is closed from its interface. Settings > Interface > PlayStation 5
> **Profile the Next Start** writes that file, so no FTP client is needed to ask for one.

## Building

Beside this folder: PS5_Vulkan, PS5_Mesa, PS5_PayloadSDK and ps5-native-app-boilerplate
(`ps5/tools/bootstrap.sh` checks and sets them up, as in the template).

```bash
ps5/tools/bootstrap.sh --check     # what is there and what is missing
ps5/tools/fetch-pcsx2.sh           # PCSX2 at the pinned revision, with ps5/patches/pcsx2 applied
ps5/tools/build.sh                 # dependencies, libraries, link, sign, notices: dist/PPSA99249/
ps5/tools/check-imports.py         # the title's imports, by provider
ps5/tests/run-host-tests.sh        # the tests that run on the PC
ps5/tests/driver/build-host-driver.sh <Mesa tree> <dir> asan   # the driver's sources, built for the PC...
ps5/tests/driver/run-driver-tests.sh <dir>                     # ...and its threaded recording checked
ps5/tools/stage-notices.py --check dist/PPSA99249   # the check before a folder is passed on
ps5/tools/make-release-zip.py v1.0.0                # the ZIP, the source beside it, SHA256SUMS
ps5/tools/deploy.sh                # upload what changed over the console's FTP server
ps5/tools/run.sh [game]            # a test run on the console
```

Built from PCSX2 `5b6e6e23eba4` (v2.9.131), the payload SDK fork `b5efad5`, PS5_Vulkan `f410e67`
and PS5 Vulkan Template `91179df`. RADV is PS5 Mesa `9c03200` (the revision PS5_Vulkan pins) with
the six patches of `ps5/patches/mesa/` (their README says what each does and where it comes from),
built by `ps5/tools/build-radv-crux2.sh`; without that archive, `build.sh` links PS5_Vulkan's own
release archive, which has none of them.

The emulator's core, recompilers and GS are optimised across files at link time (PCSX2's
`LTO_PCSX2_CORE` option, ThinLTO at `-O3`; PCSX2's own release builds are link-time optimised
too). Checked on the PC: the program imports the same names as without it, has no native
thread-local storage (the console's is emulated), and its functions that call others keep
their frame pointers. On the console
it was worth about 3% in the measured scene ([What was checked](#what-was-checked)). `build/ps5/build-identity.txt` and the boot log's second line carry one
digest over every input of the build.

PCSX2 is kept as a pinned revision plus one patch, exported by `ps5/tools/fetch-pcsx2.sh
--export`. Before this is published as a repository, the patch should become a fork of PCSX2
that the build pins by revision, and upstream should be merged into it regularly.

## Profile-guided builds

A build can be compiled with what the emulator actually does on the console (profile-guided
optimisation): the compiler then lays the hot paths out together, inlines the calls that are
taken and turns the usual target of an indirect call (PCSX2's GS dispatches through tables of
them) into a direct one. It takes three steps, the middle one on the console:

1. `PS5PX_PGO=generate ps5/tools/build.sh` makes a profiling build (in `build/ps5-pgo-generate`;
   its version ends in `-profile` when `PS5PX_VERSION` says so). It counts every branch and
   function it runs, and which function each indirect call reaches. It is slower while it
   counts. LLVM's profile runtime is not in the SDK: `ps5/tools/build-profile-runtime.sh` builds
   it for the console from compiler-rt at the release of the SDK's clang (llvmorg-18.1.3), with
   three stand-ins of its own (no host name, no `procctl`, `getpagesize` through `sysconf`), so
   the profiling build imports nothing the normal one does not. Its counters are placed with the
   writable data (`link-title.sh`), where the native tool expects them.
2. On the console, play the games the build should be good at, for some minutes each, with the
   settings they are played with. When a game closes and when Crux2 closes, the counts so far are
   written to `<data>/pgo/crux2-<when the run started>.profraw` (the boot log's `pgo:` lines say
   where and how much); Settings > System > Log > copy takes them along with the logs. A file has
   function names and counts only: nothing of a game.
3. `ps5/tools/merge-profiles.sh <files or folders>` merges them into `ps5/pgo/crux2.profdata`
   (`PS5PX_PGO_EQUAL=1` with a folder for each game makes the games weigh alike), and
   `PS5PX_PGO=use ps5/tools/build.sh` compiles with it (in `build/ps5-pgo-use`; the build
   identity names the profile, and a new profile compiles everything again).

What it is worth is for the same scene to measure before and after; nothing has been measured
yet.

## Layout

| Path | What |
|---|---|
| `ps5/frontend/ps5_host.cpp` | `main()` and everything the emulator asks of its host: the emulation thread's loop, the display, messages, settings defaults, the game list scan, test runs |
| `ps5/frontend/ps5_input_source.cpp` | The controllers (scePad), under the SDL source's names so settings and profiles carry over |
| `ps5/frontend/ps5_audio_stream.cpp` | Audio output (AudioOut): a worker thread that writes 256 frames at a time, 48 kHz |
| `ps5/frontend/ps5_platform.cpp` | The title folder, the boot log, leaving the sandbox, `network.cfg`, USB folders, file permissions |
| `ps5/frontend/ps5_crash.cpp` | The crash report |
| `ps5/frontend/ps5_profiler.cpp`, `ps5_profile_fold.h` | A test run's profiler: the sampling of the emulator's threads, and the folding of samples into functions by name |
| `ps5/frontend/ps5_card_image.h` | A memory card held in memory and written back behind the game |
| `ps5/frontend/ps5_mc.h`, `ps5_cards.cpp`, `bigpicture/bp_cards.cpp` | The memory card manager: a card's file system and the save files (with nothing of the console in it), the changes made to card files, and the card's menu |
| `ps5/frontend/ps5_presets.h` | The name servers of online services the Network page offers by name |
| `ps5/frontend/bigpicture/` | The interface: the screens from PSFlyCast (`bp_*.cpp`, with no PCSX2 in them; `bp_library.cpp` the library and its views, `bp_splash.cpp` the four start-ups, `bp_care.cpp` the care for the screen, `bp_draw.cpp` the mark, the name and the skins) and `bp_pcsx2.cpp`, which is their door to PCSX2 and stands in for PCSX2's `FullscreenUI` |
| `ps5/frontend/ps5_disc_cache.h` | A disc in the drive, read ahead of the game and held in memory (used by the patch's disc thread) |
| `ps5/frontend/ps5_extras.cpp`, `ps5_extras_logic.h` | A memory card for each game, texture packs from a drive or a share, the state saved by itself |
| `ps5/frontend/ps5_backup.cpp`, `ps5_logs.cpp`, `ps5_jobs.cpp`, `ps5_online.cpp` | Backups, the logs on the screen and their copy, and what is fetched from the network (covers and the games' other pictures, cheats, PCSX2's patches) |
| `ps5/frontend/ps5_ui.h` | What the host tells the interface from other threads: a start's progress, a scan, the controllers |
| `ps5/frontend/ps5_letter_jump.h` | The order of titles (and the letter groups of the earlier game list, with their test) |
| `ps5/frontend/ps5_libc.c` | `getaddrinfo` (numeric addresses and the console's resolver) and the libc functions the console lacks |
| `ps5/frontend/ps5_native.cpp`, `ps5_native_loader.cpp` | Native code modules: the module for the running executable, loaded into executable memory; the call PCSX2's recompiler compiles into a block (the hook is in `ps5.patch`, `iR5900.cpp`) |
| `ps5/recomp/` | The recomp kit: `tools/recomp-game.py` (a game's modules on the PC, Linux and Windows; `recomp-game.sh` calls it), `tools/ps2disc.py` (the executable and the .iso out of a .chd, .cue/.bin or .cso; `tools/accel/` its native helper for one codec), `tools/ps2unpack.py` (a packed executable unpacked, by running its decompressor in Unicorn), `tools/build-module.py`, `runtime/` (what a native module is built with: the boundary `crux2_native_abi.h`, PS2Recomp's macros on PCSX2's memory map), `patches/ps2recomp-crux2.patch` (PS2Recomp's Crux2 mode), `tests/` (the PC test against stand-in memory; `tests/disc/` the disc reader against chdman's images) |
| `ps5/frontend/vfs/` | The virtual file layer: SMB (libsmb2) behind the emulator's ordinary file calls |
| `ps5/common/` | PCSX2's platform layer for the console: memory (shared views, the code area, fastmem, page faults), threads, timing |
| `ps5/elevation/` | The sandbox elevation client and helper (BlackBearReloaded's example, with this title's ID) |
| `ps5/patches/pcsx2/ps5.patch` | The changes to PCSX2 itself, each behind `__PROSPERO__`: the build, the Vulkan loader and display surface, files, the controller defaults, 16 KiB pages |
| `ps5/src/` | The template's PS5 layer: klog, the splash, the exit |
| `ps5/notices/`, `ps5/package/` | The parts and their licences; the notices and notes a title folder carries |
| `ps5/patches/mesa/` | The changes to the graphics driver (PS5_Mesa) this build's RADV archive carries |
| `ps5/tools/`, `ps5/tests/` | The build, the link (with `make-symbols.py`: the function names for the profiler), the checks, the console tools, the host tests, `tests/driver/`: the driver's threaded recording on the PC, and `tests/bigpicture/`: the interface on the PC |

## Licences and credits

**Crux2 by Press5elect.** Crux2's own parts (the port layer, the interface, the tools, and the
changes to PCSX2 in `ps5/patches/pcsx2/ps5.patch`) are Copyright (C) 2026 Press5elect.

PCSX2 is GPL-3.0-or-later, and so is this port as a whole. It may be used, changed and passed
on under that licence, with its source: this source archive, and the revisions that
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
| PS5 Vulkan Template (the PS5 layer, the tools this build is made from) | Mihawk; Sascha Willems | MIT |
| PS5_Vulkan (link recipe, packaging tool, `libc.prx`) and PS5_Mesa (RADV for the console) | Mihawk; Mesa's authors | GPL-3.0; MIT |
| PS5 platform layer (payload SDK fork) | Mihawk; John Tornblom and ps5-payload-dev | GPL-3.0-or-later |
| USB drive access (`sandbox-elevator.elf` and its client) | BlackBearReloaded, ps5-native-app-boilerplate | GPL-3.0-or-later |
| PS2Recomp (the converter the recomp kit fetches and patches; its macros in `ps5/recomp/runtime/upstream/`) | ran-j and the PS2Recomp contributors | GPL-3.0 |
| def-jam-vendetta-recomp's work on PS2Recomp (the recomp kit's `runner/def-jam-vendetta-recomp.patch`, `runner/vu1_recomp.py`) | jcsix694 | GPL-3.0 |
| What the recomp kit's Windows converter (`recomp/bin/windows-x64`) is built with: ELFIO, toml11, fmt, rabbitizer, nlohmann/json; libdwarf; mingw-w64's runtime and GCC's libstdc++ | their authors | MIT; LGPL-2.1; the mingw-w64 licences and the GCC Runtime Library Exception |
| libsmb2 | Ronnie Sahlberg | LGPL-2.1-or-later |
| curl; OpenSSL | Daniel Stenberg and contributors; the OpenSSL Project | curl licence; Apache-2.0 |
| The libraries PCSX2 bundles and depends on (Dear ImGui, fmt, libchdr, LZMA SDK, libzip, rcheevos, SoundTouch, Zydis, FreeType, libpng, libjpeg-turbo, libwebp, zstd, LZ4, shaderc, glslang, SPIRV-Tools, PlutoSVG, rapidyaml and others) | their authors | their own; all compatible with GPL-3.0 (`licenses/`) |
| Fonts (Roboto, Twemoji, Font Awesome Free, PromptFont) | Google; Twitter and Mozilla; Fonticons; Shinmera | Apache-2.0; CC-BY-4.0; SIL OFL 1.1 |
| Roboto Bold (the interface's headings) | The Roboto Project Authors | SIL OFL 1.1 |
| Saira ExtraBold (the letters of the title's name) | The Saira Project Authors (Omnibus-Type) | SIL OFL 1.1 |
| The start-up sound (`resources/sounds/silicon-boot.wav`) | supplied by me for this title; it is mine to pass on with it | the title's own |
| FSR 1, the upscaling filter (`resources/shaders/common/fsr1/`) | Advanced Micro Devices: FidelityFX Super Resolution 1.0, unchanged | MIT |
| The layout of memory cards and save files, for the card manager (`ps5/frontend/ps5_mc.h`) | written anew after the source of mymc+ (Florian Märkl and contributors, based on Ross Ridge's mymc), which the PC tests also run as a second opinion; the unpacking of `.max` files after Haruhiko Okumura's LZARI | GPL-3.0-or-later; LZARI is free to use |
| Certificate bundle (`resources/cacert.pem`) | Mozilla, via certifi | MPL-2.0 |
| The title's mark and name (Crux2) and its pictures in the console's menu | Press5elect: drawn by `ps5/frontend/bigpicture/bp_draw.cpp` and `ps5/tools/make-title-art.py`; the line of text is set in Roboto | GPL-3.0-or-later; Apache-2.0 |

"PlayStation", "PS2", "PS5" and "DualSense" are trademarks of Sony Interactive Entertainment.
This project is not affiliated with or endorsed by Sony Interactive Entertainment or the PCSX2
project. Vulkan is a registered trademark of the Khronos Group; the RADV driver in this build
is not a conformant product.
