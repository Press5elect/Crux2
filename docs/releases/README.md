# Crux2 releases

The notes of each release as published on GitHub (`Press5elect/Crux2`), newest first. The short
entry of every version, published or not, is in [`../../CHANGELOG.md`](../../CHANGELOG.md), which
stays at the repository's root because the build copies it into the title folder for What's new.

| Release | Published | Notes | Main points |
| --- | --- | --- | --- |
| v1.0.19 | pending | [`v1.0.19.md`](v1.0.19.md) | HDR10; network shares set up on the console; light gun (GunCon 2 setup, steady aim, PlayStation Move); trophy pop-ups; texture packs over 10 connections; startup video; recomp kit with the release; PCSX2 v2.9.179 |
| v1.0.18 | 2026-11-01 | [`v1.0.18.md`](v1.0.18.md) | Settings > Folders; accurate floating point (FPU, VU0, VU1); MihawkGen frame generation; Show the movement; frame generation fixes at 30 fps and 120 Hz |
| v1.0.17 | 2026-10-31 | [`v1.0.17.md`](v1.0.17.md) | VRR (experimental); graphics driver on a core of its own; less GS-thread work; Mesa main of 2026-10-31; PCSX2 v2.9.150 (includes the unpublished v1.0.16) |
| v1.0.15 | 2026-10-25 | [`v1.0.15.md`](v1.0.15.md) | Every setting's description rewritten; version in the top bar; a lost wake-up fixed |
| v1.0.14 | 2026-10-24 | [`v1.0.14.md`](v1.0.14.md) | Frames queue while a game is behind |
| v1.0.13 | not published | [`v1.0.13.md`](v1.0.13.md) | Notes written and a ZIP built, but no tag or GitHub release; its changes reached users with v1.0.14 |
| v1.0.0 | 2026-10-23 | [`v1.0.0.md`](v1.0.0.md) | The first release (PCSX2 v2.9.138) |

Not published: v1.0.1 to v1.0.12 and v1.0.16 (development versions; their changes are in the next
published release's notes), an earlier v1.0.19 and v1.0.20, whose work was reverted out of the branch
([`../history/v1.0.19-v1.0.20-reverted.md`](../history/v1.0.19-v1.0.20-reverted.md)). The v1.0.18
notes were restored here from the commit that held them (25410ab; removed by the revert, which
went back to the tag's tree); they match the GitHub release's body.

Since v1.0.18 the notes use the short form (Added, Changed, Fixed, Installation, Reporting
problems, Source and licensing, Build versions, SHA-256). A new release adds its page here and a
row in this table ([`../dev/releasing.md`](../dev/releasing.md)).
