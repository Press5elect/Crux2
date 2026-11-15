# The startup video

`intro.ivf` (VP9, 1280 x 720, 60 frames a second, 7 s) and `intro.ogg` (Ogg Vorbis, 48 kHz stereo)
play when Crux2 starts, in the start-up animation's place (`ps5/frontend/ps5_intro.cpp`,
`ps5/frontend/bigpicture/bp_splash.cpp`; Settings > Interface > Startup video). Crux2's mark glows
behind a sheet of black glass, three blows crack it, the glass bulges and shatters, and the mark and
the name are there. The title folder carries them as `resources/intro/` (`ps5/tools/link-title.sh`);
without them Crux2 starts with the animation, as before.

Both are made from the sources in `src/`, by Press5elect, GPL-3.0-or-later like the rest of Crux2.
They are OGBox's startup video (OGBox by Press5elect: its `ps5/art/intro`) with Crux2's mark and name,
its orange, a shorter timeline (7 s), a lighter dither, and a smaller picture, so the two files stay
under 4 MB:

- `src/logo.py` draws the picture that glows behind the glass (`logo.png`, 1536 x 1024): the mark and
  the name as `ps5/tools/make-title-art.py` draws them for the console's menu.
- `src/main.js` is the scene (three.js 0.160.0, MIT: used to render, not shipped): the glass, its
  fracture, the light, the particles, the timeline (`T`) and the post-processing.
- `src/render.js` draws every frame in Chromium (Playwright) and hands them to ffmpeg.
- `src/audio.py` synthesizes the sound on the same timeline (NumPy).

To make them again (Node.js 18+, Python 3 with NumPy and Pillow, ffmpeg with libvpx and libvorbis,
Playwright's Chromium):

```sh
cd ps5/art/intro/src
npm install three@0.160.0 playwright      # (not committed: node_modules, logo.png, video.mp4)
python3 logo.py                           # logo.png
node render.js video.mp4                  # every frame in Chromium (SwiftShader): about 15 minutes
OUT=audio.wav python3 audio.py            # the sound, timed to main.js's timeline (T)
ffmpeg -y -i audio.wav -c:a libvorbis -q:a 5 -map_metadata -1 ../intro.ogg
for p in 1 2; do ffmpeg -y -i video.mp4 -an -c:v libvpx-vp9 -b:v 3400k -maxrate 5000k -bufsize 6000k \
  -pass $p -passlogfile vp9 -row-mt 1 -tile-columns 2 -g 120 -auto-alt-ref 1 -lag-in-frames 25 \
  -deadline good -cpu-used 1 -pix_fmt yuv420p -f ivf $([ $p = 1 ] && echo /dev/null || echo ../intro.ivf); done
```

The colours are BT.601 studio range (ffmpeg's default from RGB), as the player turns them back.
`node render.js stills 1.7 3.6 5.6` writes single frames (`s_<t>.png`) to look at. The player reads
any IVF of VP9 in 4:2:0 at any size up to 4096 (it is shown stretched to the screen) and any sound
file the interface's sound reads (WAV, MP3, Ogg Vorbis, FLAC), brought to the start-up sound's level.
