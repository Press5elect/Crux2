# Native code modules (experimental)

A game's main-processor code can be converted ahead of time on a PC (with PS2Recomp) into a module
that Crux2 runs natively in place of the recompiler, between the instructions only the emulator can
run. Not tried on the console game by game: treat it as an experiment.

## Making a module

1. On your PC (Linux Mint or Ubuntu), unpack the newest recomp kit (`Crux2-recomp-kit-<version>.zip`) and install
   `git cmake ninja-build clang lld python3`.
2. `recomp/tools/recomp-game.sh GAME.chd` (or the game's `.iso`, `.cue`, `.cso`, a folder of disc
   images, or the executable itself: the file `BOOT2` names in its `SYSTEM.CNF`, read out of the disc
   by itself). Add `--for both` for Crux2 Recomp's folder too, and `--ghidra <Ghidra's folder>` to have
   Ghidra find the functions (better on retail games). The first run fetches PS2Recomp and builds its
   converter; compiling a large game takes long.
3. Copy `SLUS_123.45.crux2native` to `/data/crux2/native/` on the console, and start the game.

## On Windows

The kit makes Crux2 Recomp's folders on Windows too: with Python 3 and LLVM 18 or newer installed,
drop a game's `.chd` (or `.iso`, `.cue`, `.cso`, or a folder of them) on `Crux2 Recomp.cmd`, and
copy the folder it makes in `output` (the executable, its module and the disc as an `.iso`) into
`/data/crux2recomp/`. The kit brings PS2Recomp's converter built for Windows. Crux2's native modules
are made on Linux.

## From a network share

With the games on a network share: start the game once in Crux2, and it puts its executable
in `Crux2-native/executables` on that share. Then, with the share mounted on the PC,
`recomp/tools/native-batch.sh /mnt/ps2/Crux2-native` (add `--watch` to keep it running) makes every
waiting game's module into `Crux2-native/modules`. The game's next start copies its module in by itself.

## What runs natively

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

The kit is made with `ps5/tools/make-recomp-kit.sh` ([`dev/building.md`](dev/building.md#the-recomp-kit));
the loader is `ps5/frontend/ps5_native*.cpp` and the hook in `ps5.patch` (`iR5900.cpp`).
