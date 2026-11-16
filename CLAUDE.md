# Crux2

PCSX2 ported to the PS5 as a native homebrew title (`PPSA99249`): PCSX2's core at a pinned revision
plus one patch, a PS5 host and Big Picture interface in `ps5/`, RADV linked into the title.

The reference for everything below is [`docs/dev/workflow.md`](docs/dev/workflow.md); the index of
all documentation is [`docs/README.md`](docs/README.md).

## Rules (the owner's, always)

1. **Before building, check every source is current** (Crux2 follows nightly sources, Mesa
   included): `ps5/tools/check-sources.sh` (build.sh runs it; `PS5PX_ALLOW_STALE=1` builds anyway).
   If one is behind, say so and move the pin, and re-export the patches on it.
2. **Don't contribute to PCSX2.** Everything is for Crux2; nothing is posted to PCSX2's GitHub.
   `vendor/pcsx2/CLAUDE.md` (PCSX2's AGENTS.md) applies only to upstream contributions.
3. **Work on the assigned branch**; commit and push only there. No pull requests, tags or GitHub
   releases unless the owner asks. Never publish a release without being asked.
4. **No game titles** in commit messages, release notes or anything published: serials/IDs only.
   Exception (the owner's decision, 2026-11-15): settings descriptions taken word for word from
   PCSX2 keep the games they name.
   **No model identifiers** in anything pushed.
5. **`vendor/pcsx2` is gitignored**: PCSX2 at the pin (`ps5/tools/fetch-pcsx2.sh`) plus
   `ps5/patches/pcsx2/ps5.patch`. Edit vendor, run `ps5/tools/fetch-pcsx2.sh --export`, and verify
   the patch applies to a fresh checkout at the pin ([`docs/dev/patches.md`](docs/dev/patches.md)).
6. **Release builds**: `PS5PX_RELEASE=1` from a clean tree; `stage-notices.py --check`; ZIP +
   `SHA256SUMS`; notes in `docs/releases/` in the existing style ([`docs/dev/releasing.md`](docs/dev/releasing.md)).
7. **Untestable behaviour changes go behind default-off settings.** Say plainly what was and wasn't
   tested on a console.
8. **The top bar holds places only** (Home, Library, Disc): Settings is the Options button, storage
   is a library filter, tools (texture packs) live in Settings, the game page and the library.

## Commands

```bash
ps5/tools/bootstrap.sh --check                    # sibling checkouts and tools
ps5/tools/fetch-pcsx2.sh                          # vendor/pcsx2 at the pin, patched
ps5/tools/fetch-pcsx2.sh --export                 # vendor/pcsx2's changes -> ps5.patch
ps5/tools/check-sources.sh                        # pins vs upstream (see rule 1)
ps5/tools/build.sh                                # developer build -> dist/PPSA99249/
PS5PX_VERSION=vX.Y.Z PS5PX_RELEASE=1 ps5/tools/build.sh   # release build, clean tree only
ps5/tools/build-radv-crux2.sh                     # RADV with ps5/patches/mesa-27 (after a Mesa patch change)
VFS_TEST_DIR=/tmp/crux2-host ps5/tests/run-host-tests.sh           # host tests (CI runs them)
ps5/tests/bigpicture/run-bigpicture-tests.sh /tmp/crux2-bp crawl   # interface crawl ("shots" for pictures)
ps5/tests/bigpicture/run-bigpicture-tests.sh /tmp/crux2-bp inventory  # no setting lost; bless: ps5/tools/settings-inventory.py bless (docs/dev/settings-inventory.md)
ps5/tools/stage-notices.py --check dist/PPSA99249 # the release check
ps5/tools/make-release-zip.py vX.Y.Z              # ZIP, sources, SHA256SUMS
```

Work folders go **outside the repository** (a bare `run-bigpicture-tests.sh crawl` once left a
`crawl/` folder in the root).

## The loop, in brief

Evidence first (the boot log, klog: [`docs/dev/debugging.md`](docs/dev/debugging.md)) -> check
sources -> smallest change (port code in `ps5/`, PCSX2 in `vendor/pcsx2` then `--export`, driver in
`ps5/patches/mesa-27` then rebuild RADV) -> compile and run the host tests and, for the interface,
the crawl -> build -> commit a coherent state -> tell the owner what only a console can show ->
update `CHANGELOG.md` (Unreleased), the README and `docs/dev/open-work.md`.

## Lessons (details in workflow.md)

- Verify sub-agents' edits yourself (grep, compile, test): workers have reported unfinished work as
  done and built on wrong premises.
- Parallel workers on one file: small hunks, never rewrite whole files.
- The stop hook asks to commit: commit only coherent states.
- Missing libsmb2/zstd/ryml headers when syntax-checking on the PC are the environment's, not the
  change's.
- `CHANGELOG.md` is parsed by the title (What's new): keep its `## vX.Y.Z (date)` and `- Added:` form.

## Documentation

| Where | What |
| --- | --- |
| [`README.md`](README.md) | The user's guide (shipped in the title folder) |
| [`CHANGELOG.md`](CHANGELOG.md) | Every version's short entry, Unreleased first (shipped; What's new) |
| [`docs/README.md`](docs/README.md) | The index of everything else |
| [`docs/dev/workflow.md`](docs/dev/workflow.md) | Rules, loop, lessons, tools |
| [`docs/dev/building.md`](docs/dev/building.md) | Toolchain, sibling checkouts, build.sh, RADV, pins, PGO |
| [`docs/dev/architecture.md`](docs/dev/architecture.md) | What each part of the tree does |
| [`docs/dev/patches.md`](docs/dev/patches.md) | `ps5.patch`, the Mesa series, crux2recomp |
| [`docs/dev/testing.md`](docs/dev/testing.md) | Host tests, interface tests, CI, other suites |
| [`docs/dev/debugging.md`](docs/dev/debugging.md) | Boot log, klog, crashes, test runs |
| [`docs/dev/releasing.md`](docs/dev/releasing.md) | Making a release |
| [`docs/dev/open-work.md`](docs/dev/open-work.md) | Untested on a console, unfinished, known problems |
| [`docs/history/`](docs/history/README.md) | The reverted v1.0.19/v1.0.20 work; old per-version detail |
| [`docs/releases/`](docs/releases/README.md) | Published release notes |
