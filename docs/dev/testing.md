# Testing

What can be checked on a PC, how, and what only a console can show. Console runs are in
[`debugging.md`](debugging.md#test-runs).

## The host tests: `ps5/tests/run-host-tests.sh`

The parts of the port with no console in them, built for the PC with the undefined-behaviour
sanitizer (and the address or thread sanitizer where they fit), warnings of the tests' own code as
errors. CI runs them on every push and pull request with gcc and with clang
([`.github/workflows/host-tests.yml`](../../.github/workflows/host-tests.yml)).

```bash
ps5/tools/fetch-pcsx2.sh                              # some tests take headers from vendor/pcsx2
VFS_TEST_DIR=/tmp/crux2-host ps5/tests/run-host-tests.sh       # work folder outside the repository
CC=gcc CXX=g++ ps5/tests/run-host-tests.sh            # as CI's gcc job does
```

Without `VFS_TEST_DIR` they work in a new temporary folder. `LIBSMB2_PREFIX` (a libsmb2 built for
the PC) enables the virtual file layer's test; `PS5_PAYLOAD_SDK_FORK` (default `../PS5_PayloadSDK`)
the host memory test. CI has neither, so it skips those two.

| Section | Covers |
| --- | --- |
| libc stand-ins | `ps5_libc.c` against the PC's libc |
| memory card image | `ps5_card_image.h`: random saves, the file compared after each flush |
| letter jumps | `ps5_letter_jump.h`: order, groups, L2/R2 landings |
| quick menu entries | `bp_quick_layout.h`: shown, hidden, ordered |
| frame generation's figures | `ps5_pacing.h` (FrameGenFigures) |
| the performance overlay | `ps5_osd_logic.h`: the levels (white, amber, red) at their thresholds, the sizes' presets, Custom after a switch changes (`tests/osd`) |
| PlayStation Move | `ps5_move_logic.h` and the loopback stream, against a stand-in producer |
| profile folding, profiler | `ps5_profile_fold.h`, `ps5_profiler.cpp` (plain, ASan, TSan) |
| thread times | `PS5ThreadTimes.h` under TSan |
| quick taps | the 50 ms hold (`ps5_input_source_logic.h`, TapHold) |
| the updater | `ps5_update.cpp` against a made-up GitHub: developer builds, files a new version dropped |
| host memory | `PS5HostSys.cpp` over `mock_platform.cpp`, 16 KiB pages (needs the SDK fork) |
| virtual file layer | a held read (a share that stops answering), shares woken; the full VFS test needs libsmb2 |
| boot log in memory | `ps5_logs.h` (memory_log_test) |
| texture pack manager | `ps5_texture_pack_store.cpp`: packs found, measured, moved (stopped, failing, begun again: the source kept), deleted, the queue |
| game fixes from GitHub | `ps5_fixes_fetch` (fetch, unchanged, tampered, unsafe paths, no network) |
| HDR and debanding | the last passes before the screen (`tests/hdr`) |
| downsampling and SMAA | `ps5_aa.h`'s decisions; `ps5_aa.glsl`'s passes on lavapipe against double precision, SMAA on staircases (`tests/aa`) |
| settings lock | `check-settings-lock.py`: no setting read through Host while the settings' lock is held |
| settings: nothing lost | `bigpicture/inventory/run-inventory-test.sh`: the console's settings rows made on the PC against the baseline and `settings-text.yaml` ([`settings-inventory.md`](settings-inventory.md)) |

## The interface: `ps5/tests/bigpicture/run-bigpicture-tests.sh`

```bash
ps5/tests/bigpicture/run-bigpicture-tests.sh /tmp/crux2-bp crawl   # ASan/UBSan: every row of every section (all games' and a game's, at Everything) changed every way, the settings' home and search, 2 x 60,000 random steps
ps5/tests/bigpicture/run-bigpicture-tests.sh /tmp/crux2-bp shots   # a PNG of each screen in /tmp/crux2-bp/shots
ps5/tests/bigpicture/run-bigpicture-tests.sh /tmp/crux2-bp inventory   # the no-loss test of the settings (settings-inventory.md)
ps5/tests/bigpicture/run-bigpicture-tests.sh /tmp/crux2-bp bench   # what a frame of each screen costs; kept in /tmp/crux2-bp/bench.txt
```

`bench` (optimised, no sanitizers, `-DBP_BENCH`) runs each main screen for 300 frames
(`BENCH_FRAMES`), with the preview's usual 33 games and with 3,033 (console, USB and share;
favourites, play times, saved states, sets of discs, six collections). It times the interface's
own code only (ImGui's NewFrame, the screens, Render: the draw lists made, nothing rasterised).

- `bench <fixture> <screen>`: average, median and worst microseconds of processor time a frame;
  vertices, indices and draw commands a frame; heap allocations a frame (operator new and ImGui's
  allocator, counted); settings read from the Backend a frame; and its heavy calls a frame (the
  game list, a game's states, a page of rows, the cards, the packs, a game's settings opened).
- `trans <fixture> <change>`: a change of screen (Home to a game's settings, to its page, the
  Settings, the quick menu with pins, and back): the worst single frame and which one, the first
  ten frames together, the allocations of the worst, the frames until the screen is all there, and
  what the first ten frames asked of the Backend (game lists, settings opened, states, pages,
  settings read).

The times are this PC's: compare a run with another on the same machine (they vary by a quarter
between runs on a busy one), never with the console. The counts (vertices, allocations, what is
asked of the Backend) are the same everywhere, and the Backend's are what the console pays most
for: there each is a lock, a walk through the game list or a look into a folder, which the
preview's Backend does not do. For a profile: build with `-DBP_BENCH -DBP_CALLGRIND` and run
`valgrind --tool=callgrind --instr-atstart=no preview bench <fonts> large 40 <part of a name>`,
which looks at the measured frames of the screens so named only.

The shots show the clock, so two runs differ there: to compare the pictures of two versions, hold
the time still (an `LD_PRELOAD` of `time()`) and compare the PPMs; then they are the same byte for
byte when nothing changed.

The first argument is the work folder: give a path outside the repository (a bare `crawl` makes a
`crawl/` folder wherever you are). It needs `vendor/pcsx2` (ImGui, fonts). Look at the shots after
any change to a screen.

## The other suites

Each runs on its own (most take a work folder); `run-feature-tests.sh <work folder>` runs many of
them together (interface sound, cover matching and cheat lists, network module and HDD image, the
disc watcher, memory cards, FSR 1, frame generation, pacing, the updater, the phone's page, input,
FSR 3 optical flow and NIS, looks).

| Script | Covers |
| --- | --- |
| `cards/run-cards-tests.sh` | The card file system and save files (`ps5_mc.h`), cross-checked against mymc+ |
| `cardsync/run-cardsync-tests.sh` | The memory card sync engine |
| `clock/run-clock-tests.sh` | The title's clock (`ps5_clock.c`) |
| `computefx/run-computefx-tests.sh` | FSR 3 optical flow, NIS, and FSR 3's interpolation for comparison ([its README](../../ps5/tests/computefx/fsr3fi/README.md)) |
| `discimage/run-discimage-test.sh` | Disc images written in each format and read back |
| `driver/build-host-driver.sh` + `run-driver-tests.sh` | The driver's threaded recording and timed waits, built from a Mesa tree for the PC |
| `fixes/run-fixes-tests.sh`, `hdr/run-hdr-tests.sh` | Game fixes fetch; HDR and debanding passes |
| `framegen/run-framegen-tests.sh`, `fsr/run-fsr-tests.sh` | Frame generation's and FSR 1's passes on a software Vulkan driver |
| `input/run-input-tests.sh` | Controller features (hold to leave, pressure, keymaps) |
| `ps2ftp/run-ps2ftp-tests.sh` | A PS2's memory cards over FTP |
| `slang/run-slang-tests.sh` | `.slangp` looks |
| `texturepacks/run-texturepacks-tests.sh` | Texture pack names and unpacking (not run by run-host-tests.sh) |
| `texturepacks/run-fetch-tests.sh` | The parallel ranged download (run by run-host-tests.sh): pieces in order, retries, slow restarts, the single-stream fallback, against `range_server.py` with curl and libarchive (needs `libcurl4-openssl-dev libarchive-dev`; skipped without them, as in CI today) |
| `uiaudio/run-uiaudio-tests.sh` | The interface's sounds and music |
| `update/run-update-tests.sh` | The updater |
| `web/run-web-tests.sh` | The phone/computer web panel |
| `run-vfs-tests.sh` | The virtual file layer against real Samba and FTP servers when installed |

## Other checks

- `ps5/tools/check-imports.py`: what the title imports, by provider; libc functions known to be
  missing or faulting on the console must be bound to the platform layer's.
- `ps5/tools/game-fixes-index.py --check`: `ps5/game-fixes.txt` is current.
- `ps5/tools/stage-notices.py --check dist/PPSA99249`: notices complete, nothing forbidden in the
  folder ([`releasing.md`](releasing.md)).
- Syntax-checking console code on the PC: missing libsmb2, zstd, rapidyaml and similar headers are
  the PC's, not the change's.

## What the PC cannot show

Which addresses the console's kernel grants, what its pad, audio, VideoOut and USB services answer,
what the graphics driver does on the GPU, real timing. Say in the commit and the notes which
features have and have not run on a console ([`open-work.md`](open-work.md)).
