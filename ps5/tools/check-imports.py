#!/usr/bin/env python3
"""List what the linked title imports, by who provides it at run time.

    python3 ps5/tools/check-imports.py [build/ps5/link/llvm-pie.elf]

A title's imports are filled by the console when it loads. Two kinds of provider:

  * libc.prx, the clean-room C runtime shipped in the title's sce_module/. Its export list
    (PS5_Vulkan's tooling/native/runtime/api-surface.txt, by NID) is a statement about what
    runs: a name in it is there.
  * the system's modules (libkernel, libSce*). The SDK's stub libraries for them say that the
    link accepts a name, not that the console implements it for a title: some are missing,
    refused or faulting (the platform layer and frontend/ps5_libc.c replace the known ones).

So the second list is what a first console run has to prove; this prints it, grouped by module,
so it can be read against the calls the title is known to make. Nothing here can be verified
without a console. Exit status 1 when an import has no provider at all (the native tool would
have refused the link).

After PS5_vkQuake's tools/check-runtime-surface.py (the NID derivation is quoted from
PS5_Vulkan's tooling/native/libc_builder.cpp).
"""

from __future__ import annotations

import base64
import hashlib
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
VULKAN = Path(os.environ.get('PS5_VULKAN_DIR', ROOT.parent / 'PS5_Vulkan'))
SURFACE = VULKAN / 'tooling/native/runtime/api-surface.txt'
SDK_LIB = ROOT / '.deps/native/ps5-payload-sdk/target/lib'
NM = ROOT / '.deps/native/ps5-payload-sdk/bin/llvm-nm'
DEFAULT_ELF = ROOT / 'build/ps5/link/llvm-pie.elf'

NID_SUFFIX = bytes([0x51, 0x8D, 0x64, 0xA6, 0x35, 0xDE, 0xD8, 0xC1,
                    0xE6, 0xB0, 0x39, 0xB1, 0xC3, 0xE5, 0x52, 0x30])


def nid(name: str) -> str:
    digest = hashlib.sha1(name.encode() + NID_SUFFIX).digest()
    return base64.b64encode(bytes(reversed(digest[:8]))).decode()[:11].replace('/', '-')


def runtime_exports() -> set[str]:
    exports = set()
    for line in SURFACE.read_text(encoding='utf-8').splitlines():
        line = line.strip()
        if line and not line.startswith('#'):
            exports.add(line.split('|')[0])
    return exports


def defined(library: Path) -> set[str]:
    listing = subprocess.run([str(NM), '-D', '--defined-only', str(library)],
                             capture_output=True, text=True).stdout
    return {m.group(1) for m in re.finditer(r' [TWiDBVR] ([A-Za-z_][A-Za-z0-9_]*)$', listing, re.M)}


def main() -> int:
    elf = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_ELF
    for required in (elf, NM, SURFACE):
        if not required.exists():
            raise SystemExit(f'missing {required}')

    exports = runtime_exports()
    for name, provided in {'malloc': True, 'printf': True, 'fopen': True, 'getcwd': False}.items():
        if (nid(name) in exports) != provided:
            raise SystemExit(f'self-check failed: the NID of {name} disagrees with libc.prx')

    listing = subprocess.run([str(NM), '-D', '--undefined-only', str(elf)],
                             capture_output=True, text=True, check=True).stdout
    imported = sorted({m.group(1) for m in re.finditer(r'\b([A-Za-z_][A-Za-z0-9_]*)$', listing, re.M)})

    modules: dict[str, set[str]] = {}
    for library in sorted(SDK_LIB.glob('*.so')):
        modules[library.stem] = defined(library)
    for stub in sorted((elf.parent / 'stubs').glob('*.so')):
        modules[stub.stem] = defined(stub)

    in_runtime = [name for name in imported if nid(name) in exports]
    by_module: dict[str, list[str]] = {}
    nowhere = []
    for name in imported:
        if nid(name) in exports:
            continue
        # The kernel and the Sce modules first: a libc stub of the same name is not what loads.
        owner = next((module for module in sorted(modules, key=lambda m: (not m.startswith(('libkernel', 'libSce')), m))
                      if name in modules[module]), None)
        if owner is None:
            nowhere.append(name)
        else:
            by_module.setdefault(owner, []).append(name)

    print(f'{elf.name}: {len(imported)} imports')
    print(f'  {len(in_runtime)} from libc.prx (shipped with the title: these are there)')
    for module in sorted(by_module):
        names = by_module[module]
        print(f'  {len(names)} from {module} (link stubs: proven only by a console run)')
        print('      ' + ' '.join(names))
    if nowhere:
        print(f'  {len(nowhere)} with NO provider:')
        print('      ' + ' '.join(nowhere))
    return 1 if nowhere else 0


if __name__ == '__main__':
    raise SystemExit(main())
