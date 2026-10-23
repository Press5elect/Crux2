#!/usr/bin/env python3
# Crux2 - the keyboard's key names held against PCSX2's USB keyboard device, on the PC.
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0+
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
#
#   python3 -I keymap_check.py <project root>
#
# A key pressed on the console's keyboard reaches a game through PCSX2's "HID Keyboard" in three
# steps: Crux2 reports the key's code (its HID usage, ps5_keyboard_logic.h); the device looks that
# code up by name (usb-hid.cpp's table of QEMU key names, through ConvertHostKeyboardStringToCode)
# to a QEMU key; QEMU's tables turn that key into a PC scancode and the scancode into the HID usage
# the PlayStation 2 is sent (input-keymap-qcode-to-qnum.cpp, hid.cpp). This follows every name
# through all three and checks the game is sent the usage the keyboard reported.
import re
import sys
from pathlib import Path

root = Path(sys.argv[1])
usb = root / 'vendor/pcsx2/pcsx2/USB'
names = dict(re.findall(r'\{(Q_KEY_CODE_\w+), "([^"]+)"\}', (usb / 'usb-hid/usb-hid.cpp').read_text()))
by_name = {}
for qcode, name in names.items():
    by_name.setdefault(name, qcode)  # the device keeps the first of a name (Meta)
qnum = {q: int(v, 16) for q, v in re.findall(r'\{(Q_KEY_CODE_\w+), (0x[0-9a-fA-F]+)\}',
                                             (usb / 'qemu-usb/input-keymap-qcode-to-qnum.cpp').read_text())}
hid = (usb / 'qemu-usb/hid.cpp').read_text()
table = hid[hid.index('hid_usage_keys[0x100] = {'):]
table = table[table.index('{') + 1:table.index('};')]
usage_of = [int(v, 16) for v in re.findall(r'0x[0-9a-fA-F]+', table)]
assert len(usage_of) == 0x100, len(usage_of)

ours = re.findall(r'\{(0x[0-9a-f]+), "([^"]+)"\}', (root / 'ps5/frontend/ps5_keyboard_logic.h').read_text())
checked = failed = unknown = dropped = 0
for usage, name in ours:
    usage = int(usage, 16)
    qcode = by_name.get(name)
    if qcode is None:
        unknown += 1  # a key the device does not know: it is not sent to the game
        continue
    if name == 'Pause':
        continue  # sent as its own three-byte sequence, which hid.cpp turns back into 0x48 itself
    checked += 1
    sent = usage_of[qnum[qcode] & 0xff] if qcode in qnum else 0
    if sent == 0:
        dropped += 1  # QEMU's tables have no scancode for it: the game is sent nothing
    elif sent != usage:
        failed += 1
        print(f'FAILED {name}: reported as 0x{usage:02x}, the game is sent 0x{sent:02x}')
missing = sorted(n for n in by_name if n not in {n for _, n in ours})
print(f'keyboard names: {checked} keys followed to the game, {failed} sent as another key, '
      f'{dropped} that QEMU\'s tables cannot send, {unknown} not known to the device'
      f'{", device names with no key here: " + " ".join(missing) if missing else ""}')
sys.exit(1 if failed else 0)
