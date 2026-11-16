# The settings inventory, the no-loss test and settings-text.yaml

Phase 1 of the interface's revamp (the settings regrouped into eight areas). Nothing the user sees
changes in it: it records every option there is today, makes sure none is lost on the way, and puts
every row's words in one file to edit. The owner's rule for the revamp: **no option is deleted**; all
ten library layouts and all 36 skins stay.

## The inventory (`ps5/frontend/bigpicture/settings-inventory.json`)

Every row the settings can show, made from the code itself: `bp_options.cpp`'s pages and
`bp_pcsx2.cpp`'s (the console's Backend: `extraRows`, the controllers with PCSX2's own controller and
hotkey tables, the memory cards, the network, the folders, the logs, the achievements, a game's
"This game" page), for the settings of all games and for a game's own, at every level. For each row:
its id, page, heading, label, kind, section and key, default, choices (names and values), range,
the switches a combined row is made of, flags (`Advanced`, `Expert`, `AllGames`, `OneGame`, `Front`,
`NextStart`, `Restart`, `NoGame`), whether another row makes it usable, and its description. Then the
library's layouts and the skins, by the number `[UI] View` and `[PS5UI] Look` keep.

How it reaches the real rows: `ps5/tests/bigpicture/inventory/inventory.cpp` `#include`s
`bp_pcsx2.cpp` itself and is built for the PC against PCSX2's headers (`build-inventory.sh`), with the
interface's other files as they are. The PCSX2 files the pages read while they are made are compiled
for real (the settings layers, `Pcsx2Config.cpp`, the pad types, the hotkey lists, `GS.cpp`); the few
console functions they ask are faked in `host_fakes.cpp` as a console would answer them; every other
symbol is a stub made by `make-stubs.py` that stops the program with its name. So `bp_pcsx2.cpp` needs
no change for it, and a page that starts to need something new fails the test loudly instead of
leaving rows out. The pages are made twice: on a new console with one USB drive in ("new"), and set up
(signed in at RetroAchievements, a game folder and a share, a share being added, USB controllers on:
"set-up"), so that rows shown only in one of those states are in it too. Not in it: rows of states the
fakes do not make (a typed cheat, saved controller profiles, backups found on the drive, multitap
ports, the share being changed rather than added), and what only a console shows.

## The no-loss test

```bash
ps5/tests/bigpicture/run-bigpicture-tests.sh /tmp/crux2-bp inventory   # also in run-host-tests.sh (and CI)
ps5/tools/settings-inventory.py check                                   # the same, building in a temporary folder
ps5/tools/settings-inventory.py show                                    # counts by page, kind and level
```

It fails when a setting of the baseline (its section and key), a binding, an action, a layout or a
skin is gone; when a setting is no longer offered for one game (or for all games) where it was; and
when a setting's kind, default, choices (their values), range or combined switches changed. A layout
or skin under another number fails too: the number is what the settings keep. New rows, and changed
words, levels, flags and places, are only listed.

**Blessing a new baseline**, for a change that is meant to remove or change an option (never to make
the test pass):

```bash
ps5/tests/bigpicture/inventory/build-inventory.sh /tmp/crux2-inv
ps5/tools/settings-inventory.py bless --dump /tmp/crux2-inv/dump.json   # prints what changed, writes the baseline
```

Put what it printed, and why, in the commit.

## settings-text.yaml

`ps5/frontend/bigpicture/settings-text.yaml` has an entry for every row of the inventory, under its
page and heading of today: its `label`, `desc` and `choices` (the names), and for the revamp `area`
(Picture, Sound, Controls, Games and saves, Emulation, Network, Interface, System), `section` (the part
of the area), `level` (Essentials, More, Everything: today's Normal, Advanced, Expert; `level_one_game`
where a game's page shows it at another), and `origin` (`pcsx2` or `crux2`). A PCSX2 setting has
PCSX2's own words beside it, from PCSX2 at the pin without `ps5.patch`: `pcsx2_title`,
`pcsx2_description` and `pcsx2_source` (file:line; Big Picture's `FullscreenUI_Settings.cpp` first,
else PCSX2's controller and hotkey tables, else the Qt interface's tooltips and forms), and
`pcsx2_tooltip` where the Qt interface says something else as well. PCSX2's texts are copied whole,
the games they name included (the owner's decision of 2026-11-15, an exception to rule 4).
`dynamic: true`: the row's words come from data (PCSX2's tables, a game's patches, a drive's name).
`doubtful:` says why an area or section is a guess (`settings-text.py doubtful` lists them).

```bash
ps5/tools/settings-text.py make      # writes the file from the inventory (keeps what was edited by hand)
ps5/tools/settings-text.py check     # every row has an entry, no stale ids, valid fields, the code's words, the new words' checks
ps5/tools/settings-text.py bake      # bp_settings_text.h, the table the interface will compile in
ps5/tools/settings-text.py review --out /tmp/review.html   # the review sheet: every row, today's words and the new
ps5/tools/settings-text.py apply     # phase 3: the new words become the row's words, then bake
```

In phase 1 the interface did not read `bp_settings_text.h`, and `check` required the file's words to
be exactly the code's. Phase 2 wrote the new words beside today's; phase 3 applied them and made the
interface take its words and places from the baked table: `bp_settings_words.cpp` puts each entry's
label (not a `dynamic` row's), description and choice names on the row it finds by its id, on its page
and under its heading of today. So `check`'s comparison of the file's words with the code's still
holds: the code's rows, as the inventory makes them, show the file's words. The labels in the code
are the rows' handles: an action's or an information line's id is made from the label the code gives
it (the dump's `code_label` when the file's words took its place), so ids do not change when words do.
The settings' screens group the rows by `area` and `section` (a row with no entry goes where the row
before it on its page goes).

## The new words (phase 2)

The owner's requirement: PCSX2's descriptions word for word wherever PCSX2 has the setting, and
everything else in PCSX2's style. Each entry has, beside today's `label`, `desc` and `choices`:

| Field | What |
| --- | --- |
| `new_label`, `new_desc`, `new_choices` | the words to be (the choice names only: their values never change) |
| `text` | the description's kind: `exact`, `written` or `exact-but-wrong` (every row has one) |
| `text_source` | where PCSX2's text is (file:line at the pin), for `exact` and `exact-but-wrong` |
| `label_text`, `label_source` | the label's kind (`exact` or `written`) and where PCSX2's is; none where the label is data |
| `choices_text`, `choices_source` | the choice names' kind (`exact`, `mixed`, `written`, `exact-but-wrong`) and PCSX2's list |
| `text_note` | why a text is exact-but-wrong, or what else is worth knowing |

- **exact**: PCSX2's text at the pin, byte for byte (a `\n` or a trailing space included). The
  setting is matched by section and key, never by label. FullscreenUI's text is preferred, then the
  controller and hotkey tables, then the Qt interface's tooltip (`pcsx2_tooltip` keeps Qt's where it
  differs). Labels and choice names are FullscreenUI's for that key where it has them, else the
  tables' or the Qt form's (a form label's colon left off). A choice is mapped to PCSX2's name by its
  value. A few actions are matched to the PCSX2 button that does the same (Scan For New Games, Login,
  Save Profile...: `text_note` says so); the four crop rows take Qt's text for their side, since
  FullscreenUI has one row for all four.
- **exact-but-wrong**: PCSX2's text for the setting, untrue of this row (it joins switches, offers
  more, or states something the console does not have). The owner decides; `apply` leaves today's
  words for it.
- **written**: Crux2's rows, and PCSX2 settings PCSX2 gives no text, in PCSX2's voice:
  - one or two sentences (three at most), present tense, starting with what it does: Enables,
    Determines, Selects, Sets, Shows, Controls, Uses, Applies, Reduces, Allows, Prevents, Forces;
    "When enabled, ..." for what a switch then does; "When disabled, ..." for the other side;
  - the cost or risk as PCSX2 states it: "Reduces performance.", "May cause issues in some games.",
    "Can improve performance in some games.", "Not supported in all games.";
  - PCSX2's terms: internal resolution, upscaling, hardware/software renderer, save state, memory
    card, on-screen display (OSD), game list, cover, BIOS, PS2, virtual machine, vsync, input lag,
    FMV, hotkey, bindings, input profile, game fixes, patches, cheats, network adapter, HDD, startup;
  - American spelling, as PCSX2 writes (color, center, gray, license);
  - labels in Title Case, a noun phrase or Enable/Show/Use + noun, as PCSX2's ("Show FPS",
    "Fast Boot", "Swap OK/Cancel"); choice names in Title Case, the standard value marked
    `(Default)` where PCSX2 marks one, "Default" where a choice is the standard one itself, "Use
    Global Setting" for a game's "as in the Settings";
  - never: "Crux2" (except a folder's name in quotes), "our", "own" meaning ours, "as before",
    "new", "now", version numbers, history, implementation details, file names the user need not
    know, "PS5's"; and no "Takes effect at the next start" or "Experimental" (PCSX2 never says them:
    the row's `NextStart`/`Restart` flag says the first, a risk sentence the second).

`check` adds, for the new words: every `exact` (and `exact-but-wrong`) description, label and
choice list is PCSX2's at its source, read again from `vendor/pcsx2`'s git HEAD (the pin without
`ps5.patch`), naming the row and the source when not; written words have none of the banned words or
British spellings (a name in double quotes is left out of the search); no description is empty
(bindings and information lines may be: PCSX2's have none, a line shows its value) or another row's
by accident; a choice named `(Default)` is the row's default; every row has `text`.

`apply` (phase 3) wrote the new words over today's and baked (the four `exact-but-wrong` rows were
written in PCSX2's style first, the owner's decision). `check` runs the same checks on the applied
words. The inventory's baseline was blessed for the new words: only labels (490), descriptions (288)
and choice names (109 rows) changed, and `code_label` was added; no id, key, default, choice value,
range, layout or skin.

## Phase 3's places

The 60 `doubtful` places were decided and the notes taken out. Moved: Pause On Controller
Disconnection to Controls > Controllers; Warn About Unsafe Settings to Emulation > Emulation; the
download of cheats for all games and of game patches to Games and saves > Cheats and > Patches; a
game's patches and cheats (the rows, their warnings, Show Patches/Cheats For All CRCs, Enable Cheats,
Download Cheats, Enter Cheat Code) to Games and saves > Patches and > Cheats; Thread Pinning and the
driver's own core to System > PlayStation 5; the five frame-pacing rows (Frame Pacing, Maximum Frame
Latency, Queue Frames When Slow, Advanced Frame Display, Skip Presenting Duplicate Frames) to a section
of their own, Emulation > Frame Pacing; Move OSD Periodically to Interface > Screen care; the remote
control page and its two actions to Network > Remote Control. Kept where phase 1 put them: Swap
OK/Cancel (Interface), Show Gameplay Background (Interface > Library layouts), Sync Covers (Game
List), Console Language and the two fast-boot rows (BIOS), Black Frame Insertion (Output and HDR),
the texture dumping rows (Texture packs), Debanding and the color blindness filter (Looks), the USB
microphones and their tests (Controls > USB devices), Map to USB Controller (Controllers), Enable Game
Fixes, Enable Patches and the database's recommended settings (Game Fixes), the native code module
rows (System > PlayStation 5), and a game's own first page (Games and saves > This game, its first
section in a game's settings).
