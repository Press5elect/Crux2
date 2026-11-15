# Open work

What is known to be unfinished, untested on a console, or deliberately left. Update it when an
item is done or something new is learned; delete what no longer applies.

## State of the branch

- The branch was reverted to the v1.0.18 release tree in 883c832 (the owner asked to go back to
  the release on GitHub). The v1.0.19/v1.0.20 work is in history before it:
  [`../history/v1.0.19-v1.0.20-reverted.md`](../history/v1.0.19-v1.0.20-reverted.md). Do not assume
  anything from it is in the tree.
- b075fe1 and the work after it are Unreleased ([`CHANGELOG.md`](../../CHANGELOG.md)). **Nothing of
  it has run on a console**; the host tests and the interface crawl pass.
- The pins were moved on 2026-11-15 (PCSX2 master after v2.9.179, PS5_Vulkan
  `7f492b4`, the SDK `3345cde` with upstream v0.45's C++ runtime, PS2Recomp `e563698`); `check-sources.sh`
  reports every source current. None of it has run on a console. Save states from before don't load.
- USB controllers (DualShock 3, Xbox 360 pads, guitars and drums): never tried on a console.
- VRR has not been tried on a TV that has it.
- Surround: which console channel each ends up in has not been heard.
- Online play through the network adapter's DNS presets (PS2 Online, PS Rewired) has not been tried.
- The overlay's EE/GS/VU percentages are each thread's own report; whether a title may read
  another thread's processor time is not measured.
- Not every setting has been tried one by one; paraLLEl-GS not in every game; native code modules
  and many PS2 accessories not tried. PGO's worth has not been measured before/after.
- Screen care: how fast a panel ages is not measured.
- Looks' black screen (GSDeviceVK_PS5.cpp, DoLook): the look's picture is now left in the shader
  read layout and PCSX2's vertex buffer bound again after the look's passes (the two causes the
  reverted eca7923/d01f2cd found). Not run on a console. To check: a Look (e.g. CRT) shows the game,
  the menus over it are drawn; the boot log has `look: frame 1 made, WxH from WxH`.
- Downsampling ([PS5] Downsample) and SMAA ([PS5] SMAA), both off by default (ps5_aa.h, ps5_aa.glsl,
  GSDevice::PS5Antialias, GSDeviceVK_PS5.cpp's DoDownsample/DoSMAA). Checked on lavapipe only
  (`tests/aa`). On a console, check: with Upscaling Off or CAS and 8x at 4K, Soft and Sharp show a
  picture with less shimmer than Bilinear, nothing shifted or cropped, the menus fine; the boot log's
  `picture: downsampling's passes made`, `picture: downsampled (Sharp) from ... to ...`, `picture:
  SMAA's passes made`, `picture: SMAA runs at WxH`; a paused game (menu open) looks the same as
  running; with frame generation and a look on top; HDR and debanding after it. GPU cost: the test
  window's `downsampling and SMAA` figure in the boot log's GPU line (and the overlay's conversions).
  Not measured: the cost at 8x-12x (Sharp's two passes reach up to 33 texels each at a ratio of 8).
- The HD texture pack manager (bp_textures.cpp, ps5_texture_pack_manager.cpp; the file work in
  ps5_texture_pack_store.cpp, host-tested in `tests/texturepacks/store_test.cpp`; the screen in the
  interface crawl). Not run on a console. To check: the manager lists packs on the console, a USB
  drive (elevated) and a share, with sizes (`texture packs: ... measured in` lines; how long 100k
  files take on a drive and over SMB); a move console to USB and back, and to a share (the share's
  copy is written with `ps5vfs_store_from`, file by file: its speed), stopped half-way (the source
  as it was, `replacements.crux2-moving` beside the target) and begun again; a delete stopped and
  finished; two downloads queued one after the other; the bar during a move. Free space: the
  console gives a title none (statvfs is a stand-in), so the "may not fit" question never comes on
  the console; a pack too large stops with the drive full. Whether rename() works on an exFAT USB
  drive from the elevated title (the move's last step there) is not known.
- PS Move: whether the console's own libSceMove gives a payload process Move data is unknown; the
  probe in [`../../ps5/probes/move/`](../../ps5/probes/move/README.md) (built, never run) calls it with
  the PS4's signatures and logs to `/data/crux2-move-probe.log`. Nothing in the title uses it yet.

## Known problems

- paraLLEl-GS is slower than PCSX2's renderer in the heaviest game tested, and its picture is
  softer by design.
- The free BIOS (OpenPS2BIOS) is early work: many games reach their menus and go no further.
- RetroAchievements' hardcore mode is not offered; its sounds are not played.
- 50 Hz output does not exist on the console: PAL games alternate refreshes.

## Partial or not ported

- From the reverted v1.0.20 work (LRPS2/ARMSX2 studies), never ported: target widening for a
  constant Z, and LRPS2's native taps for an indexed read of a scaled target. The rest of that work
  is in history (2bf2d08 and before) and would have to be brought back.
- From the reverted v1.0.19 work: Settings > Debug > Logs, network shares in Settings > Folders,
  the boot log writer thread, the safety save in memory, the USB headset, CJK names: all out of
  the tree now (`settings-text.yaml` is back, made again from the code: the interface revamp's
  phase 1, [`settings-inventory.md`](settings-inventory.md)). b075fe1's boot log in memory overlaps with the
  v1.0.19 log settings; reconcile before restoring either.
- The interface revamp: phase 1 (the inventory, the no-loss test, `settings-text.yaml`) is in;
  phase 2 (Crux2's descriptions rewritten in PCSX2's voice, PCSX2's taken word for word) and phase 3
  (the eight areas on the screen, the interface reading `bp_settings_text.h`) are not. 60 rows' area
  or section is a guess (`ps5/tools/settings-text.py doubtful`), for the owner. About 18 of today's
  descriptions (game fixes and a few graphics fixes, copied from PCSX2) name games; phase 2 should
  word them without. The inventory does not see rows of states its fakes do not make (a typed cheat,
  saved controller profiles, backups on the drive, multitap ports, a share being changed).
- Porpoise ports in progress (networking, input/aim, texture-pack memory cap, steady 30,
  colour-blind filter, trophy pop-ups, launch arguments, recommended settings, backups,
  screenshots): see the Unreleased changelog as they land.

## Decisions on record

- PCSX2 stays a pinned revision plus `ps5.patch`; nothing goes upstream.
- Behaviour that cannot be checked without a console starts off by default.
- Release assets are the ZIP and `SHA256SUMS`.
- `PS5PX_PGO_SPLIT` stays off: measured worse on the console (2026-10-12).
- The RSQRT.S negative-zero change was not made: a console capture showed the existing behaviour
  was right.

## Found while integrating the Porpoise ports

- `ps5/tests/ps2ftp/run-ps2ftp-tests.sh` (not part of run-host-tests.sh) fails 4 of 13 checks on the
  PC, the same before and after the networking changes: not looked into.
- Before the Light gun choice, nothing in the interface plugged a GunCon 2 in, and
  `PS5InputSource::SetGunCon2Defaults` had no caller: the light gun worked only through a hand-edited
  ini. The choice now does both; check on the console that a light-gun game sees the gun.
- The ugen code (raw USB) comes from Porpoise's open pull requests (pr36, pr37), not its main branch;
  the notices entry names main's revision.
- The recomp kit's three runner files (PS5_Crux2Recomp's `ps5/runner/`, on its `main` since
  2026-11-15) come from kit v1.6.4, older than the
  runner and Vendetta patches in `crux2recomp/ps5/patches/ps2recomp/` (rebased for PS2Recomp e563698).
  The kit is made and its tools start, but no module has been made with it since: check one game on
  Windows and Linux before a release ships it.
- **Mesa is held at `0618bfe0ffb`** (`ps5/patches/mesa-27/HOLD`, the owner's decision of 2026-11-15).
  The refresh to Mesa main `aead7293cfd` (commit 9ad6b51; its 0012 is 38 MB with deleted binaries'
  old contents stripped, `ps5/tools/strip-binary-delete-preimage.py`) hung the GPU on each game's
  first start with a cold shader cache (`radv/ps5: the GPU has not reached submission ...`); the same
  build with this merge played fine. Before refreshing again, bisect 0618bfe0ffb..aead7293cfd (ACO's
  scheduler rework and RADV's push-constant/descriptor rework are the first suspects; the four
  hand-merged files are listed in 9ad6b51's message) and test a game's first start on the console.
  `check-sources.sh` reports Mesa as held while 0012 is the HOLD revision.
- HD texture packs download in 4 MiB pieces over 10 connections by default (a fresh connection per
  piece, slow pieces restarted): untested on the console. Whether the Internet Archive throttles per
  connection or per address shows in the boot log's `texture packs:` lines every 5 s; up to N+3
  pieces (52 MiB at 10) are held in memory. `Download connections` = 1 is the old single stream.
- CI skips the download's end-to-end test: its apt line lacks `libcurl4-openssl-dev libarchive-dev`.
- `ps5/tests/bigpicture/run-bigpicture-tests.sh <dir> shots` reports 21 failed checks (quick menu,
  skin, screen saver) with or without the download bar: not looked into (the crawl passes).

