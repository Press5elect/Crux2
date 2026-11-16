# Move probe

A standalone payload, not part of the title: it asks whether the console's own PS Move library,
`/system/common/lib/libSceMove.sprx`, gives a homebrew process Move data. Nothing in Crux2 uses it
yet; the answer decides whether a Move can be read the way games read it.

Move Management (GPL-3.0) found that the library loads in a payload process on firmware 13.60 and
that its functions are exported. Nobody had called them. This probe calls them, with the PS4's
signatures (REMovePS4's `scemove.h`, shadPS4's `move.h`); **the PS5's signatures are not known**.

## What it does

1. Opens `/data/crux2-move-probe.log` (every line `fsync`'d at once) and posts a notification.
   A hard stop (`alarm`) ends the process after 45 s; a crash or the hard stop writes one line
   naming the step it happened in.
2. Loads the library with `sceKernelLoadStartModule` on its path (Move Management's way). Only if
   that fails, `sceSysmoduleLoadModule(0x008F)` (the PS4's ID; the PS5's is not known). It also
   `dlopen`s the path through the payload runtime.
3. Resolves `sceMoveInit`, `Term`, `Open`, `Close`, `ReadStateLatest`, `ReadStateRecent`,
   `GetDeviceInfo`, `SetLightSphere`, `SetVibration`, `ResetLightSphere`: `sceKernelDlsym` by name,
   then by NID (as `ps5/frontend/ps5_keyboard.cpp` does), then `dlsym`. It looks up
   `scePadIsMoveConnected` in libScePad but **does not call it** (its signature is not known).
4. `sceUserServiceInitialize`, `sceUserServiceGetInitialUser` (the foreground user if that fails),
   `sceMoveInit`, then `sceMoveOpen(user, 0, 0)` and `sceMoveOpen(user, 0, 1)`.
5. For 30 s, about 100 times a second per open index, `ReadStateLatest` into a buffer whose first
   64 bytes (the PS4 structure) are zeroed and whose next 4032 bytes are a sentinel (`0xCD`); once a
   second `ReadStateRecent` (room for 64 entries plus a guard). Notifications prompt: 0-10 s every
   button, 10-20 s the T trigger, 20-30 s tilt and turn.
6. For each open index: `GetDeviceInfo`, the light blue then orange (1 s each), a 0.3 s rumble at
   128, `ResetLightSphere`, `Close`; then `Term`, a `RESULT:` line, a notification and `complete`.

## What it never touches

No `/dev/ugen*`, no `sceUsbd`, no `libSceBluetoothHid`, no HCI command, no pairing, no other
process. (In Move Management's tests, everything that broke DualSense reconnection came from
opening `/dev/ugen0.2` or reading the HCI event endpoint.) Its imports are libkernel and
libSceUserService; the library itself is loaded at run time, so a missing export cannot stop the
payload from starting.

## Before running it

- **Move Management is not running** (reboot if it was sent since the last start).
- **The Move is paired to the PS5 itself.** Plug it into the console by USB, press PS, unplug it,
  and check that it moves the home-screen cursor. A Move that Move Management re-pointed to an
  external Bluetooth adapter has to be paired again to the PS5 this way (or restored with Move
  Management's `RESTORE_ORIGINAL=1`).
- **Check the DualSense first**: turn it off (hold PS for 10 s or from the quick menu) and press PS:
  it must reconnect. Do the same after the probe, and say in the report whether it did.
- Stay signed in as the user the Move is assigned to (the probe opens for the initial user).

## Building and sending

```bash
ps5/probes/move/build.sh                 # -> build/move-probe.elf (the pinned SDK, setup-sdk.sh)
.deps/native/ps5-payload-sdk/bin/prospero-deploy -h <console-ip> -p 9021 build/move-probe.elf
# or: nc -q0 <console-ip> 9021 < build/move-probe.elf
```

Port 9021 is the payload loader (elfldr, which kstuff setups run), the one Crux2's sandbox
elevator and notification relay are sent to. Hold the Move while it runs; it ends by itself in
about 35 s (45 s at most).

## What to send back

`/data/crux2-move-probe.log` (FTP, or any file manager), and whether the DualSense reconnected
before and after, the light colours seen and whether it rumbled.

## Reading the log

Each line starts with seconds since start.

| Line | Meaning |
| --- | --- |
| `sceKernelLoadStartModule(...) -> handle/rc` | A handle (>= 0) means the library loaded; negative is the kernel's refusal |
| `path load failed: sceSysmoduleLoadModule` | Only after a refused path load: the sysmodule route with the PS4's ID |
| `libSceMove in the module list` / `dlopen` | Where the library is found in the process; the module list is printed when something is missing |
| `resolve <name> nid <nid> -> <address> (<how>)` | Each function's address and which lookup found it, or `missing` |
| `scePadIsMoveConnected: present/missing` | Looked up only, never called |
| `sceUserServiceInitialize ... user` | The user the Move is opened for (a non-zero Initialize can just mean already initialised; what matters is a user >= 0) |
| `sceMoveInit() ->` | 0 is success; `0x80EE0002` already initialised |
| `sceMoveOpen(user, type 0, index N) ->` | A handle >= 0, or an error |
| `port N: ReadStateLatest rc=` | Logged on the first read and whenever it changes |
| `ReadStateLatest wrote past byte 64` | The PS5 structure is larger than the PS4's (the bytes after 64 are shown) |
| `first non-zero state` | The first 64 bytes the library wrote, raw |
| `counter A -> B, timestamp` | The first five counter changes and the time between samples |
| `buttons ... -> ...` | Every change of the button word, decoded with the PS4's bits |
| `port N: t=... counter=... (+K in 0.5 s) ...` | Every 0.5 s: counter, samples in that half second, buttons, T, accel (g), gyro (rad/s), temperature, timestamp |
| `ReadStateRecent(since) rc count` | Once a second: how many past samples it returned |
| `GetDeviceInfo`, `SetLightSphere`, `SetVibration`, `ResetLightSphere`, `sceMoveClose`, `sceMoveTerm` | Return codes; device info decoded as the PS4's sphere radius and offset, plus raw bytes |
| `RESULT:` | The one-line verdict |
| `HARD STOP` / `CRASH ... during step: X` | It did not finish; X is where it stopped |

## The outcomes

| Seen | Means |
| --- | --- |
| `rc=0` and the counter changing, buttons and motion following the prompts | **It works**: the console's library gives a homebrew process Move data |
| `rc=0x1` throughout | The library works, but no Move is bound to that user (check the pairing and the signed-in user) |
| Open `0x80EE0006` (invalid port) or `0x80EE0007` (already open) | Refused: the system or another process holds the Move, or the PS5 refuses this process |
| Other `0x80EE00xx` | See `error_name` in `move_probe.c` (the PS4's codes) |
| Load or every lookup failing | This route is closed for a payload; another loading route would be needed |
| `CRASH` in a call | The PS5's signature differs from the PS4's for that function |
| `rc=0` but a frozen counter, or numbers that make no sense | The structure differs on the PS5: the raw bytes are in the log |

Not tested on a console: written and built on the PC only.
