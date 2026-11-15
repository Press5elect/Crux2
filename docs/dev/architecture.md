# How the port is laid out

Crux2 is PCSX2's emulator core (fetched into `vendor/pcsx2` and patched) with a PS5 host of its own
in place of PCSX2's Qt and desktop front ends, a Big Picture interface from PSFlyCast in place of
PCSX2's `FullscreenUI`, and RADV (Mesa's Vulkan driver, built for the console) linked into the
title. The title ID is `PPSA99249`; it runs from `/data/homebrew/PPSA99249/` (`/app0` inside the
sandbox) and leaves the sandbox only for USB drives, through the elevation helper.

## The tree

| Path | What |
| --- | --- |
| `vendor/pcsx2/` | PCSX2 at the pin plus `ps5/patches/pcsx2/ps5.patch` (gitignored; [`patches.md`](patches.md)) |
| `ps5/common/` | PCSX2's platform layer for the console: memory (shared views, the code area, fastmem, page faults: `PS5HostSys.cpp`), threads, timing, thread times |
| `ps5/frontend/` | The host: everything PCSX2 asks of its host, and the console's services (below) |
| `ps5/frontend/bigpicture/` | The interface (below) |
| `ps5/frontend/vfs/` | The virtual file layer: SMB (libsmb2), NFS and FTP behind the emulator's ordinary file calls; shares woken at start, stalled reads counted (`ps5vfs_stalled_seconds`) |
| `ps5/frontend/third_party/` | FidelityFX (FSR 1, FSR 3 optical flow), MihawkGen, NIS, dr_libs, stb_vorbis, qrcodegen, each with its licence |
| `ps5/pgs/`, `crux2recomp/` | paraLLEl-GS, the second GS renderer, built from Crux2 Recomp's sources (`ps5/frontend/ps5_pgs.cpp` is its door) |
| `ps5/recomp/` | The native-code-module kit: `tools/recomp-game.py` (`recomp-game.sh`), `ps2disc.py`, `ps2unpack.py`, `build-module.py`, `runtime/` (`crux2_native_abi.h`), `patches/ps2recomp-crux2.patch`, `tests/` |
| `ps5/elevation/` | The sandbox elevation client and helper (BlackBearReloaded's example, with this title's ID) |
| `ps5/src/` | The template's PS5 layer: klog, the splash, the exit |
| `ps5/patches/` | `pcsx2/ps5.patch`, `mesa-27/` (and the older `mesa/`), `deps/` |
| `ps5/package/` | What the title folder carries besides the program: `LEGAL.txt`, the folders' README texts, `resources/` (shaders, fonts, sounds, the free BIOS; game fixes go in `resources/game-fixes/`, empty so far) |
| `ps5/sce_sys/` | `param.json` (content version; `attribute` 0x20000000 lets VideoOut take HDR10, and the 120 Hz flag) and the title's pictures |
| `ps5/notices/components.json`, `ps5/licenses/` | Every third-party part, its licence and revision; staged into `licenses/` by `stage-notices.py` |
| `ps5/art/intro/` | The startup video and its sources ([README](../../ps5/art/intro/README.md)) |
| `ps5/ASSETS.md` | Every non-code asset, its origin and licence |
| `ps5/game-fixes.txt` | The list of game fixes the title fetches from GitHub `main` (`game-fixes-index.py`) |
| `ps5/pgo/` | The committed PGO profile (`crux2.profdata`) |
| `ps5/tools/`, `ps5/tests/` | Build, link, checks and console tools; the host tests ([`testing.md`](testing.md)) |

## The host (`ps5/frontend/`)

| File(s) | What |
| --- | --- |
| `ps5_host.cpp` | `main()` and the emulator's host: the emulation thread's loop, the display, messages, settings defaults, the game list scan, test runs |
| `ps5_platform.cpp` | The title folder and the data folder (`/data/crux2`), the boot log (in memory or written, `LogMode`), leaving the sandbox, `network.cfg`, USB folders, permissions |
| `ps5_crash.cpp` | The crash report written to the boot log |
| `ps5_input_source.cpp`, `_logic.h` | The controllers (scePad) under SDL's names, so settings and profiles carry over; the 50 ms tap hold |
| `ps5_move_source.cpp`, `ps5_move_logic.h` | A PlayStation Move as player 1's GunCon 2, read from Move Management's loopback stream |
| `ps5_mic.cpp`, `ps5_keyboard.cpp` | USB microphones and keyboard |
| `ps5_audio_stream.cpp` | Audio output (AudioOut): a worker writing 256 frames at a time, 48 kHz |
| `ps5_pacing.cpp`, `ps5_framegen_pacer.h`, `ps5_clock.c` | Frame pacing, frame generation's pacing and figures, the title's clock |
| `ps5_hdr.h` | HDR10 output's state and calibration (the passes are in the patch's `GSDeviceVK_PS5.cpp`) |
| `ps5_aa.h` | Downsampling's and SMAA's decisions and push constants (the passes: `resources/shaders/vulkan/ps5_aa.glsl`, recorded in the patch's `GSDeviceVK_PS5.cpp`; SMAA's textures in `third_party/smaa`) |
| `ps5_compute_fx.cpp`, `ps5_slang.cpp`, `ps5_mihawkgen_model.cpp` | Compute effects (FSR 3 optical flow, NIS), `.slangp` looks, MihawkGen |
| `ps5_osd.cpp`, `ps5_auto_quality.cpp`, `ps5_thread_layout.cpp`, `ps5_priority.cpp` | The on-screen display, Automatic quality, thread pinning |
| `ps5_disc.cpp`, `ps5_disc_cache.h`, `ps5_disc_image.cpp` | Disc drives, the disc read into memory, disc images made |
| `ps5_card_image.h`, `ps5_mc.h`, `ps5_cards.cpp`, `ps5_card_sync.cpp`, `ps5_ps2_ftp.cpp` | Memory cards in memory, the card manager, card sync, a PS2's cards over FTP |
| `ps5_extras.cpp`, `ps5_texture_packs*.{h,cpp}`, `ps5_texture_pack_{store,manager}.{h,cpp}` | A card per game, the auto-save, texture packs from drives and shares, their downloads, the manager's moves and deletes |
| `ps5_backup.cpp`, `ps5_logs.cpp`, `ps5_jobs.cpp`, `ps5_online.cpp` | Backups; the logs on screen and their copy; jobs on drives and shares; covers, pictures, cheats, PCSX2's patches from the network |
| `ps5_fixes_fetch.cpp` | Game fixes fetched from GitHub at start, checked against `ps5/game-fixes.txt` |
| `ps5_update.cpp` | The updater (GitHub releases; prunes files a new version no longer ships) |
| `ps5_intro.cpp` | The startup video (VP9 through libvpx, Vorbis) |
| `ps5_native.cpp`, `ps5_native_loader.cpp` | Native code modules |
| `ps5_net.cpp`, `ps5_presets.h`, `ps5_services.cpp` | What the emulated network adapter and hard drive need from the console, name-server presets, the title's services wired to PCSX2's pieces |
| `ps5_profiler.cpp`, `ps5_profile_fold.h`, `ps5_pgo.cpp` | A test run's sampling profiler; PGO counters |
| `ps5_free_bios.cpp` | OpenPS2BIOS when no BIOS is there |
| `ps5_libc.c`, `compat/` | `getaddrinfo` and the libc functions the console lacks; `compat/` a `malloc.h` and the stdio redirect |
| `ps5_ui.h`, `ps5_ui_audio*.cpp` | What the host tells the interface from other threads; the interface's sounds and music |

## The interface (`ps5/frontend/bigpicture/`)

The screens came from PSFlyCast's front end and have no PCSX2 in them (`bp_*.cpp`):
`bp_library.cpp` (the library and its ten views), `bp_settings.cpp` and `bp_options.cpp` (the
settings pages, grouped as PCSX2's settings window groups them), `bp_quick.cpp` and
`bp_quick_layout.h` (the quick menu), `bp_splash.cpp` (start-ups and the video), `bp_care.cpp`
(screen care), `bp_draw.cpp` (the mark, the name, the 36 skins; 30 of them generated into
`bp_kit_skins.inc` by `make-kit-skins.py`), `bp_cards.cpp` (the card manager), `bp_textures.cpp` (the texture pack manager). `bp_pcsx2.cpp` is
their door to PCSX2: it stands in for `FullscreenUI` and adds the PS5 rows to the pages
(`extraRows("library" | "interface" | "motion" | "output" | "picture" | "saves" | "system" |
"network" | "controllers")`). `ps5/tests/bigpicture/preview.cpp` runs the same screens on a PC.
`settings-inventory.json` (every settings row, made from the code) and `settings-text.yaml` (their
words, and the revamp's areas) are the revamp's records ([`settings-inventory.md`](settings-inventory.md)).

## The build's output

`dist/PPSA99249/`: `eboot.bin`, `sce_sys/`, `resources/`, `sandbox-elevator.elf`, `symbols.txt`,
`README.md` and `CHANGELOG.md` (copied from the repository's root), `LEGAL.txt`, `NOTICE.txt`,
`licenses/`. The ZIP is made from this folder ([`releasing.md`](releasing.md)).
