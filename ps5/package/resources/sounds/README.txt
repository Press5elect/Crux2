Crux2 - the interface's sounds
==========================================================

silicon-boot.wav
    The start-up sound, Crux2's own (see the project's licence).

glass/*.wav, paper/*.wav  (36 files, 1,771,168 bytes)
    The interface's sound effects (focus, select, back, tab, open, modal_open,
    modal_close, toggle, slider, error, notify, launch, favorite_on,
    favorite_off, saved, welcome, resume; _NN numbers the variations of one).

Origin
    ps5-homebrew-ui by BlackBearReloaded
    https://github.com/blackbearreloaded/ps5-homebrew-ui
    revision 333e4ee07498c552b30770acf6063003fed6833e (2026-10-10)
    files: assets/audio/sfx/glass, assets/audio/sfx/paper

    As that project's THIRD_PARTY_NOTICES.md says: the paper set is the sound
    effects of ProsperoPuzzles and the glass set those of ProsperoEden, both
    by BlackBearReloaded. They were generated for those projects with
    ElevenLabs Sound Effects v2 and prepared with the kit's
    tools/process-sfx.py (trimmed, faded and levelled).

    Copied here unchanged (they are already 48 kHz 16-bit PCM WAV, mono or
    stereo). Only the recordings of the
    seventeen interface cues Crux2 plays are taken; a cue one set does not
    record is played from the other set (glass has no favorite_on/_off,
    paper no saved, welcome or resume), as the kit itself does.

Licence
    Copyright (C) 2026 BlackBearReloaded.
    Distributed under the GNU General Public License, version 3 or (at your
    option) any later version (GPL-3.0-or-later), the licence of
    ps5-homebrew-ui and of Crux2.
