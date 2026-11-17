# Building Crux2

How the title is built on an x86-64 Linux PC, what it is built from, and how the pins are kept
current. Release builds have their own steps in [`releasing.md`](releasing.md).

## What sits beside this repository

Crux2 is built on Mihawk's PS5 Vulkan Template and its stack, checked out beside this repository (the sandbox elevation helper, from BlackBearReloaded's
ps5-native-app-boilerplate, is vendored in `ps5/elevation/` and built by `build-elevator.sh`):

| Checkout | What Crux2 takes from it |
| --- | --- |
| `../PS5_Vulkan` (`PS5_VULKAN_DIR`) | The link recipe, the native tool, `libc.prx`, the packaging, the console tools (ps5vkctl, klog, FTP) and the RADV build (`tools/build-radv.sh`, which names the PS5 Mesa revision it pins) |
| `../PS5_Mesa` (`PS5_MESA_FORK`) | RADV for the console; `build-radv-crux2.sh` makes a local `crux2-radv` branch in it (nothing is pushed) |
| `../PS5_PayloadSDK` | The payload SDK fork and its platform layer, exported at the revision `ps5/tools/setup-sdk.sh` pins |
| `../PS5_Crux2Recomp` (optional, `PS5_CRUX2RECOMP_DIR`) | paraLLEl-GS's sources; without it `crux2recomp/` in this repository is used |

`ps5/tools/bootstrap.sh --check` reports what is there; without `--check` it clones and builds what
is missing (the payload SDK at PS5_Vulkan's pin, the native tool and `libc.prx`, RADV's release
archive, ps5vkctl, PCSX2 in `vendor/pcsx2`, this repository's SDK pin). It needs git, cmake,
clang 18 or later, python3, glslangValidator, rsync, curl, zstd and make. The console's address
goes in PS5_Vulkan's `.env` (`PS5_HOST=...`), which every console tool reads; never commit it.

## The commands

```bash
ps5/tools/bootstrap.sh --check          # what is there and what is missing
ps5/tools/fetch-pcsx2.sh                # PCSX2 at the pin with ps5/patches/pcsx2/ps5.patch
ps5/tools/build-radv-crux2.sh           # RADV with ps5/patches/mesa-27 (long; after a Mesa patch change)
ps5/tools/build.sh                      # libraries, link, sign, notices: dist/PPSA99249/
PS5PX_VERSION=v1.0.19 ps5/tools/build.sh   # the version shown in the title (none by default)
ps5/tools/check-imports.py              # the title's imports, by provider
```

`build.sh` runs `build-deps.sh` (the libraries: libsmb2, curl, OpenSSL, libvpx and the others,
into `.deps/`), builds paraLLEl-GS (`build-parallel-gs.sh`), compiles `vendor/pcsx2` and
`ps5/{common,frontend}` with the SDK's toolchain, and links and packages with `link-title.sh`
(which also copies `README.md` and `CHANGELOG.md` into the title folder). `--compile` stops before
the link.

| Variable | Meaning |
| --- | --- |
| `PS5PX_VERSION` | The version the build is made as (`v1.0.19`): About page, top bar, updater |
| `PS5PX_RELEASE=1` | A release build; only from a clean tree, otherwise build.sh says so and makes a developer build ("v1.0.18 - developer build", whose updater offers that version's release) |
| `PS5PX_PGO` | `use` (the default when `ps5/pgo/crux2.profdata` is there), `generate`, `cs-generate`, `off` |
| `PS5PX_ALLOW_STALE=1` | Build even when the source check finds a pin behind  |
| `PS5_CLANG` | The host clang the compiler wrappers drive (default `clang`) |

`build/ps5/build-identity.txt` and the boot log's second line carry one digest over every input of
the build.

## RADV

The title links `.deps/native/radv-crux2` in PS5_Vulkan when it is there, else PS5_Vulkan's own
release archive (which has none of Crux2's patches). `build-radv-crux2.sh` takes PS5 Mesa at the
revision PS5_Vulkan pins, commits each patch of `ps5/patches/mesa-27` (the default;
`CRUX2_MESA_PATCHES=ps5/patches/mesa` for the older 26.2.5 series) on a local `crux2-radv`
branch, and builds the archive. The boot log's Mesa revision is that local branch's, not one on
GitHub. **`build.sh` does not rebuild RADV**: after a Mesa patch changes, run it yourself. HDR10
output needs an archive built with `0020`. The patches are described in [`patches.md`](patches.md)
and [`ps5/patches/mesa-27/README.md`](../../ps5/patches/mesa-27/README.md).

## Pins and the source check

Where each pin lives, and its value on this branch (the v1.0.18 tree plus b075fe1):

| Source | Pinned in | Pin |
| --- | --- | --- |
| PCSX2 | `ps5/tools/fetch-pcsx2.sh` (`revision=`) | `840774630f7a` (master of 2026-11-17, after v2.9.179; `0ab00c49b157` in v1.0.21, three commits behind it) |
| Payload SDK fork | `ps5/tools/setup-sdk.sh` (`sdk_revision=`) | `3345cde` (over upstream's v0.45 release; was `b5efad5`, over v0.42) |
| PS5_Vulkan | `ps5/tools/bootstrap.sh` (`vulkan_revision=`); the checkout beside this repository is checked against it | `7f492b4` (v1.0.18 was built with `f410e67`) |
| PS5 Mesa | PS5_Vulkan's `tools/build-radv.sh` (`mesa_revision=`) | `1ba9874` (at PS5_Vulkan `f410e67` and `ace98a9`) |
| Mesa main (the 27.0-devel merge) | `ps5/patches/mesa-27/0012` | `0618bfe0ffb` (2026-10-31), **held** (`ps5/patches/mesa-27/HOLD`: the 2026-11-15 refresh hung the GPU) |
| paraLLEl-GS, Granite, volk, Vulkan headers | `crux2recomp/ps5/tools/fetch-parallel-gs.sh` | `cc6184a`, `16e7395`, `47cddf7`, `11d6898` |
| PS2Recomp | `crux2recomp/ps5/tools/fetch-ps2recomp.sh` (`revision=`) | `e563698` |

The reverted v1.0.19/v1.0.20 work had moved several of these (PCSX2 to v2.9.166, the SDK to
`e105cf6`, PS5_Vulkan pinned at `ace98a9`, Mesa main to `6b41b56dea2`, PS2Recomp to `e563698`):
[`../history/v1.0.19-v1.0.20-reverted.md`](../history/v1.0.19-v1.0.20-reverted.md).

The rule ([`workflow.md`](workflow.md#the-rules)): before building, every source is checked against
what it follows, and a source that is behind is reported and its pin moved, with the patches on it
re-exported. `ps5/tools/check-sources.sh` does this (git ls-remote of each upstream, and PCSX2's
newest release tag), and `build.sh` runs it first. (Both went out with the revert in 883c832 and
were restored after it.)

### Checking by hand

```bash
git ls-remote --tags https://github.com/PCSX2/pcsx2 'v2.9.*' | grep -v '\^{}' | sort -t/ -k3 -V | tail -1
git ls-remote https://gitlab.freedesktop.org/mesa/mesa refs/heads/main
git -C ../PS5_Vulkan fetch && git -C ../PS5_Vulkan log --oneline -1 origin/main
git -C ../PS5_PayloadSDK fetch && git -C ../PS5_PayloadSDK log --oneline -1 origin/HEAD
```

Moving the PCSX2 pin: change `revision=` in `fetch-pcsx2.sh`, fetch the new revision into a fresh
checkout, apply `ps5.patch` and resolve what no longer applies, then `--export`
([`patches.md`](patches.md)). Moving Mesa main means regenerating `0012` (a three-way merge; the
commit message of a95b1bc describes the last one).

## Profile-guided builds

A build can be compiled with what the emulator does on the console. Three steps, the middle one on
the console:

1. `PS5PX_PGO=generate ps5/tools/build.sh` makes a profiling build (`build/ps5-pgo-generate`). LLVM's
   profile runtime is not in the SDK: `ps5/tools/build-profile-runtime.sh` builds it from
   compiler-rt at the SDK clang's own release (the script reads the version; llvmorg-18.1.3 when it was written) with three stand-ins (no host name, no
   `procctl`, `getpagesize` through `sysconf`); its counters are placed with the writable data
   (`link-title.sh`).
2. On the console, play the games the build should be good at. When a game or Crux2 closes, the
   counts go to `<data>/pgo/crux2-<start time>.profraw` (the boot log's `pgo:` lines); Settings >
   Log's copy takes them along. A file holds function names and counts only.
3. `ps5/tools/merge-profiles.sh <files or folders>` merges them into `ps5/pgo/crux2.profdata`
   (`PS5PX_PGO_EQUAL=1` with a folder per game weighs games alike); `PS5PX_PGO=use` compiles with it.

`PS5PX_PGO_SPLIT=1` (cold splitting, cache-directed sort, a second round's counts) is off by
default: measured on the console on 2026-10-12, it played worse than the first round's profile
alone. The committed profile is `f965a89d3a30` (named in every release's build table): the
previous one (`7f52d5c1b676`, many games' runs of 2026-11-06) merged with two runs of 2026-11-15
on PCSX2 0ab00c4, so that games not played on the new sources keep their counts; functions whose
code changed take the new runs' counts only.

## Link-time optimisation

The emulator's core, recompilers and GS are built with PCSX2's `LTO_PCSX2_CORE` (ThinLTO at `-O3`).
Checked on the PC: the same imports as without it, no native thread-local storage (the console's is
emulated), frame pointers kept in functions that call others. On the console it was worth about 3%
in the measured scene.

## The recomp kit

`ps5/tools/make-recomp-kit.sh <version> [out]` packs the recomp kit (`ps5/recomp/`, with Crux2
Recomp's runner files and patches), and `build-recomp-windows.sh` builds PS2Recomp's converter for
Windows for it (needs `g++-mingw-w64-x86-64-posix gcc-mingw-w64-x86-64-posix`). The kit takes Crux2
Recomp's runner files (`ps5/runner/`: `crux2_recomp_config.h`, `tools/vu1_recomp.py`,
`tools/build-runner-module.py`) and patches from a checkout of
[Press5elect/PS5_Crux2Recomp](https://github.com/Press5elect/PS5_Crux2Recomp) beside this one
(`git clone https://github.com/Press5elect/PS5_Crux2Recomp ../PS5_Crux2Recomp`);
`build-recomp-windows.sh` falls back to this repository's `crux2recomp/` for the patches. `make-release-zip.py` makes the kit
for every release (`--no-kit` leaves it out). Using the kit is
described for users in [`../native-modules.md`](../native-modules.md).
