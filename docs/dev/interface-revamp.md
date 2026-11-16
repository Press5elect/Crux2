# Revamping the interface: the playbook

How the 2026-11 revamp (phases 1 to 5, commits 9666870 to 2e244cf) was done, written so the next
one can be done the same way. It applies to any large change to the Big Picture interface
(`ps5/frontend/bigpicture/`): a new look, new screens, or settings moved about. Read
[`workflow.md`](workflow.md) first; its rules always come before this page.

## What the owner expects (the standing brief)

- **Nothing lost.** No option, setting, action or screen is removed. Things may move; each one that
  moves is listed with where it lives now.
- **PCSX2's words.** A setting PCSX2 also has keeps PCSX2's label and description word for word
  (with their source line). A setting of Crux2's own is written in PCSX2's voice and Title Case, so
  nobody can tell it apart. Never "Crux2 own" or the like: the default is the default. PCSX2's
  descriptions keep the games they name (rule 4's exception).
- **Like with like** (rule 8). The top bar holds places only: Home, Library, and Disc while a disc is
  in. Settings is the Options button. Storage (console, USB, share) is a library filter. Tools such
  as the texture pack manager are reached from Settings, a game's page and the library. One thing
  never gets two doors that look alike.
- **Every skin, every size.** All 36 skins (`[PS5UI] Look`), the accent colours, Interface size
  70 %, Text size Smallest and Largest, High contrast and the colour-blind colours must look right.
  Draw with the `col::` tokens and the skin's roundness, never with fixed colours.
- **Real data only.** A card or a line shows what the Backend really knows. When a figure does not
  exist (achievements of a game that is not running, a live resolution), the item is left out, not
  invented.
- **"Top notch."** The owner looks at pictures. Look at them before the owner does.

## The order of work

### 0. Mockups, and nothing built

Draw the screens first, as HTML mockups (1280x720, one file per screen, kept in the scratchpad or
published as an Artifact), and show them to the owner. Use the existing look's words and fake game
names only. Build nothing until the owner gives the go-ahead; ask about the things that are theirs
to choose (which library layouts to keep, what goes in the top bar) and write the answers down
here or in `workflow.md`.

The 2026-11 mockups: Home, the settings home, a settings area, search, the quick menu, a game's
page, the library as a grid and as a list, the performance overlay in three sizes. Design
language: dark ground, cards a step lighter with a hairline edge, one accent colour for focus and
the chosen item, small-capitals labels over cards, the button hints at the bottom right with Cross
rightmost.

### 1. An inventory and a no-loss test (before touching a screen)

Make the list of everything that exists, and a test that fails when something goes. For settings
this is [`settings-inventory.md`](settings-inventory.md): `settings-inventory.json` (every row, its
key, kind, choices, default and flags), the inventory dump made from the code by the preview, and
`run-bigpicture-tests.sh <dir> inventory`. The host tests run it too, so CI catches a lost row.

For actions that are not settings (a menu's entries, a screen's buttons), the worker who moves them
writes a table "before → where now" in their report, and the crawl and the preview's scripted
steps must still reach each one.

Bless the baseline (`settings-inventory.py bless --dump <dump.json>`) only when the change is
meant, and say in the commit what changed (labels, descriptions, choice names). Never bless to make
a failure go away.

### 2. The words, for review (not yet on the screen)

`settings-text.yaml` holds every row's words, area, section and level, and where PCSX2's words come
from (`pcsx2_source: file:line`). `settings-text.py`:

- `make` writes it from the code; `review --out <file>` makes a page for the owner;
- `doubtful` lists the rows whose area or section is a guess, for the owner to decide;
- `apply` makes the reviewed words the rows' words; `bake` writes `bp_settings_text.h`;
- `check` compares the file with the code and the inventory (0 problems before any commit).

Phase 2 changed only words, and was reviewed by the owner before phase 3 put them on the screen.

### 3. The screens, one area at a time, by workers that own files

Split the screens between at most two or three workers, each owning whole functions or files
(for example: one the top bar, Home and the library's frame; another the game page and the quick
menu). In files they share they make small hunks with Edit, re-read before each edit, and never
rewrite a whole file. Nobody commits but the lead.

A worker's brief carries: the files and functions it owns and those it must not touch; the mockup
files; the standing brief above (nothing lost, PCSX2's words, rule 8, skins, real data only); the
checks below, with their result lines pasted into the report; preview shots it must add and read
itself; and "report what remains console-only". When the owner changes their mind mid-phase, send
the change to the worker it touches at once (SendMessage), and write the decision down.

### 4. An independent review

When the screens are in, start a **read-only** reviewer on the commits, not told the lead's
conclusions, working in its own `git worktree`. Ask for concrete failing scenarios only: settings
written to the wrong layer (all games, one game, the interface's own), values not saved or not
applied (Restart / NextStart), stale indices after the game list changes, a press handled twice in
one frame, button clashes between screens, data shared with other threads without a lock, and work
done every frame (sorting, scanning, file calls). The 2026-11 review found a regression (Change disc
could no longer reach the drive) and a settings bug (the overlay's size stuck at Custom) that every
test had passed. Check each finding against the code yourself before fixing it.

### 5. Polish

The checklist that phase 5 went through:

- text that is cut off at Text size Largest or Interface size 70 % (wrap or choose what to show);
- messages (toasts) that cover hints or content;
- the button hints in one order on every screen (`hintBar` sorts them: Circle, shoulders, L2/R2,
  directions, L3, R3, touch pad, Options, Square, Triangle, Cross);
- motion: short (0.2 s), never in the way of a press, and Motion Less = fades only, None = none;
- one card radius, one edge, one label style; the skin's roundness applied once (`rect` and
  `outline` already multiply by `col::round`);
- empty states on every screen (no games, nothing pinned, no save states, no share);
- old wording left behind (old tab names, old level names, old button meanings) in the code, the
  README and the settings' descriptions;
- per-frame cost: cache anything that sorts or scans the library, keyed on `libraryGeneration` or
  the settings' build count;
- a sheet of every new screen in all 36 skins, and a sheet of the ways of seeing.

## The checks, every phase

All of them, on the whole tree after the workers are done, run by the lead (not only by the
workers):

```bash
VFS_TEST_DIR=/tmp/x-host ps5/tests/run-host-tests.sh                    # all host tests passed
ps5/tests/bigpicture/run-bigpicture-tests.sh /tmp/x-bp crawl            # 0 failed (two seeds)
ps5/tests/bigpicture/run-bigpicture-tests.sh /tmp/x-bp inventory        # nothing lost (731 rows), 0 problems
ps5/tests/bigpicture/run-bigpicture-tests.sh /tmp/x-bp shots            # 0 failed
python3 ps5/tools/settings-text.py check                                # 0 problems
```

Then **read the pictures yourself** (the new screens, a light skin, 70 % with Largest text) before
sending them to the owner; workers have reported clean pictures that were taken before another
worker's change. New logic with no console in it gets a host test (as `ps5/tests/osd/` for the
overlay's sizes and colours).

Disk: a shots run writes about 1.6 GB of PPM files. Keep work folders under `/tmp`, convert or
delete the PPMs, and remove old work folders before a run.

## Committing

- Commit coherent states only: a phase when its checks pass, or a finished piece that stands on
  its own (a rule written down, a fix with its test). While workers are mid-edit, say so to the stop
  hook instead of committing.
- When a file holds both finished and unfinished work, stage only the finished hunks
  (`git diff file > full.diff`, keep the hunks, `git apply --cached`).
- Write the message to a file and `git commit -F`; run the commit and the push as two commands.
- No game titles (fake names only in preview fixtures), no model identifiers.
- Each phase updates `CHANGELOG.md` (Unreleased, `- Changed:` / `- Added:` / `- Fixed:`), the README
  (settings named by their place: "Settings > Area > Section > **Label**") and
  [`open-work.md`](open-work.md) (what was checked on the PC only, and what to check on a console).

## After the last phase: a test build

`check-sources.sh` (rule 1), then `PS5PX_VERSION=vX.Y.Z-test ps5/tools/build.sh`, then a ZIP of
`dist/PPSA99249` for the owner with a short list of what to try first on the console, in the order
a person would meet it (start-up and Home, the library, a game's page, the quick menu in a game,
Settings, the overlay). Say plainly that none of it has run on a console until the owner says so,
and start from the boot log when something goes wrong.
