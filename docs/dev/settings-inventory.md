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
else the Qt interface's tooltips, else PCSX2's controller tables), and `pcsx2_tooltip` where the Qt
interface says something else as well. A PCSX2 text that names games is not copied (rule 4):
`*_withheld` says so and the source line is given. `dynamic: true`: the row's words come from data
(PCSX2's tables, a game's patches, a drive's name). `doubtful:` says why an area or section is a guess
(`settings-text.py doubtful` lists them).

```bash
ps5/tools/settings-text.py make      # writes the file from the inventory (keeps what was edited by hand)
ps5/tools/settings-text.py check     # every row has an entry, no stale ids, valid fields, the code's words
ps5/tools/settings-text.py bake      # bp_settings_text.h, the table the interface will compile in
```

In phase 1 the interface does not read `bp_settings_text.h` yet, and `check` requires the file's
words to be exactly the code's, so nothing on the screen can change. Phase 2 rewrites Crux2's own
descriptions in PCSX2's voice and uses PCSX2's word for word; phase 3 makes the interface take its
words and places from the baked table (and lifts that check).
