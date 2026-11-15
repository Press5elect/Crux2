# The startup video

`intro.ivf` (VP9, 1280 x 720, 60 frames a second, 6.6 s) and `intro.ogg` (Ogg Vorbis, 48 kHz stereo,
6.6 s) play when Crux2 starts, in the start-up animation's place (`ps5/frontend/ps5_intro.cpp`,
`ps5/frontend/bigpicture/bp_splash.cpp`; Settings > Interface > Startup video). The title folder
carries them as `resources/intro/` (`ps5/tools/link-title.sh`); without them Crux2 starts with the
animation, as before.

Crux2's mark is put together from its own bars, in a warm dark like the console icon's:

| s | |
| --- | --- |
| 0.0 - 0.5 | out of black; dust drifts; thin orange guide lines draw out from the middle where the bars will go |
| 0.5 - 0.82 | the first cream bar slides in from the top left along its line and clacks home (a muted A) |
| 0.92 - 1.24 | the second from the bottom right (a muted D); the plane they lie on turns towards the camera |
| 1.5 - 1.96 | the orange bar runs in from the bottom left on a light trail (a rising charge) and latches into the slot between them: a thump, a chord, the dust pushed back |
| 2.06 - 2.56 | a light runs along the bars (bells, left to right) |
| 2.7 - 3.2 | the mark draws back to its size as the X of the name |
| 3.02 - 3.77 | C, R, U and 2 come out from behind it one after another; each lands with a tick and a rising note |
| 4.0 - 4.6 | a light crosses the name; the chord opens out |
| 4.6 - 5.9 | the name rests |
| 5.9 - 6.6 | it fades to black with the sound |

Both are made from the sources in `src/`, by Press5elect, GPL-3.0-or-later like the rest of Crux2:

- `src/logo.py` writes the name's letters (`letters.png`, `letters.json`): C, R, U and 2 in Saira
  ExtraBold, in the colours and the layout of `ps5/tools/make-title-art.py`'s `crux_name`.
- `src/main.js` is the scene and its timeline (`T`) (three.js 0.160.0, MIT: used to render, not
  shipped). It draws the mark itself, exactly as `crux_mark` does (a square of 100: bars 84 long,
  7 and 16 wide, the thin ones cut 2.5 clear of the broad one), with the bars where the timeline puts
  them; the dust, the space, the motion blur (10 sub-frames while things move, a half-frame shutter)
  and a soft bloom.
- `src/render.js` draws every frame in Chromium (Playwright) and hands them to ffmpeg.
- `src/audio.py` synthesizes the sound on the same timeline (NumPy; nothing sampled).

To make them again (Node.js 18+, Python 3 with NumPy and Pillow, ffmpeg with libvpx and libvorbis,
Playwright's Chromium), in a copy of `src/` outside the tree (`CRUX2_ROOT` tells `logo.py` where it is):

```sh
R=$PWD; W=/tmp/crux2-intro                # from the top of the tree
mkdir -p $W && cp ps5/art/intro/src/* $W && cd $W
npm install three@0.160.0 playwright
CRUX2_ROOT=$R python3 logo.py             # letters.png, letters.json
node render.js video.mp4                  # every frame in Chromium (SwiftShader): about 5 minutes
OUT=audio.wav python3 audio.py            # the sound, timed to main.js's timeline (T)
ffmpeg -y -i audio.wav -c:a libvorbis -q:a 4 -map_metadata -1 $R/ps5/art/intro/intro.ogg
for p in 1 2; do ffmpeg -y -i video.mp4 -an -c:v libvpx-vp9 -b:v 2600k -maxrate 4000k -bufsize 5000k \
  -pass $p -passlogfile vp9 -row-mt 1 -tile-columns 2 -g 120 -auto-alt-ref 1 -lag-in-frames 25 \
  -deadline good -cpu-used 1 -pix_fmt yuv420p -f ivf $([ $p = 1 ] && echo /dev/null || echo $R/ps5/art/intro/intro.ivf); done
```

The colours are BT.601 studio range (ffmpeg's default from RGB), as the player turns them back.
`node render.js stills 1.96 3.5 5.0` writes single frames (`s_<t>.png`) to look at. The player reads
any IVF of VP9 in 4:2:0 at any size up to 4096 (it is shown stretched to the screen) and any sound
file the interface's sound reads (WAV, MP3, Ogg Vorbis, FLAC), brought to the start-up sound's level.
