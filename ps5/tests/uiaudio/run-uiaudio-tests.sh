#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - the interface's sound on a PC, with the address and undefined-behaviour sanitizers.
#
#   ps5/tests/uiaudio/run-uiaudio-tests.sh <work folder> [a start-up sound of one's own.wav]
#
# The work folder gets what was heard as WAV files (startup-effects-alone.wav, sequence.wav,
# cues-*.wav, playlist-crossfade.wav, game-mix-44k.wav, and startup-with-own-sound.wav when a sound
# is given), to listen to. Its resources/ is the package's (the interface's sounds and the kit's
# music are read from there, as on the console).
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
front=$(cd "$here/../../frontend" && pwd)
work=${1:?usage: run-uiaudio-tests.sh <work folder> [startup.wav]}
mkdir -p "$work"
ln -sfn "$front/../package/resources" "$work/resources"
if command -v ffmpeg > /dev/null; then
    for ext in mp3 ogg flac; do
        ffmpeg -loglevel error -y -f lavfi -i "sine=frequency=330:duration=2" -ac 2 "$work/t-good.$ext" || rm -f "$work/t-good.$ext"
    done
fi
flags=(-std=c++20 -g -O1 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -I "$front")
for source in "$here/uiaudio_test.cpp" "$front/ps5_ui_audio.cpp" "$front/ps5_ui_audio_dsp.cpp" "$front/ps5_ui_audio_decoders.cpp"; do
    object="$work/$(basename "$source").o"
    if [[ ! -f $object || $source -nt $object || $front/ps5_ui_audio.h -nt $object || $front/ps5_ui_audio_dsp.h -nt $object ]]; then
        clang++ "${flags[@]}" -c "$source" -o "$object" &
    fi
done
wait
clang++ "${flags[@]}" "$work"/*.o -lpthread -o "$work/uiaudio_test"
UIAUDIO_BUILTIN="$front/../package/resources/sounds/silicon-boot.wav" ASAN_OPTIONS=detect_leaks=0 "$work/uiaudio_test" "$work" ${2:+"$2"}
