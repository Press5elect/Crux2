#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - make a game's modules on your PC: recomp-game.py, which does the work on Linux and on
# Windows (its --help says how). Kept for the commands and scripts that call this name.
#
#   recomp-game.sh GAME.chd --for recomp      a folder for Crux2 Recomp (the executable, its module, the .iso)
#   recomp-game.sh SLUS_213.76                a native module for Crux2 (SLUS_213.76.crux2native)
set -euo pipefail
exec python3 "$(cd "$(dirname "$0")" && pwd)/recomp-game.py" "$@"
