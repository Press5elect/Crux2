# Making a release

A release is the title folder as a ZIP on a GitHub release of `Press5elect/Crux2`, with
`SHA256SUMS` beside it, and its notes in `docs/releases/`. **Only when the owner asks.** No tag,
release or pull request is made without the owner's word ([`workflow.md`](workflow.md#the-rules)).

## Never in a release

Games or game files, BIOS or firmware, keys, saves, settings of a console, the console's address,
captured logs, screenshots of games. Never a game title in the notes, the changelog or a commit
message: serials and IDs only. No model identifiers.

## Steps

1. **Sources.** Run the source check ([`building.md`](building.md#pins-and-the-source-check)); move
   and re-export what is behind, or say why a pin stays.
2. **The version**, in one commit:
   - `ps5/sce_sys/param.json`'s `contentVersion`: `01.0YY.000` for `v1.0.YY` (`01.018.000` for
     v1.0.18; it goes up with every release so the console and the homebrew catalog see updates);
   - [`CHANGELOG.md`](../../CHANGELOG.md): rename `## Unreleased` to `## vX.Y.Z (YYYY-MM-DD)`, keep
     the `- Added:` / `- Changed:` / `- Fixed:` bullets (What's new parses them), and start a new
     empty Unreleased section only when there is new work;
   - [`../../README.md`](../../README.md) where a feature changed what it says;
   - `ps5/notices/components.json` if a third-party part was added or its revision moved.
3. **Tests**: the host tests and the interface crawl pass ([`testing.md`](testing.md)).
4. **The release build, from a clean tree** (a dirty tree makes a developer build, and build.sh
   says so):

   ```bash
   git status --short                      # must be empty
   PS5PX_RELEASE=1 PS5PX_VERSION=vX.Y.Z ps5/tools/build.sh
   ps5/tools/stage-notices.py --check dist/PPSA99249
   ps5/tools/make-release-zip.py vX.Y.Z --date YYYY-MM-DD    # into dist/release-vX.Y.Z/
   ```

   `make-release-zip.py` writes the ZIP (every entry mode 777, timestamps set to the date, so the
   same folder gives the same file; tested and checked unpacked), the source archives,
   `SOURCES.txt`, the recomp kit (`Crux2-recomp-kit-vX.Y.Z.zip`, made by `make-recomp-kit.sh`) and
   `SHA256SUMS` of them all. The GitHub releases since v1.0.17 carry **the ZIP and
   `SHA256SUMS`**, and from the next release on **the recomp kit** beside them (the owner's
   decision of 2026-11-15); the source is the repository at the tag plus the revisions in
   `licenses/components.json`. Note the build identity and build number it prints.
5. **The notes**: `docs/releases/vX.Y.Z.md`, in the form of the last one
   ([`v1.0.18.md`](../releases/v1.0.18.md), the short form used since v1.0.18): the piracy note,
   Added, Changed, Fixed, Installation, Reporting problems, Source and licensing (PCSX2's revision
   linked), Build versions (Crux2 commit and build number, PCSX2, RADV, PS5_Vulkan, PS5_PayloadSDK,
   paraLLEl-GS, MihawkGen) and the ZIP's SHA-256. Say plainly what was tested on a console. Add a
   row to [`../releases/README.md`](../releases/README.md).
6. **Hand over** to the owner: the ZIP, the recomp kit, `SHA256SUMS`, the notes, and the commit to tag. The tag and
   the GitHub release are made when the owner says so.
7. **Read back** once published: fetch the release (`gh api repos/Press5elect/Crux2/releases/tags/vX.Y.Z`),
   compare the asset's SHA-256 with the notes, check the body names no game, and that
   `docs/releases/vX.Y.Z.md` matches it.

## After a release

Test builds go back to developer builds (no `PS5PX_RELEASE`). A version that was built but not
published (v1.0.16, v1.0.19) is said so in the next release's notes.
