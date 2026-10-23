#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - pack a built title folder, and the source that goes with it.
#
#   ps5/tools/make-release-zip.py <tag> [--date YYYY-MM-DD] [--out DIR] [--part-mib N]
#
# Writes into DIR (default dist/release-<tag>):
#
#   Crux2-<tag>.zip            dist/<TITLE_ID>/ as one archive: directory entries included,
#                                  deflate, every entry's Unix mode 777, every timestamp set to the date, so
#                                  the same folder always gives the same file. It is tested,
#                                  unpacked into a scratch folder, and the release check
#                                  (stage-notices.py --check) is run on that copy.
#   Crux2-<tag>-partNofM.zip   with --part-mib: the same folder in archives no larger than
#                                  that, for a channel with a size limit. Each unpacks into the
#                                  same <TITLE_ID>/ folder; all of them together are the title.
#   Crux2-<tag>-source.zip     this project: the port layer, the host, the patches to PCSX2
#                                  and Mesa, the tools and the tests (what is not fetched)
#   pcsx2-<revision>.tar.xz        PCSX2 at the pinned revision, as fetched (the patch is in the
#                                  project archive)
#   SOURCES.txt                    every other part: its repository and the exact revision
#   SHA256SUMS                     of everything above
#
# The shape is the release procedure of Mihawk's PS5 titles (one ZIP of the title folder, the
# source beside it, the check run on the unpacked copy). The parts whose source is not copied
# here are public repositories named by revision in SOURCES.txt; a public release should attach
# an archive of each of them as well, so that the source stays available with the binary.
import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

PS5 = Path(__file__).resolve().parent.parent
ROOT = PS5.parent
SOURCE_SKIP = {"build", "dist", "klog", ".deps", "vendor", ".git", "__pycache__"}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for block in iter(lambda: file.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def is_profile_run(relative: Path) -> bool:
    """The counts of single runs on the console (ps5/pgo/runs/, cs-runs/, ...): the merged profile
    the build uses, ps5/pgo/*.profdata, is what goes into the source; the runs it was merged from
    are kept out (their folders are named by the games played)."""
    parts = relative.parts
    return len(parts) >= 3 and parts[:2] == ("ps5", "pgo") and parts[2] in ("runs", "cs-runs", "menu-only")


def add(archive: zipfile.ZipFile, path: Path, name: str, stamp: tuple, open_to_all: bool = False) -> None:
    """One entry with the fixed timestamp and a Unix mode: the file's own (755 or 644), or, for
    the title folder (open_to_all), 777 for everything. The console starts a title only when its
    files are open to every user, and an FTP client that keeps the archive's modes then uploads
    them so (the convention of BlackBearReloaded's PS5 app boilerplate)."""
    if path.is_dir():
        info = zipfile.ZipInfo(name.rstrip("/") + "/", date_time=stamp)
        info.external_attr = ((0o40777 if open_to_all else 0o40755) << 16) | 0x10
        info.create_system = 3
        archive.writestr(info, b"")
        return
    info = zipfile.ZipInfo(name, date_time=stamp)
    info.external_attr = (0o100000 | (0o777 if open_to_all else 0o755 if path.stat().st_mode & 0o100 else 0o644)) << 16
    info.create_system = 3
    info.compress_type = zipfile.ZIP_DEFLATED
    with path.open("rb") as source, archive.open(info, "w", force_zip64=False) as sink:
        shutil.copyfileobj(source, sink, 1 << 20)


def entries(folder: Path) -> list:
    """The folder's directories and files, parents first, in one fixed order."""
    return sorted(folder.rglob("*"), key=lambda p: p.relative_to(folder).as_posix())


def pack(folder: Path, top: str, chosen, out: Path, stamp: tuple, open_to_all: bool = False) -> None:
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        add(archive, folder, top, stamp, open_to_all)
        written = set()
        for path in chosen:
            relative = path.relative_to(folder)
            # a part carries the directories its files are in
            for depth in range(1, len(relative.parts)):
                parent = Path(*relative.parts[:depth])
                if parent not in written:
                    add(archive, folder / parent, f"{top}/{parent.as_posix()}", stamp, open_to_all)
                    written.add(parent)
            if path.is_dir():
                if relative not in written:
                    add(archive, path, f"{top}/{relative.as_posix()}", stamp, open_to_all)
                    written.add(relative)
            else:
                add(archive, path, f"{top}/{relative.as_posix()}", stamp, open_to_all)
    with zipfile.ZipFile(out) as archive:
        bad = archive.testzip()
        if bad:
            raise SystemExit(f"{out.name}: damaged entry {bad}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Pack the title folder and its source.")
    parser.add_argument("tag")
    parser.add_argument("--date", default=None, help="the timestamp of every entry (default: today)")
    parser.add_argument("--out", type=Path)
    parser.add_argument("--part-mib", type=float, default=0.0, help="also write parts no larger than this")
    args = parser.parse_args()

    title_id = json.loads((PS5 / "sce_sys/param.json").read_text())["titleId"]
    app = ROOT / "dist" / title_id
    if not (app / "eboot.bin").is_file():
        print(f"no {app}/eboot.bin: run ps5/tools/build.sh first", file=sys.stderr)
        return 2
    if subprocess.run([sys.executable, str(PS5 / "tools/stage-notices.py"), "--check", str(app)]).returncode != 0:
        return 1

    date = args.date or subprocess.run(["date", "-u", "+%Y-%m-%d"], capture_output=True, text=True).stdout.strip()
    year, month, day = (int(part) for part in date.split("-"))
    stamp = (year, month, day, 0, 0, 0)
    out = args.out or (ROOT / "dist" / f"release-{args.tag}")
    out.mkdir(parents=True, exist_ok=True)
    produced = []

    # ---- the title folder, whole
    whole = out / f"Crux2-{args.tag}.zip"
    pack(app, title_id, entries(app), whole, stamp, open_to_all=True)
    with tempfile.TemporaryDirectory() as scratch:
        with zipfile.ZipFile(whole) as archive:
            archive.extractall(scratch)
        if subprocess.run([sys.executable, str(PS5 / "tools/stage-notices.py"), "--check",
                           str(Path(scratch) / title_id)]).returncode != 0:
            print("the unpacked ZIP fails the release check", file=sys.stderr)
            return 1
    produced.append(whole)

    # ---- the same folder in parts
    if args.part_mib > 0:
        limit = int(args.part_mib * 1024 * 1024)
        files = [path for path in entries(app) if path.is_file()]
        # how large each file is once compressed, to fill the parts
        sizes = {}
        with zipfile.ZipFile(whole) as archive:
            for info in archive.infolist():
                sizes[info.filename] = info.compress_size + len(info.filename) * 2 + 120
        parts, current, used = [], [], 4096
        for path in files:
            size = sizes[f"{title_id}/{path.relative_to(app).as_posix()}"]
            if size > limit:
                print(f"{path.name} alone is larger than a part", file=sys.stderr)
                return 1
            if current and used + size > limit:
                parts.append(current)
                current, used = [], 4096
            current.append(path)
            used += size
        if current:
            parts.append(current)
        empty_folders = [path for path in entries(app) if path.is_dir() and not any(path.iterdir())]
        for index, chosen in enumerate(parts, 1):
            part = out / f"Crux2-{args.tag}-part{index}of{len(parts)}.zip"
            pack(app, title_id, chosen + (empty_folders if index == len(parts) else []), part, stamp, open_to_all=True)
            if part.stat().st_size > limit:
                print(f"{part.name} came out larger than {args.part_mib} MiB", file=sys.stderr)
                return 1
            produced.append(part)
        # the parts together are the folder: unpack them over each other and compare
        with tempfile.TemporaryDirectory() as scratch:
            for part in produced[1:]:
                with zipfile.ZipFile(part) as archive:
                    archive.extractall(scratch)
            copy = Path(scratch) / title_id
            ours = {p.relative_to(app).as_posix(): sha256(p) for p in entries(app) if p.is_file()}
            theirs = {p.relative_to(copy).as_posix(): sha256(p) for p in entries(copy) if p.is_file()}
            if ours != theirs:
                print("the parts do not add up to the title folder", file=sys.stderr)
                return 1

    # ---- the source
    source = out / f"Crux2-{args.tag}-source.zip"
    chosen = [path for path in entries(ROOT)
              if not (set(path.relative_to(ROOT).parts) & SOURCE_SKIP) and path.suffix != ".pyc"
              and not is_profile_run(path.relative_to(ROOT))]
    pack(ROOT, f"Crux2-{args.tag}", chosen, source, stamp)
    produced.append(source)

    pcsx2 = ROOT / "vendor/pcsx2"
    revision = subprocess.run(["git", "-C", str(pcsx2), "rev-parse", "HEAD"], check=True, capture_output=True,
                              text=True).stdout.strip()
    upstream = out / f"pcsx2-{revision[:12]}.tar.xz"
    with upstream.open("wb") as sink:
        tar = subprocess.Popen(["git", "-C", str(pcsx2), "archive", "--format=tar", f"--prefix=pcsx2-{revision[:12]}/",
                                "HEAD"], stdout=subprocess.PIPE)
        subprocess.run(["xz", "-T0", "-6"], stdin=tar.stdout, stdout=sink, check=True)
        if tar.wait() != 0:
            return 1
    produced.append(upstream)

    components = json.loads((app / "licenses/components.json").read_text())
    lines = [f"Crux2 {args.tag}: where the source of every part is", "",
             f"Build identity: {components['build_identity']}", "",
             f"In this folder: Crux2-{args.tag}-source.zip (the port, its patches, tools and tests) and",
             f"pcsx2-{revision[:12]}.tar.xz (PCSX2 at the pinned revision; ps5/patches/pcsx2/ps5.patch of the",
             "project archive is applied to it by ps5/tools/fetch-pcsx2.sh).", "",
             "Every other part is fetched by the build (ps5/tools/bootstrap.sh, build-deps.sh) from a public",
             "repository at the revision below:", ""]
    grouped = {}
    for component in components["components"]:
        if component["id"] not in ("pcsx2", "port"):
            grouped.setdefault((component["source"]["url"], component["source"]["revision"]), []).append(component["name"])
    for (url, revision_named), names in grouped.items():
        lines += ["; ".join(names), f"    {url}", f"    revision: {revision_named}", ""]
    sources = out / "SOURCES.txt"
    sources.write_text("\n".join(lines))
    produced.append(sources)

    (out / "SHA256SUMS").write_text("".join(f"{sha256(path)}  {path.name}\n" for path in produced))
    for path in produced:
        print(f"{path.stat().st_size / 1048576:8.1f} MiB  {path.name}")
    print(f"sha256 of {whole.name}: {sha256(whole)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
