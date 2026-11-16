# Working on Crux2

The rules, the loop for a change, and what earlier sessions learned the hard way. The root
[`CLAUDE.md`](../../CLAUDE.md) has the short form; this is the reference.

## The rules

These are the owner's standing rules. They hold for every session and every agent.

1. **Check that every source is current before building.** Crux2 follows nightly sources: PCSX2's
   master, Mesa main (for the 27.0-devel merge in `ps5/patches/mesa-27/0012`), the PS5 Mesa
   revision PS5_Vulkan pins, PS5_Vulkan, the payload SDK fork, paraLLEl-GS with its Granite and
   volk, and PS2Recomp. `ps5/tools/check-sources.sh` compares each pin with its upstream, and
   `build.sh` runs it first and stops when one is behind (`PS5PX_ALLOW_STALE=1` builds anyway).
   A source the owner chose to keep is recorded as held (`ps5/patches/mesa-27/HOLD`: the revision,
   then why) and reported as such. When a source is behind, **say so and move the pin** (and re-export the patches that sit on it:
   `ps5.patch`, the Mesa series). See [`building.md`](building.md#pins-and-the-source-check).
   (The script and its call in `build.sh` went out with the revert to v1.0.18 in 883c832 and were
   restored after it; PS5_Vulkan's pin is `vulkan_revision` in `ps5/tools/bootstrap.sh`.)
2. **Don't contribute to PCSX2: everything is for Crux2.** Nothing is posted to PCSX2's GitHub
   (no pull requests, issues or comments). `vendor/pcsx2/CLAUDE.md` and `vendor/pcsx2/AGENTS.md`
   are PCSX2's rules for upstream contributions; they do not govern work on Crux2.
3. **Work on the assigned branch; commit and push only there.** No pull requests, tags or GitHub
   releases unless the owner asks. **Never publish a release without being asked.**
4. **No game titles** in commit messages, release notes, changelog entries or any published text:
   serials and IDs only (`SLUS-12345`). **No model identifiers** in anything pushed (commits, notes,
   comments, docs). Chat with the owner may name games. Exception (the owner's decision of
   2026-11-15): a setting's description taken word for word from PCSX2 keeps the games it names, in
   the code, `settings-text.yaml` and the inventory. (Open question for the owner: the
   attribution trailers sessions are told to add, and that past commits carry, name a model.)
5. **PCSX2 is a pinned revision plus one patch.** `vendor/pcsx2` is gitignored: it is PCSX2 at the
   revision in `ps5/tools/fetch-pcsx2.sh`, with `ps5/patches/pcsx2/ps5.patch` applied. Edit the
   files in `vendor/pcsx2`, then `ps5/tools/fetch-pcsx2.sh --export` writes the patch again, and
   check that it applies to a fresh checkout at the pin ([`patches.md`](patches.md#checking-the-patch)).
   A change to `vendor/pcsx2` that is not exported is lost.
6. **Release builds** are `PS5PX_RELEASE=1` from a clean tree, then `stage-notices.py --check`,
   the ZIP and `SHA256SUMS`, and release notes in `docs/releases/` in the style of the last ones
   ([`releasing.md`](releasing.md)).
7. **Behaviour-changing features that cannot be verified without a console go behind a setting
   that is off by default.** Say plainly what was and was not tested on a console, in the commit,
   the changelog and the notes.
8. **The interface's top bar holds places, not tools** (the owner's decision, 2026-11-16): Home,
   Library, and Disc while a disc is in. Settings is the Options button, everywhere, so it is no
   tab. Storage (console, USB, the share) is a filter in the library, never a tab; tools such as
   the texture pack manager are reached from Settings, a game's page and the library, never from
   the top bar. Keep like things with like, and don't give one thing two doors that look alike.

## The loop for a change

1. **Evidence first.** Read the boot log or klog the owner sent ([`debugging.md`](debugging.md)).
   Find the cause before changing code; when a premise is someone else's (a sub-agent's, an old
   note's), check it against the code.
2. **Sources.** Before a build, run the source check (rule 1).
3. **Change the smallest thing** the evidence supports: port code in `ps5/`, PCSX2 code in
   `vendor/pcsx2` (then `--export`), driver code as a patch in `ps5/patches/mesa-27`.
4. **Check on the PC**: compile what changed, run the host tests and, for an interface change, the
   interface crawl ([`testing.md`](testing.md)). New logic that has no console in it gets a host
   test of its own.
5. **Build** (`ps5/tools/build.sh`; a developer build unless it is a release).
6. **Commit** a coherent state on the assigned branch, with a message that says what changed for
   whom, why, how it was checked, and what has not run on a console. End it with the attribution
   lines the session gives.
7. **Tell the owner** what the build changes, what only a console run can show, and what to send
   back (the boot log after Save the log, or a test run's klog).
8. Update the docs that the change makes wrong: [`CHANGELOG.md`](../../CHANGELOG.md) (Unreleased),
   the user guide ([`README.md`](../../README.md)) when a setting or behaviour changed,
   [`open-work.md`](open-work.md) for anything untested or unfinished.

## Lessons from earlier sessions

- **Verify sub-agents' work yourself** (grep the change, compile it, run its test). Workers have
  reported "done" for work they had not finished, and claimed premises that were wrong: a
  "missing" integer square-root shortcut in the EE soft-float kernel that already existed (d31c195's
  host test showed it), and an RSQRT.S negative-zero change that a console capture refuted.
- **Parallel workers on one file**: edit in small hunks, never rewrite or reformat a whole file,
  and re-read the part you change just before you change it.
- **The stop hook asks to commit**: commit only a coherent state (it builds, its tests pass, the
  patch is exported). Otherwise say why not and leave it.
- **Run tests with work folders outside the repository.** The interface tests take the work folder
  as their first argument: `run-bigpicture-tests.sh crawl` once made a `crawl/` folder in the
  repository's root. Use `run-bigpicture-tests.sh /tmp/bp crawl`.
- **RADV is not rebuilt by `build.sh`.** After a change to a Mesa patch, run
  `ps5/tools/build-radv-crux2.sh`; otherwise the title links the old archive (HDR10 needs 0020 in
  it).
- **Missing host libraries are environment-only.** When syntax-checking console code on a PC,
  errors for libsmb2, zstd, rapidyaml (ryml) and similar headers mean the PC lacks them, not that
  the change is wrong; check the code you changed and move on.
- **The changelog is read by the title.** What's new parses `CHANGELOG.md`; keep its heading and
  bullet format (see the note at its top).
- **A revert keeps history.** The v1.0.19/v1.0.20 work was reverted, not deleted: before
  re-implementing something, look in [`../history/`](../history/README.md) and `git log` for it.

## Tools

| Tool | Use |
| --- | --- |
| `ps5/tools/bootstrap.sh [--check]` | The sibling checkouts (PS5_Vulkan, PS5_Mesa, PS5_PayloadSDK) checked and set up |
| `ps5/tools/fetch-pcsx2.sh [--export]` | PCSX2 at the pin with `ps5.patch` applied; `--export` writes `vendor/pcsx2`'s changes back to the patch |
| `ps5/tools/check-sources.sh` | Every pin against its upstream (see rule 1) |
| `ps5/tools/build.sh [--compile]` | The title into `dist/PPSA99249/` (`PS5PX_VERSION`, `PS5PX_RELEASE`, `PS5PX_PGO`) |
| `ps5/tools/build-radv-crux2.sh` | The RADV archive with `ps5/patches/mesa-27` (after any Mesa patch change) |
| `ps5/tools/build-parallel-gs.sh` | The paraLLEl-GS renderer from `crux2recomp/` (build.sh calls it) |
| `ps5/tools/check-imports.py` | What the title imports, by provider |
| `ps5/tools/game-fixes-index.py [--check]` | `ps5/game-fixes.txt`, the list of game fixes the title fetches from `main` |
| `ps5/tools/stage-notices.py --check dist/PPSA99249` | The release check: notices complete, nothing forbidden |
| `ps5/tools/make-release-zip.py vX.Y.Z` | The ZIP, the source archives, `SOURCES.txt`, `SHA256SUMS` |
| `ps5/tools/deploy.sh`, `ps5/tools/run.sh [game]` | Upload what changed over FTP; a test run on the console with klog ([`debugging.md`](debugging.md#test-runs)) |
| `ps5/tools/merge-profiles.sh` | PGO profiles merged ([`building.md`](building.md#profile-guided-builds)) |
| `ps5/tests/run-host-tests.sh` | The host tests (CI runs them) |
| `ps5/tests/bigpicture/run-bigpicture-tests.sh <dir> [crawl\|shots\|inventory\|all]` | The interface on the PC; `inventory`: no setting lost |
| `ps5/tools/settings-inventory.py check\|bless\|show`, `ps5/tools/settings-text.py make\|check\|bake\|review\|apply` | The settings inventory and its baseline; `settings-text.yaml` ([`settings-inventory.md`](settings-inventory.md)) |
