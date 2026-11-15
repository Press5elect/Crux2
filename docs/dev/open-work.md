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
- The pins are v1.0.18's. PCSX2 is at v2.9.150 while upstream had v2.9.179 on 2026-11-15; Mesa
  main, the SDK, PS5_Vulkan and PS2Recomp were also moved on in the reverted work and are behind.
- PS5_Vulkan is pinned at `f410e67` (what v1.0.18 was built with; `vulkan_revision` in
  `ps5/tools/bootstrap.sh`); the checkout beside the repository is at `ace98a9`.
- On 2026-11-15 `check-sources.sh` reports six sources behind (PCSX2, Mesa main, PS5_Vulkan, the
  SDK, PS2Recomp, and the checkout's PS5_Vulkan): the pins are moved before the next build.

## Untested on a console (Unreleased, from b075fe1)

| Item | State / what to check |
| --- | --- |
| HDR10 output (Settings > Graphics > HDR, Adjust HDR brightness) | Host test of the passes only. **Needs RADV rebuilt with `ps5/patches/mesa-27/0020`** (`build-radv-crux2.sh`) and `param.json`'s attribute `0x20000000` (in the tree). Check `wsi/videoout: framebuffers HDR10:` and `wsi/videoout: output status` lines |
| Debanding | Host test of the pass |
| Startup video (`ps5_intro.cpp`, libvpx) | **On by default** (`[PS5] StartupVideo`), against the rule that untested behaviour changes start off: the owner should confirm or it should default off |
| Game fixes from GitHub at start | **On by default** (`FixesAtStart`); `ps5/game-fixes.txt` lists no fixes yet, and the title fetches the list from `main`, so nothing is fetched until a list with entries is on `main` |
| Boot log kept in memory | **The default** (`LogMode = memory`): a run that does not crash leaves no log unless Save the log is chosen. Confirm on a console that a crash, a failed start and the 15 s no-picture case do write it |
| Save the log in the quick menu | Present (`bp_quick.cpp`); not run |
| Share stall panel | A read stuck for 2 s is said on the screen and the GS thread keeps presenting (`ps5vfs_stalled_seconds`, the waker in `ps5.patch`); not run |
| Exit-write bar, settings written when menus close | Not run |
| Customisable quick menu, Apply changes now | Host test of the layout; not run |
| Covers three at a time, the cover loader queue | Not run |
| 50 ms tap hold | Host test; not run |
| Developer-build label, updater pruning stale files | Host test of the updater; not run |
| PlayStation Move as GunCon 2 (Move Management) | Off by default; host test against a stand-in producer; needs the Move Management payload (`/home/user/ext/move` on the build machine) |
| Frame generation's counters | Host test; not run |

## Older items untested on a console

- USB controllers (DualShock 3, Xbox 360 pads, guitars and drums): never tried on a console.
- VRR has not been tried on a TV that has it.
- Surround: which console channel each ends up in has not been heard.
- Online play through the network adapter's DNS presets (PS2 Online, PS Rewired) has not been tried.
- The overlay's EE/GS/VU percentages are each thread's own report; whether a title may read
  another thread's processor time is not measured.
- Not every setting has been tried one by one; paraLLEl-GS not in every game; native code modules
  and many PS2 accessories not tried. PGO's worth has not been measured before/after.
- Screen care: how fast a panel ages is not measured.

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
  the boot log writer thread, the safety save in memory, the USB headset, CJK names,
  `settings-text.yaml`: all out of the tree now. b075fe1's boot log in memory overlaps with the
  v1.0.19 log settings; reconcile before restoring either.
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

