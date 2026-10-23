# Assets

Everything in the title folder that is not code, with its origin and licence.

| Asset | Origin | Licence |
|---|---|---|
| `sce_sys/icon0.png`, `pic0.dds`, `pic1.dds` | drawn by `ps5/tools/make-title-art.py` (`--style own`): a disc on a blue tile and the name set in Roboto. No part of them is taken from another work. | GPL-3.0-or-later (the script's output); Roboto: Apache-2.0 |
| `resources/` (fonts, the game database, the interface's pictures and sounds, shaders) | PCSX2's repository, `bin/resources`, at the pinned revision, less the Direct3D and OpenGL shaders | GPL-3.0-or-later, and for the fonts the licences beside them (`resources/fonts/*-license`, `*-copyright`) |
| `resources/cacert.pem` | Mozilla's certificate list, as the certifi package carries it | MPL-2.0 |
| `sce_sys/param.json`, `games/README.txt`, `bios/README.txt`, `LEGAL.txt`, `README.md` | written for this port | GPL-3.0-or-later |

Not shipped: PCSX2's patch collection (`patches.zip`, no stated licence: the title downloads
it from its repository's releases when its user asks for it); PCSX2's own icon and
the wordmark made of its banner, as the title's pictures and name, in a package passed on to
others (`make-title-art.py --style pcsx2` and `make-wordmark.py` draw them, for one's own
console); any cover picture, game, BIOS, firmware, key, save or setting of a console.
