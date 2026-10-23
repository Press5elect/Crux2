#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - no setting read through Host while the settings' lock is held.
#
#   ps5/tests/check-settings-lock.py
#
# Host::GetBase*SettingValue and its kind take the settings' lock (a std::mutex, not recursive): one
# called while the same thread holds it ends the title ("mutex lock failed: Resource deadlock
# avoided", v1.0.1's controllers page). The interface's tests run on a stand-in backend, so they
# cannot meet it; this reads the port's sources instead: in every scope that took the lock
# (`... = Host::GetSettingsLock();`), up to the scope's end, no such call outside a lambda (a
# lambda runs later, without the lock). Calls known to be safe are listed in ALLOWED.
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1] / "frontend"
TAKES_LOCK = re.compile(
    r"Host::(GetBase\w+SettingValue|SetBase\w+SettingValue|Get(Bool|Int|UInt|Float|Double|String|StringList)SettingValue|"
    r"GetBaseStringListSetting|SetBaseStringListSettingValue|AddBaseValueToStringList|RemoveBaseValueFromStringList|"
    r"DeleteBaseSettingValue|GetSettingsLock)\(")
HOLDS = re.compile(r"=\s*Host::GetSettingsLock\(\)")
LAMBDA = re.compile(r"\[[^\]]*\]\s*(\([^)]*\))?\s*(mutable\s*)?\{")
# CommitBaseSettingChanges saves the file; it takes no lock of its own.
ALLOWED = {"CommitBaseSettingChanges"}


def scan(path: Path) -> list:
    problems = []
    depth = 0
    locks = []    # depths at which a scope took the lock
    lambdas = []  # depths at which a lambda's body opened
    for number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").split("\n"), 1):
        code = re.sub(r'"(\\.|[^"\\])*"', '""', line.split("//")[0])
        in_lambda = bool(lambdas) or bool(LAMBDA.search(code))
        if locks and not in_lambda and not HOLDS.search(code):
            for m in TAKES_LOCK.finditer(code):
                if m.group(1) not in ALLOWED:
                    problems.append(f"{path.relative_to(ROOT.parent.parent)}:{number}: Host::{m.group(1)} while the settings' "
                                    f"lock is held (taken at line {locks[-1][1]})")
        if HOLDS.search(code):
            locks.append((depth, number))
        for m in LAMBDA.finditer(code):
            lambdas.append(depth + code[:m.end()].count("{") - code[:m.end()].count("}") - 1)
        depth += code.count("{") - code.count("}")
        while locks and depth < locks[-1][0]:
            locks.pop()
        while lambdas and depth <= lambdas[-1]:
            lambdas.pop()
    return problems


def main() -> int:
    problems = []
    for path in sorted(ROOT.rglob("*.cpp")):
        problems += scan(path)
    for p in problems:
        print(p)
    print(f"check-settings-lock: {len(problems)} problem(s) in {len(list(ROOT.rglob('*.cpp')))} files")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
