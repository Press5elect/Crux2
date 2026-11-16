#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - the no-loss test of the settings (docs/dev/settings-inventory.md).
#
#   ps5/tests/bigpicture/inventory/run-inventory-test.sh <work folder>
#
# Makes the settings inventory from the code (build-inventory.sh) and fails when an option of the
# baseline (ps5/frontend/bigpicture/settings-inventory.json) is gone or changed
# (settings-inventory.py check), or when settings-text.yaml does not cover every row, has a stale
# one, or does not have the code's words (settings-text.py check). A change meant to remove or
# change an option: ps5/tools/settings-inventory.py bless --dump <work folder>/dump.json, and say why.
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=$(cd "$here/../../../.." && pwd)
work=${1:?usage: run-inventory-test.sh <work folder>}
status=0
bash "$here/build-inventory.sh" "$work"
python3 "$root/ps5/tools/settings-inventory.py" check --dump "$work/dump.json" || status=1
python3 "$root/ps5/tools/settings-inventory.py" show --dump "$work/dump.json" > "$work/counts.txt"
# The baked table as a compiler reads it: its words must be the code's.
clang++ -std=c++20 -Wall -Wextra -Werror -I"$root/ps5/frontend/bigpicture" "$here/baked.cpp" -o "$work/baked"
"$work/baked" > "$work/baked.json"
python3 "$root/ps5/tools/settings-text.py" check --dump "$work/dump.json" --baked "$work/baked.json" || status=1
exit $status
