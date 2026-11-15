# Crux2 documentation

What exists, and where. The top-level [`README.md`](../README.md) is the user's guide (it is also
copied into the title folder); everything else is indexed here.

## For users

| Document | What it holds |
| --- | --- |
| [`../README.md`](../README.md) | What Crux2 is, installing, where files go, the interface, controls, where games are, settings, saves, texture packs, backups, logs, troubleshooting, credits and the licence |
| [`../CHANGELOG.md`](../CHANGELOG.md) | A short entry for every version, and the Unreleased work (What's new in the title reads it) |
| [`releases/README.md`](releases/README.md) | Every published release's notes |
| [`native-modules.md`](native-modules.md) | Native code modules: making one with the recomp kit (experimental) |
| [`../NOTICE`](../NOTICE), [`../LICENSE`](../LICENSE), [`../ps5/ASSETS.md`](../ps5/ASSETS.md) | The licence, its additional term, and where every non-code asset comes from |

## For people and agents working on Crux2

Start with [`../CLAUDE.md`](../CLAUDE.md): the rules that always apply, the commands, and this index.

| Document | Read it when |
| --- | --- |
| [`dev/workflow.md`](dev/workflow.md) | Before any change: the owner's rules, the loop, lessons from earlier sessions, the tools |
| [`dev/building.md`](dev/building.md) | Setting up the machine, building, RADV, the pins and the source check, PGO |
| [`dev/architecture.md`](dev/architecture.md) | Finding your way: what each folder and file of the port does |
| [`dev/patches.md`](dev/patches.md) | Changing PCSX2 (`ps5.patch`, `--export`), the Mesa series, paraLLEl-GS and PS2Recomp |
| [`dev/testing.md`](dev/testing.md) | The host tests, the interface crawl and shots, CI, the other suites |
| [`dev/settings-inventory.md`](dev/settings-inventory.md) | The settings inventory, the no-loss test (and blessing its baseline), `settings-text.yaml` |
| [`dev/debugging.md`](dev/debugging.md) | Reading a boot log; klog; crashes and hangs; test runs and the profiler |
| [`dev/releasing.md`](dev/releasing.md) | Making a release (only when the owner asks) |
| [`dev/open-work.md`](dev/open-work.md) | What is untested on a console, unfinished, or known broken; decisions on record |
| [`../ps5/probes/move/README.md`](../ps5/probes/move/README.md) | The Move probe: a standalone payload asking whether the console's libSceMove gives a homebrew process Move data |

## History

| Document | What it holds |
| --- | --- |
| [`history/README.md`](history/README.md) | The index of historical material |
| [`history/v1.0.19-v1.0.20-reverted.md`](history/v1.0.19-v1.0.20-reverted.md) | The work reverted by 883c832, commit by commit, and how to restore it |
| [`history/version-details-v1.0.md`](history/version-details-v1.0.md) | The README's detailed per-version sections, v1.0.0 to v1.0.18 |

## Elsewhere in the tree

| Document | What it holds |
| --- | --- |
| [`../ps5/patches/mesa-27/README.md`](../ps5/patches/mesa-27/README.md) | Each patch Crux2's RADV carries (Mesa main, the default) |
| [`../ps5/patches/mesa/README.md`](../ps5/patches/mesa/README.md) | The older series on Mesa 26.2.5 |
| [`../crux2recomp/README.md`](../crux2recomp/README.md) | paraLLEl-GS's and PS2Recomp's sources as Crux2 builds them |
| [`../ps5/art/intro/README.md`](../ps5/art/intro/README.md) | The startup video and how to make it again |
| [`../ps5/tests/computefx/fsr3fi/README.md`](../ps5/tests/computefx/fsr3fi/README.md) | FSR 3's interpolation entry points (tests only) |
| `ps5/frontend/third_party/*/README.md`, `ps5/package/resources/shaders/slang/README.md` | The vendored FidelityFX, NIS and MihawkGen sources; the bundled slang shaders |
| `ps5/notices/components.json` | Every third-party part, its licence and revision (staged into the title folder's `licenses/`) |
| `vendor/pcsx2/CLAUDE.md`, `vendor/pcsx2/AGENTS.md` | PCSX2's own rules for contributions to PCSX2. They do not apply to Crux2 work, and nothing is contributed upstream |
