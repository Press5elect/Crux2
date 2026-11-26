# Debugging

What Crux2 writes down, where, and how to get a measured run from the console. For the user's side
(Settings > Log, sending a log) see the [README](../../README.md#if-something-goes-wrong).

## The boot log

`crux2-boot.log` in the title folder (`/data/homebrew/PPSA99249/`), and the run before as
`crux2-boot.previous.log` (the one before that is gone). It holds each start-up step, down to the
graphics driver's (`radv: ...`; the driver's standard error is piped in), the emulator's warnings
and errors, the session's summary and, after a crash, the crash report as its last lines. PCSX2's
own log is `logs/emulog.txt`.

- **Line 2, `build identity`**: the digest over every input of the build
  (`build/ps5/build-identity.txt`). Check it first: it says which build actually ran.
- **Kept in memory by default (Unreleased, `[PS5] LogMode`).** With *Boot log* at "Only when
  something goes wrong" (`memory`, the default), the session's log is held in memory and written to
  the file after a crash, a game that did not start, 15 seconds without a new picture while a game
  runs, or **Save the log** (Settings > Log, or the quick menu). "Always written" (`always`) writes
  each line as it is logged, as before. It takes effect at the next start. The log says which, in
  `boot log:` lines. So **ask the owner to choose Save the log** (or set Always written) before
  sending a log of a run that did not crash.
- The data folder in use: a `data folder: ...` line (`/data/crux2` when it exists and is writable).

## Lines worth knowing

Grep the boot log for the prefix of the feature in question.

| Prefix | Says |
| --- | --- |
| `build identity` | Which build ran |
| `settings:`, `game fixes:` | The settings in effect and the game database fixes applied (and, Unreleased, the fixes fetched from GitHub) |
| `display:`, `wsi/videoout:` | The output mode chosen (`display: 3840x2160 at 119.88 Hz`, or why not), VRR, HDR10 framebuffers |
| `radv/ps5:` | Driver details, e.g. `GFX10.1 hardware workarounds on/off` |
| `picture:`, `framegen:`, `pacing:` | FSR 1's sizes, frame generation's state and figures (every 10 s, Unreleased), pacing |
| `test:` | A test run's windows: frame rate, speed, the game's own rate, gaps over 40 ms, short audio writes, frames queued; EE/GS/VU/GPU and draw counts |
| `slow:` | A game under full speed for 2 s: threads, GPU, what a frame drew |
| `profile:` | The sampling profiler's windows ([below](#test-runs)) |
| `paraLLEl-GS:` | Its GPU time, reads of GS memory and their cost, every 30 s |
| `GS: PS5: packed format` | The vertex formats a game draws in |
| `disc:` | Each drive: `/dev` names, open, size, what the disc was taken for, tray unlock and eject, the 30 s read-ahead figures |
| `USB drives:`, `usb pads:` | Sandbox elevation and the folders found; the USB controller library |
| `network shares:`, `net:` | Each share and the server's answer; the network adapter's lookups and connections |
| `covers:`, `pictures:`, `cheats:`, `patches:` | What was fetched |
| `move:` | The PlayStation Move source (Unreleased) |
| `hdr test:` | Adjust HDR brightness (Unreleased) |
| `intro:` | The startup video (Unreleased) |
| `update:`, `install:` | The updater |
| `broker:` | The write broker (Fast Writes to Console Storage, Unreleased): started or why not, why it stopped, a job's files, MiB and MiB/s |
| `auto-save:`, `native:`, `pgo:`, `mic:` | The state saved by itself; native modules; PGO counts; microphones |

## klog

The console's kernel log carries the title's lines too, live, which is what a test run captures
(PS5_Vulkan's klog tool, port 3232 by default; the console's address is in PS5_Vulkan's `.env`).
klog survives a hang that leaves the boot log unwritten.

## Crashes and hangs

- A crash writes a report to the boot log: the signal, the registers and the return addresses as
  offsets into the program, to be read against the build's ELF (`build/ps5/link/llvm-pie.elf`, the
  build named by `build identity`). `ps5/tools/run.sh` symbolises it.
- When the run before ended in a crash, the library says so when it opens, with the game that was
  loaded. A title closed from the console's own menu is not a crash.
- **A hang**: an empty `watchdog.txt` next to `eboot.bin` makes the title report where each thread
  was when it stopped answering.
- **A black screen after leaving the sandbox**: an empty `usb-off.txt` next to `eboot.bin` skips the
  elevation.

## Test runs

A launch that finds `test-run.txt` in the title folder deletes it, does what it says, writes `test:`
lines to the boot log and klog, and ends through the same shutdown as Quit Crux2:

```
seconds 60
game games/my-test-disc.iso
screenshot
```

Every 5 seconds it reports the frame rate, the emulation speed, the game's own rate, the longest gap
between frames, the gaps over 40 ms and the short audio writes; at the end the same over everything
after the first window, a screenshot (`snaps/test-run.png`) and `test: end`.
`ps5/tools/run.sh [game]` does all of it with PS5_Vulkan's tools (writes the file, launches through
ps5vkctl, captures klog until `test: end`, a crash or the exit, symbolises a crash, fetches the boot
log and the screenshot). `ps5/tools/deploy.sh` uploads what changed over FTP first.

Two more lines:

- `profile` samples the graphics thread every 2 ms and every ten seconds writes `profile:` lines:
  the share of the emulator's code, the driver's, the platform layer's, other libraries' and outside
  the program, then the functions by name (from `symbols.txt`, written by the link), and the
  instructions interrupted at most as offsets for the disassembly. `profile all` samples the
  emulation and VU threads too. Windows under 15% busy are not written; a thread at most 60 times.
- `threaded-recording` sets `RADV_THREADED_RECORDING=1` for that launch.

A file with `profile` or `threaded-recording` and neither `seconds` nor `game` is a run played by
hand, ended from the interface. Settings > PlayStation 5 > **Profile the next start** writes that
file.

## Diagnostic switches

| Switch | Effect |
| --- | --- |
| `[PS5] LogMode = always` | Every line written as it is logged |
| `RADV_PS5_GFX101_WORKAROUNDS=0` | The GFX10.1 hardware workarounds off (Mesa 0014) |
| `RADV_PS5_WAIT_SPIN_US`, `RADV_PS5_WAIT_SLEEP_US` | GPU waits' spin and sleep (set from the settings) |
| `PS5_VIDEOOUT_59HZ`, `PS5_VIDEOOUT_VRR=1` | Keep 59.94 Hz; ask for variable refresh |
| `watchdog.txt`, `usb-off.txt` | See above |
