#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 Recomp - build a game's module from PS2Recomp's output (its normal mode) for the console.
#
#   build-runner-module.py --generated out/ --out SLUS_213.76.crux2recomp --sdk SDK --runtime RUNTIME [--jobs N]
#
# SDK is the console's headers (SDK/target/include, with libc++'s in SDK/target/include/c++/v1),
# RUNTIME PS2Recomp's tree at Crux2 Recomp's revision with its runner.patch applied (ps2xRuntime/
# and ps2xIOP/), and --config Crux2 Recomp's crux2_recomp_config.h. The generated code is compiled for the console with the
# same switches as the title's own copy of the runtime, and linked into one shared object whose
# imports are the title's functions (Crux2 Recomp's loader binds them by name).
#
# The module is the game's code, made from your own copy of the game: keep it to yourself.
import argparse
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

MODULE_ABI = 2  # crux2_recomp_config.h's CRUX2_RECOMP_MODULE_ABI


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--generated", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--sdk", required=True, type=Path)
    parser.add_argument("--runtime", required=True, type=Path)
    parser.add_argument("--config", type=Path, help="crux2_recomp_config.h (default: RUNTIME/crux2_recomp_config.h)")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "clang++"))
    parser.add_argument("--lld", default=os.environ.get("LLD", "ld.lld"))
    parser.add_argument("--opt", default="-O2")
    args = parser.parse_args()

    gen, sdk, rt = args.generated.resolve(), args.sdk.resolve(), args.runtime.resolve()
    if not (sdk / "target/include/c++/v1").is_dir():
        raise SystemExit(f"{sdk}: not the console's headers (no target/include/c++/v1)")
    config = (args.config or rt / "crux2_recomp_config.h").resolve()
    if not config.is_file():
        raise SystemExit(f"{config}: not found (crux2_recomp_config.h)")
    if not (rt / "ps2xRuntime/include/ps2_runtime.h").is_file():
        raise SystemExit(f"{rt}: not PS2Recomp's tree (no ps2xRuntime/include/ps2_runtime.h)")

    stub = gen / "crux2_recomp_module.cpp"
    text = (f"// Written by Crux2 Recomp's build-runner-module.py.\n"
            f"extern \"C\" __attribute__((visibility(\"default\"))) const int g_crux2RecompModuleAbi = {MODULE_ABI};\n")
    if not stub.exists() or stub.read_text() != text:
        stub.write_text(text)
    sources = sorted(gen.glob("*.cpp"))

    flags = [args.cxx, "-target", "x86_64-sie-ps5", "-isysroot", str(sdk), "--sysroot", str(sdk),
             "-stdlib++-isystem", str(sdk / "target/include/c++/v1"),
             "-fno-stack-protector", "-fno-plt", "-femulated-tls", "-fdenormal-fp-math=ieee", "-frtti", "-fexceptions",
             "-fvisibility-nodllstorageclass=default", "-fPIC", "-march=znver2", "-std=c++20", args.opt,
             "-ffunction-sections", "-w", "-include", str(config),
             "-I", str(rt / "ps2xRuntime/include"), "-I", str(rt / "ps2xRuntime/src/lib/Kernel"),
             "-I", str(rt / "ps2xIOP/include"), "-I", str(gen)]
    objects = gen / "obj-runner"
    objects.mkdir(exist_ok=True)
    headers = max([config.stat().st_mtime] + [p.stat().st_mtime for p in (rt / "ps2xRuntime/include").rglob("*.h")])

    def compile_one(src: Path):
        obj = objects / (src.stem + ".o")
        if obj.exists() and obj.stat().st_mtime > max(src.stat().st_mtime, headers):
            return None
        result = subprocess.run(flags + ["-c", str(src), "-o", str(obj)], capture_output=True, text=True)
        return None if result.returncode == 0 else f"{src.name}:\n{result.stderr[-4000:]}"

    failures = []
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for i, failure in enumerate(pool.map(compile_one, sources)):
            if failure:
                failures.append(failure)
            if (i + 1) % 200 == 0:
                print(f"build-runner-module: compiled {i + 1} of {len(sources)}", flush=True)
    if failures:
        sys.stderr.write("\n".join(failures[:5]) + f"\nbuild-runner-module: {len(failures)} file(s) did not compile\n")
        return 1

    link = [args.lld, "-shared", "-Bsymbolic", "--hash-style=sysv", "-z", "now", "--gc-sections", "--eh-frame-hdr",
            "-soname", args.out.name, "-o", str(args.out)] + [str(objects / (s.stem + ".o")) for s in sources]
    result = subprocess.run(link, capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(result.stderr[-4000:])
        return 1
    print(f"build-runner-module: {args.out} ({args.out.stat().st_size // 1024} KiB, {len(sources)} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
