#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
"""Crux2 - make a game's modules on your PC (Windows or Linux), from your own copy of the game.

  recomp-game.py GAME.chd --for recomp      a folder for Crux2 Recomp: the executable, its module and
                                            the disc as an .iso (copy the folder to the console)
  recomp-game.py GAME.chd                   a native module for Crux2 (Linux; SLUS_213.76.crux2native)
  recomp-game.py GAME.chd --for both        both
  recomp-game.py SLUS_213.76 ...            the executable itself (the file BOOT2 names in SYSTEM.CNF)
  recomp-game.py GAMES_FOLDER ...           every disc image in the folder, one after another

A game is a disc image (.chd, .iso, .cue with its .bin, .cso), a folder of the disc's files, or the
executable alone. From a disc the executable is read out by itself (SYSTEM.CNF's BOOT2).

Options:
  --for WHAT     crux2, recomp or both (default: crux2 on Linux, recomp on Windows)
  --out DIR      where the results go (default: here)
  --iso WHEN     auto (default): for Crux2 Recomp, a .chd, .cue/.bin or .cso is written out as an .iso
                 into the game's folder (the console reads .iso); yes; no
  --ghidra DIR   Ghidra (installed in DIR) finds the functions: slow, better on retail games. Without
                 it PS2Recomp's analyzer finds them (quick; fine for a try). GHIDRA_PROCESSOR names
                 the language to import with, if you have an Emotion Engine one.
  --work DIR     where everything is made; kept between runs, so a second run only redoes what
                 changed (default: ~/.cache/crux2-recomp, or %LOCALAPPDATA%\\Crux2\\recomp on Windows)
  --jobs N       compilers at once (default: every processor)

It needs Python 3.9 or newer and LLVM 18 or newer (clang, clang++, ld.lld):
  Windows: LLVM's installer from https://github.com/llvm/llvm-project/releases (LLVM-<version>-win64.msi);
           it is found in C:\\Program Files\\LLVM\\bin even when not on the PATH. The kit brings
           PS2Recomp's converter built for Windows.
  Linux:   sudo apt install git cmake ninja-build clang lld python3 (the first run fetches PS2Recomp
           and builds its converter).

The results are the game's code: keep them to yourself, as you would the disc.
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
RECOMP = HERE.parent
sys.path.insert(0, str(HERE))
import ps2disc  # noqa: E402

PS2RECOMP_URL = "https://github.com/ran-j/PS2Recomp.git"
PS2RECOMP_REVISION = "eb8688b2fd191129f103a39543002c005b33409c"

WINDOWS = os.name == "nt"
EXE = ".exe" if WINDOWS else ""
DISC_SUFFIXES = (".chd", ".iso", ".cue", ".cso", ".bin")


class Failure(Exception):
    pass


def say(text=""):
    print(text, flush=True)


def run(command, log=None, cwd=None, env=None):
    """A command; its output to log (a path) or here. Failure shows the end of the log."""
    if log is None:
        result = subprocess.run([str(c) for c in command], cwd=cwd, env=env)
        if result.returncode != 0:
            raise Failure(f"{Path(str(command[0])).name} failed")
        return
    with open(log, "w", encoding="utf-8", errors="replace") as f:
        result = subprocess.run([str(c) for c in command], cwd=cwd, env=env, stdout=f, stderr=subprocess.STDOUT)
    if result.returncode != 0:
        tail = Path(log).read_text(encoding="utf-8", errors="replace").splitlines()[-30:]
        say("\n".join("    " + line for line in tail))
        raise Failure(f"{Path(str(command[0])).name} failed (its log: {log})")


# --------------------------------------------------------------------------------------------------
# The tools


def clang_major(clang: str) -> int:
    try:
        out = subprocess.run([clang, "--version"], capture_output=True, text=True).stdout
    except OSError:
        return 0
    m = re.search(r"clang version (\d+)", out)
    return int(m.group(1)) if m else 0


def find_llvm() -> dict:
    """clang, clang++ and ld.lld, 18 or newer: on the PATH (clang-NN too), or LLVM's own folder."""
    folders = [None]
    if os.environ.get("LLVM_DIR"):
        folders.insert(0, Path(os.environ["LLVM_DIR"]) / "bin")
    if WINDOWS:
        for var in ("ProgramW6432", "ProgramFiles", "ProgramFiles(x86)"):
            if os.environ.get(var):
                folders.append(Path(os.environ[var]) / "LLVM" / "bin")

    def look(folder, name):
        if folder is None:
            return shutil.which(name)
        path = folder / (name + EXE)
        return str(path) if path.is_file() else None

    older = None
    for folder in folders:
        for suffix in [""] + [f"-{n}" for n in range(24, 17, -1)]:
            tools = {"clang": look(folder, "clang" + suffix), "clang++": look(folder, "clang++" + suffix),
                     "ld.lld": look(folder, "ld.lld" + suffix) or look(folder, "ld.lld")}
            if not all(tools.values()):
                continue
            major = clang_major(tools["clang"])
            if major >= 18:
                return tools
            older = older or (tools["clang"], major)
    if older:
        raise Failure(f"clang {older[1]} found ({older[0]}); Crux2 Recomp's modules need clang 18 or newer")
    if WINDOWS:
        raise Failure("LLVM (clang, ld.lld) is not installed: get LLVM-<version>-win64.msi from "
                      "https://github.com/llvm/llvm-project/releases and run it")
    raise Failure("clang 18+, clang++ and ld.lld are not installed: sudo apt install clang lld (or clang-18 lld-18)")


def kit_converter():
    """The converter the kit brings (Windows), with the runtime's sources it was made with."""
    if not WINDOWS:
        return None
    bin_dir = RECOMP / "bin" / "windows-x64"
    tree = RECOMP / "ps2recomp"
    converter, analyzer = bin_dir / "ps2_recomp.exe", bin_dir / "ps2_analyzer.exe"
    if converter.is_file() and analyzer.is_file() and (tree / "ps2xRuntime" / "include" / "ps2_runtime.h").is_file():
        return converter, analyzer, tree
    return None


def build_converter(work: Path, jobs: int, runner: Path, tools: dict):
    """PS2Recomp at the revision Crux2 was tested with, with Crux2's patches, its converter built once."""
    missing = [t for t in ("git", "cmake", "ninja") if not shutil.which(t)]
    if missing:
        hint = ("On Linux Mint or Ubuntu: sudo apt install git cmake ninja-build" if not WINDOWS else
                "Use the recomp kit, which brings the converter built for Windows")
        raise Failure(f"missing {', '.join(missing)} (to build PS2Recomp's converter). {hint}")
    src = work / "PS2Recomp"
    patches = [p for p in (runner / "def-jam-vendetta-recomp.patch",) if p.is_file()]
    patches.append(RECOMP / "patches" / "ps2recomp-crux2.patch")
    if (runner / "runner.patch").is_file():
        patches.append(runner / "runner.patch")
    digest = hashlib.sha256(b"".join(p.read_bytes() for p in patches)).hexdigest()[:12]
    stamp = work / f".converter-{PS2RECOMP_REVISION}-{digest}"
    out = src / "out"
    if not stamp.is_file():
        say(f"==> PS2Recomp {PS2RECOMP_REVISION[:7]}, with Crux2's patches")
        git = ["git", "-c", "core.autocrlf=false", "-C", str(src)]
        if not (src / ".git").is_dir():
            run(["git", "-c", "core.autocrlf=false", "clone", "--quiet", os.environ.get("PS2RECOMP_MIRROR", PS2RECOMP_URL), str(src)])
            run(git + ["remote", "set-url", "origin", PS2RECOMP_URL])
        if subprocess.run(git + ["cat-file", "-e", PS2RECOMP_REVISION + "^{commit}"], capture_output=True).returncode != 0:
            run(git + ["fetch", "--quiet", "origin"])
        run(git + ["checkout", "--quiet", "--force", PS2RECOMP_REVISION])
        run(git + ["clean", "-fdq", "-e", "out"])
        for patch in patches:
            run(git + ["apply", str(patch)])
        say("==> building its converter (a few minutes, once)")
        cc = os.environ.get("CC") or tools["clang"]
        cxx = os.environ.get("CXX") or tools["clang++"]
        run(["cmake", "-S", src, "-B", out, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_C_COMPILER={cc}",
             f"-DCMAKE_CXX_COMPILER={cxx}", "-DPS2X_BUILD_RUNTIME=OFF", "-DPS2X_BUILD_TEST=OFF", "-DPS2X_BUILD_STUDIO=OFF"],
            log=work / "converter-configure.log")
        run(["cmake", "--build", out, "--target", "ps2_recomp", "ps2_analyzer", "-j", str(jobs)], log=work / "converter-build.log")
        for old in work.glob(".converter-*"):
            old.unlink()
        stamp.touch()
    converter = next((p for p in out.rglob("ps2_recomp" + EXE) if p.is_file()), None)
    analyzer = next((p for p in out.rglob("ps2_analyzer" + EXE) if p.is_file()), None)
    if not converter or not analyzer:
        raise Failure("the converter did not build")
    return converter, analyzer, src


# --------------------------------------------------------------------------------------------------
# One game


def configure(found: Path, toml: Path, elf: Path, output: Path, mode: str):
    """A configuration for one kind of output: the executable from here, the C++ into <output>.new/.
    In Crux2 mode every function is kept (no stubs, nothing skipped: Crux2 runs the rest itself)."""
    text = found.read_text(encoding="utf-8")

    def setting(text, key, value):
        pattern = re.compile(rf'^{key}\s*=\s*"[^"]*"', re.M)
        line = f'{key} = "{value}"'
        return pattern.sub(lambda _: line, text, count=1) if pattern.search(text) else text.replace("[general]", f"[general]\n{line}", 1)

    text = setting(text, "input", str(elf).replace("\\", "/"))
    text = setting(text, "output", str(output).replace("\\", "/") + ".new/")
    if mode == "crux2":
        for key in ("stubs", "skip"):
            text = re.sub(rf"^{key}\s*=\s*\[.*?\]", f"{key} = []", text, flags=re.M | re.S)
    toml.write_text(text, encoding="utf-8")


def convert(converter: Path, toml: Path, game: Path, out: Path, label: str, name: str, crux2: bool):
    """Converts into out, keeping files that came out the same (and so their compiled objects)."""
    say(f"==> converting {name} to C++ ({label})")
    new = Path(str(out) + ".new")
    shutil.rmtree(new, ignore_errors=True)
    env = dict(os.environ)
    if crux2:
        env["PS2RECOMP_CRUX2"] = "1"
    else:
        env.pop("PS2RECOMP_CRUX2", None)
    run([converter, toml], log=Path(str(out) + ".log"), cwd=game, env=env)
    out.mkdir(parents=True, exist_ok=True)
    for f in new.iterdir():
        target = out / f.name
        if not target.is_file() or target.read_bytes() != f.read_bytes():
            shutil.copy2(f, target)
    keep = {"crux2_module_info.cpp", "crux2_recomp_module.cpp", "vu1_compiled.cpp"}
    for f in list(out.glob("*.cpp")) + list(out.glob("*.h")):
        if f.name not in keep and not (new / f.name).exists():
            f.unlink()
    shutil.rmtree(new, ignore_errors=True)
    say(f"    {len(list(out.glob('*.cpp')))} files")


class Game:
    """What one input gives: the executable (from a disc, or as it is) and the disc, if any."""

    def __init__(self, path: Path, work: Path):
        self.input = path
        self.disc = None
        head = path.read_bytes()[:4] if path.is_file() else b""
        if head == b"\x7fELF":
            self.name = path.name
            self.elf = path.resolve()
            return
        self.disc = ps2disc.Disc(path)
        say(f"==> {path.name}: {self.disc.describe()}")
        self.name, data = self.disc.boot()
        say(f"    boots {self.name} ({len(data) // 1024} KiB)")
        folder = work / self.name / "executable"
        folder.mkdir(parents=True, exist_ok=True)
        self.elf = folder / self.name
        if not self.elf.is_file() or self.elf.read_bytes() != data:
            self.elf.write_bytes(data)

    def close(self):
        if self.disc:
            self.disc.close()


def make(game: Game, what: str, tools: dict, converter, analyzer, tree: Path, args) -> list:
    name, elf = game.name, game.elf
    work = args.work / name
    work.mkdir(parents=True, exist_ok=True)
    runner = RECOMP / "runner"
    made = []

    # ---- the functions
    found = work / "functions.toml"
    if args.ghidra:
        ghidra = Path(args.ghidra)
        headless = ghidra / "support" / ("analyzeHeadless.bat" if WINDOWS else "analyzeHeadless")
        if not headless.is_file():
            raise Failure(f"no Ghidra at {ghidra} (looked for {headless})")
        say(f"==> Ghidra: analysing {name} (this can take an hour on a large game)")
        project = work / "ghidra"
        shutil.rmtree(project, ignore_errors=True)
        project.mkdir(parents=True)
        processor = ["-processor", os.environ["GHIDRA_PROCESSOR"]] if os.environ.get("GHIDRA_PROCESSOR") else []
        run([headless, project, "crux2", "-import", elf] + processor +
            ["-scriptPath", tree / "ps2xRecomp" / "tools" / "ghidra", "-postScript", "ExportPS2Functions.java", found,
             work / "functions.csv", "-deleteProject"], log=work / "ghidra.log")
        if not found.is_file():
            raise Failure(f"Ghidra wrote no function list (its log: {work / 'ghidra.log'})")
    else:
        say(f"==> the analyzer: finding {name}'s functions")
        run([analyzer, elf, found], log=work / "analyzer.log")

    env = dict(os.environ, CXX=tools["clang++"], CC=tools["clang"], LLD=tools["ld.lld"])

    if what in ("crux2", "both"):
        configure(found, work / "crux2.toml", elf, work / "generated", "crux2")
        convert(converter, work / "crux2.toml", work, work / "generated", "Crux2 mode", name, True)
        say("==> building the native module for Crux2 (the longest part: every function is compiled for the console's processor)")
        args.out.mkdir(parents=True, exist_ok=True)
        module = args.out / f"{name}.crux2native"
        result = subprocess.run([sys.executable, str(HERE / "build-module.py"), "--elf", str(elf), "--generated", str(work / "generated"),
                                 "--out", str(module), "--jobs", str(args.jobs), "--made-by",
                                 f"PS2Recomp {PS2RECOMP_REVISION[:7]}, Crux2 mode"], env=env)
        if result.returncode != 0:
            raise Failure("the native module did not build")
        made.append(f"{module}  ->  Crux2: /data/crux2/native/{name}.crux2native")

    if what in ("recomp", "both"):
        configure(found, work / "recomp.toml", elf, work / "generated-recomp", "recomp")
        convert(converter, work / "recomp.toml", work, work / "generated-recomp", "PS2Recomp's own mode, for Crux2 Recomp", name, False)
        if (runner / "vu1_recomp.py").is_file():
            say("==> the game's VU1 microprograms to C++ (def-jam-vendetta-recomp's VU1 recompiler)")
            result = subprocess.run([sys.executable, str(runner / "vu1_recomp.py"), str(elf), str(work / "generated-recomp" / "vu1_compiled.cpp")])
            if result.returncode != 0:
                raise Failure("the VU1 recompiler failed")
        say("==> building the module for Crux2 Recomp")
        folder = args.out / name
        folder.mkdir(parents=True, exist_ok=True)
        module = folder / f"{name}.crux2recomp"
        result = subprocess.run([sys.executable, str(runner / "build-runner-module.py"), "--generated", str(work / "generated-recomp"),
                                 "--out", str(module), "--sdk", str(runner / "sdk"), "--runtime", str(tree),
                                 "--config", str(runner / "crux2_recomp_config.h"), "--jobs", str(args.jobs)], env=env)
        if result.returncode != 0:
            raise Failure("the module for Crux2 Recomp did not build")
        if (folder / name).resolve() != elf.resolve():
            shutil.copy2(elf, folder / name)
        disc_note = disc_for_recomp(game, folder, args.iso)
        made.append(f"{folder}{os.sep}  ->  Crux2 Recomp: copy the folder into /data/crux2recomp/ ({disc_note})")
    return made


def disc_for_recomp(game: Game, folder: Path, when: str) -> str:
    """The disc beside the module, as Crux2 Recomp reads it (an .iso, or its files in disc/)."""
    if game.disc is None:
        return "add the game's disc to it: its .iso, or its files in a folder named disc"
    if game.disc.folder is not None:
        return f"add the disc's files to it, in a folder named disc (they are in {game.disc.folder})"
    if game.input.suffix.lower() == ".iso" and when != "yes":
        return f"add {game.input.name} to it"
    if when == "no":
        return "add the disc to it as an .iso"
    iso = folder / (game.input.stem + ".iso")
    expected = game.disc.source.sectors * ps2disc.SECTOR
    if iso.is_file() and iso.stat().st_size == expected:
        say(f"==> {iso.name} is there already")
    else:
        say(f"==> the disc as {iso.name} ({expected / 2**30:.2f} GiB)")
        game.disc.write_iso(iso, ps2disc._progress)
    return f"with {iso.name}"


# --------------------------------------------------------------------------------------------------


def inputs_of(paths) -> list:
    out = []
    for p in paths:
        p = Path(p)
        if p.is_dir() and not any(c.name.upper() == "SYSTEM.CNF" for c in p.iterdir()):
            images = sorted(c for c in p.iterdir() if c.is_file() and c.suffix.lower() in DISC_SUFFIXES)
            cues = {c.stem.lower() for c in images if c.suffix.lower() == ".cue"}
            images = [c for c in images if not (c.suffix.lower() == ".bin" and c.stem.lower() in cues)]
            if not images:
                raise Failure(f"{p}: no disc images in it (.chd, .iso, .cue, .cso), and no SYSTEM.CNF")
            out += images
        elif p.exists():
            out.append(p)
        else:
            raise Failure(f"{p}: not found")
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0], add_help=False)
    parser.add_argument("inputs", nargs="*", type=Path)
    parser.add_argument("--for", dest="what", choices=("crux2", "recomp", "both"), default="recomp" if WINDOWS else "crux2")
    parser.add_argument("--out", type=Path, default=Path.cwd())
    parser.add_argument("--iso", choices=("auto", "yes", "no"), default="auto")
    parser.add_argument("--ghidra")
    default_work = (Path(os.environ.get("LOCALAPPDATA", Path.home())) / "Crux2" / "recomp") if WINDOWS else Path.home() / ".cache" / "crux2-recomp"
    parser.add_argument("--work", type=Path, default=default_work)
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("-h", "--help", action="store_true")
    args = parser.parse_args()
    if args.help or not args.inputs:
        say(__doc__)
        return 0 if args.help else 1
    args.out = args.out.resolve()
    args.work = args.work.resolve()

    try:
        if args.what in ("crux2", "both") and WINDOWS:
            raise Failure("Crux2's native modules are made on Linux for now: --for recomp makes Crux2 Recomp's here")
        games = inputs_of(args.inputs)
        tools = find_llvm()
        args.work.mkdir(parents=True, exist_ok=True)
        say(f"==> work folder: {args.work}")
        say(f"    clang {clang_major(tools['clang'])}: {Path(tools['clang']).parent}")
        runner = RECOMP / "runner"
        if args.what != "crux2":
            for f in ("runner.patch", "def-jam-vendetta-recomp.patch", "crux2_recomp_config.h", "build-runner-module.py", "sdk/target/include/c++/v1"):
                if not (runner / f).exists():
                    raise Failure(f"{runner / f} is missing (Crux2's recomp kit has it)")
        kit = kit_converter()
        if kit:
            converter, analyzer, tree = kit
            say(f"    PS2Recomp's converter: the kit's ({converter.parent})")
        else:
            converter, analyzer, tree = build_converter(args.work, args.jobs, runner, tools)
    except Failure as e:
        say(f"recomp-game: {e}")
        return 1

    results, failures = [], []
    for path in games:
        start = time.time()
        game = None
        try:
            game = Game(path, args.work)
            results += make(game, args.what, tools, converter, analyzer, tree, args)
            say(f"    ({path.name}: {time.time() - start:.0f} s)")
        except (Failure, ps2disc.DiscError, OSError) as e:
            say(f"recomp-game: {path.name}: {e}")
            failures.append(path.name)
        finally:
            if game:
                game.close()

    say()
    if results:
        say("Done:")
        for line in results:
            say(f"  {line}")
        say("Copy them to the console (FTP, or a USB drive). Crux2's log says 'native: <name>.crux2native loaded' when the")
        say("game starts; Crux2 Recomp lists the game by name.")
    if failures:
        say(f"Not made: {', '.join(failures)}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
