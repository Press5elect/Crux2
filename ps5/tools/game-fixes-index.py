#!/usr/bin/env python3
# Crux2 - the list of game fixes the title fetches from GitHub at start (frontend/ps5_fixes_fetch.h).
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0+
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
#
#   ps5/tools/game-fixes-index.py          # writes ps5/game-fixes.txt
#   ps5/tools/game-fixes-index.py --check  # fails when it is not current (the PC tests run this)
#
# Every file of ps5/package/resources/game-fixes/gamedb/*.yaml and .../patches/*.pnach, by its path
# below game-fixes/ (the package has the folder as resources/game-fixes/). Commit the list with the
# change to a fix: consoles fetch what main's list names, and refuse a file whose size or SHA-256
# is not the list's. Names of letters, digits, . _ - only (the title refuses others).
import hashlib
import os
import re
import sys

ps5 = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
fixes = os.path.join(ps5, 'package', 'resources', 'game-fixes')
out = os.path.join(ps5, 'game-fixes.txt')
HEADER = '# Crux2 game fixes 1'
SAFE = re.compile(r'^[A-Za-z0-9_.-]+$')
KINDS = (('gamedb', '.yaml'), ('patches', '.pnach'))


def build():
    lines = [HEADER, '# <sha256> <bytes> <path below game-fixes/>: written by ps5/tools/game-fixes-index.py']
    for folder, extension in KINDS:
        root = os.path.join(fixes, folder)
        if not os.path.isdir(root):
            continue
        for name in sorted(os.listdir(root)):
            path = os.path.join(root, name)
            if not os.path.isfile(path) or name == 'README.txt':
                continue
            if not name.endswith(extension) or not SAFE.match(name) or name.startswith('.'):
                sys.exit(f'game-fixes-index: {folder}/{name}: only *{extension} named with letters, digits, . _ - '
                         '(the title refuses others)')
            with open(path, 'rb') as f:
                data = f.read()
            lines.append(f'{hashlib.sha256(data).hexdigest()} {len(data)} {folder}/{name}')
    return '\n'.join(lines) + '\n'


text = build()
if '--check' in sys.argv[1:]:
    have = open(out, encoding='utf-8').read() if os.path.exists(out) else ''
    if have != text:
        sys.exit('game-fixes-index: ps5/game-fixes.txt is not current: run ps5/tools/game-fixes-index.py')
    print('game-fixes-index: current')
else:
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    print(f'game-fixes-index: {out} ({text.count(chr(10)) - 2} file(s))')
