#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - the notices a title folder carries, and the check before one leaves the machine.
#
#   ps5/tools/stage-notices.py dist/PPSA99249            write LEGAL.txt and licenses/
#   ps5/tools/stage-notices.py --check dist/PPSA99249    check a title folder (or an unpacked ZIP)
#
# Staging (tools/link-title.sh runs it) writes, from ps5/notices/components.json:
#   LEGAL.txt                 no piracy, the licence, what is left out and why, trademarks
#   NOTICE.txt                Crux2's copyright (Press5elect) and its attribution term
#   licenses/README.txt       the same notice, then every part with its author and licence
#   licenses/components.json  every part: licence, copyright, modifications, the revision and
#                             address of its source, and the files it is in with their sha256
#   licenses/*                the licence texts the parts require
#
# The check is the gate for anything that is passed on. It fails when
#   - a file that must never be in a package is there: a disc image, a BIOS (but OpenPS2BIOS, by its
#     place and its bytes) or firmware file, a
#     memory card or save state, settings or logs of somebody's console, an address file
#     (network.cfg, .env), or PCSX2's patch collection (no stated licence);
#   - an executable file is not covered by a part of components.json;
#   - a notice or a licence text is missing, or a file changed since the notices were written;
#   - a part's source revision is unknown.
#
# The shape is PS5_RetroArch's tools/stage-notices.py and check-notices.py (Mihawk-99), made for
# one emulator instead of a frontend and its cores.
import os
import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path

PS5 = Path(__file__).resolve().parent.parent
ROOT = PS5.parent

# Never in a package. (The names are matched without regard to case.)
FORBIDDEN_SUFFIXES = {
    ".iso", ".chd", ".cso", ".zso", ".gz", ".mdf", ".mds", ".nrg", ".img", ".cue", ".ccd", ".m3u",   # disc images
    ".rom", ".rom0", ".rom1", ".rom2", ".erom", ".nvm", ".mec", ".pup",                              # BIOS, firmware
    ".ps2", ".mcd", ".mcr", ".p2s", ".psu", ".max", ".cbs",                                          # cards, states, saves
    ".rap", ".rif", ".ird", ".key", ".keys",                                                         # keys, licences
    ".log", ".env", ".ini", ".cfg",                                                                  # a console's own files
}
FORBIDDEN_NAMES = {"patches.zip", "network.cfg", "test-run.txt", "usb-off.txt", ".env"}
FORBIDDEN_FOLDERS = {"inis", "memcards", "sstates", "snaps", "logs", "cache", "covers", "cheats", "patches",
                     "gamesettings", "inputprofiles", "textures", "videos", "radv-shader-cache"}
ALLOWED_BIN = {"eboot.bin"}
# The one BIOS a package may carry: OpenPS2BIOS (MIT, no Sony code), at this place and with exactly
# these bytes, so that nothing else can pass for it.
FREE_BIOS = {"resources/free-bios/openps2bios-0.2.bin": "1451bade21b8b162d1092faee567dc4620037cede367792c2e258ec43273c72a"}
EXECUTABLE_SUFFIXES = {".bin", ".prx", ".elf", ".sprx", ".self", ".so"}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for block in iter(lambda: file.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def git_revision(directory: Path) -> str:
    try:
        revision = subprocess.run(["git", "-C", str(directory), "rev-parse", "HEAD"], check=True,
                                  capture_output=True, text=True).stdout.strip()
        dirty = subprocess.run(["git", "-C", str(directory), "status", "--porcelain", "--untracked-files=no"],
                               check=True, capture_output=True, text=True).stdout.strip()
        return revision + (" (with local changes)" if dirty else "")
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def pins() -> dict:
    """The revisions the '@name' sources stand for."""
    def read(path: Path, default="unknown"):
        return path.read_text().strip() if path.is_file() else default

    vulkan = ROOT.parent / "PS5_Vulkan"
    # Crux2's archive (build-radv-crux2.sh) names the published PS5 Mesa revision it was made from.
    provenance = read(vulkan / ".deps/native/radv-crux2/PROVENANCE.txt", "") or read(vulkan / ".deps/native/radv-release/PROVENANCE.txt", "")
    mesa = next((line.split()[3] for line in provenance.splitlines() if line.startswith("Crux2: PS5 Mesa ")), "") or \
        next((line.split(": ", 1)[1] for line in provenance.splitlines() if line.startswith("revision: ")), "unknown")
    try:
        import certifi
        certifi_version = certifi.__version__
    except ImportError:
        certifi_version = "unknown"
    pcsx2 = subprocess.run(["git", "-C", str(ROOT / "vendor/pcsx2"), "rev-parse", "HEAD"], capture_output=True,
                           text=True).stdout.strip() or "unknown"
    return {
        "@pcsx2": pcsx2 + " + ps5/patches/pcsx2/ps5.patch",
        "@sdk": read(ROOT / ".deps/native/ps5-payload-sdk/.ps5-sdk-revision"),
        "@vulkan": git_revision(vulkan),
        "@mesa": mesa + " + ps5/patches/mesa/ (" + ", ".join(sorted(p.stem for p in (ROOT / "ps5/patches/mesa").glob("*.patch"))) +
                 "; 0007 and 0009 from PS5CEMU-HAR, GPL-3.0)",
        "@template": git_revision(ROOT.parent / "PS5_VulkanTemplate"),
        "@boilerplate": git_revision(ROOT.parent / "ps5-native-app-boilerplate"),
        # The build that made the title folder: PS5PX_BUILD_DIR from tools/link-title.sh (build/ps5,
        # build/ps5-pgo-use, ...), build/ps5 when it is not set.
        "@identity": read(Path(os.environ.get("PS5PX_BUILD_DIR", str(ROOT / "build/ps5"))) / "build-identity.txt"),
        "@certifi": "certifi " + certifi_version,
        # paraLLEl-GS and what it is built with: the revisions Crux2 Recomp's fetch scripts pin.
        **recomp_pins(),
    }


def recomp_pins() -> dict:
    """paraLLEl-GS's parts, as PS5_Crux2Recomp's tools/fetch-parallel-gs.sh and fetch-ps2recomp.sh pin them."""
    import re
    recomp = ROOT.parent / "PS5_Crux2Recomp"
    def pin(script: str, name: str) -> str:
        path = recomp / "ps5/tools" / script
        text = path.read_text() if path.is_file() else ""
        found = re.search(rf"^{name}=([0-9a-f]{{40}})", text, re.M)
        return found.group(1) if found else "unknown"
    vendetta = recomp / "ps5/patches/ps2recomp/def-jam-vendetta-recomp.patch"
    head = vendetta.read_text().split("\n", 2)[:2] if vendetta.is_file() else []
    defjam = next((m.group(0) for line in head for m in [re.search(r"[0-9a-f]{40}", line)] if m), "unknown")
    recomp_revision = git_revision(recomp)
    return {
        "@pgs": pin("fetch-parallel-gs.sh", "pgs_revision") + " + crux2recomp/ps5/patches/parallel-gs/ of Crux2's source (from PS5_Crux2Recomp " + recomp_revision + ")",
        "@granite": pin("fetch-parallel-gs.sh", "granite_revision"),
        "@volk": pin("fetch-parallel-gs.sh", "volk_revision"),
        "@defjam": defjam + " + crux2recomp/ps5/patches/ps2recomp/runner.patch of Crux2's source (from PS5_Crux2Recomp " + recomp_revision + ")",
    }


def artifact_record(app: Path, name: str) -> dict:
    path = app / name
    if name.endswith("/"):
        files = sorted(p for p in path.rglob("*") if p.is_file())
        digest = hashlib.sha256("".join(f"{sha256(p)}  {p.relative_to(app).as_posix()}\n" for p in files).encode())
        return {"path": name, "files": len(files), "sha256_of_listing": digest.hexdigest()}
    return {"path": name, "sha256": sha256(path)}


def stage(app: Path) -> int:
    components = json.loads((PS5 / "notices/components.json").read_text())["components"]
    resolved = pins()
    licenses = app / "licenses"
    if licenses.exists():
        shutil.rmtree(licenses)
    licenses.mkdir(parents=True)

    legal = (PS5 / "package/LEGAL.txt").read_text()
    (app / "LEGAL.txt").write_text(legal)
    # The copyright and the attribution term of Crux2's own parts (GPL-3.0 section 7(b)).
    shutil.copyfile(ROOT / "NOTICE", app / "NOTICE.txt")

    lines = [legal.rstrip(), "", "=" * 92, "THE PARTS", "=" * 92, ""]
    records = []
    for component in components:
        texts = []
        for name, source in component["texts"].items():
            source_path = Path(source) if source.startswith("/") else (ROOT / source)
            if not source_path.is_file():
                print(f"{component['id']}: licence text missing: {source}", file=sys.stderr)
                return 1
            shutil.copyfile(source_path, licenses / name)
            texts.append(name)
        revision = component["source"]["revision"]
        revision = resolved.get(revision, revision)
        record = {
            "id": component["id"], "name": component["name"], "licence": component["licence"],
            "copyright": component["copyright"], "modifications": component["modifications"], "texts": texts,
            "source": {"url": component["source"]["url"], "revision": revision},
            "artifacts": [artifact_record(app, name) for name in component["artifacts"]],
        }
        records.append(record)
        lines += [component["name"], f"    licence:   {component['licence']}", f"    copyright: {component['copyright']}",
                  f"    source:    {component['source']['url']}", f"    revision:  {revision}"]
        if component["modifications"] not in ("none", "-"):
            lines.append(f"    changed:   {component['modifications']}")
        if texts:
            lines.append(f"    texts:     {', '.join(texts)}")
        lines.append("")

    lines += ["The licence texts of the libraries taken prebuilt (curl, OpenSSL, libpsl, Zstandard, libjpeg-turbo,",
              "libwebp, bzip2, zlib) are the copyright files a Debian system carries for the same libraries.",
              "PCSX2-ThirdPartyLicenses.html is PCSX2's own list for the libraries it bundles.", ""]
    (licenses / "README.txt").write_text("\n".join(lines))
    (licenses / "components.json").write_text(json.dumps(
        {"title": app.name, "build_identity": resolved["@identity"], "components": records}, indent=2) + "\n")
    print(f"notices: {len(records)} parts, {len(list(licenses.iterdir()))} files in {licenses.relative_to(app.parent)}")
    return 0


def check(app: Path) -> int:
    problems = []
    for path in sorted(app.rglob("*")):
        relative = path.relative_to(app)
        parts = [part.lower() for part in relative.parts]
        if path.is_dir():
            if len(parts) == 1 and parts[0] in FORBIDDEN_FOLDERS:
                problems.append(f"a console's own folder is in the package: {relative}/")
            continue
        name, suffix = parts[-1], path.suffix.lower()
        shipped_text = (parts[0] == "resources" or parts[0] == "licenses") and suffix in {".ini", ".cfg"}
        free_bios = FREE_BIOS.get("/".join(parts)) is not None and sha256(path) == FREE_BIOS["/".join(parts)]
        if name in FORBIDDEN_NAMES or (suffix == ".bin" and name not in ALLOWED_BIN and not free_bios) or \
                (suffix in FORBIDDEN_SUFFIXES and not shipped_text):
            problems.append(f"must never be in a package: {relative}")

    for required in ("LEGAL.txt", "NOTICE.txt", "README.md", "licenses/README.txt", "licenses/components.json", "licenses/GPL-3.0.txt",
                     "eboot.bin", "sce_sys/param.json", "sce_sys/icon0.png", "sce_module/libc.prx"):
        if not (app / required).is_file():
            problems.append(f"missing: {required}")

    manifest = app / "licenses/components.json"
    if manifest.is_file():
        components = json.loads(manifest.read_text())["components"]
        covered_files, covered_folders = set(), []
        for component in components:
            if "unknown" in component["source"]["revision"]:
                problems.append(f"{component['id']}: the revision of its source is unknown")
            if "with local changes" in component["source"]["revision"]:
                problems.append(f"{component['id']}: built from a tree with uncommitted changes")
            for text in component["texts"]:
                if not (app / "licenses" / text).is_file():
                    problems.append(f"{component['id']}: licence text missing: licenses/{text}")
            for artifact in component["artifacts"]:
                path = app / artifact["path"]
                if artifact["path"].endswith("/"):
                    covered_folders.append(artifact["path"])
                    if artifact_record(app, artifact["path"]) != artifact:
                        problems.append(f"{component['id']}: {artifact['path']} changed since the notices were written")
                else:
                    covered_files.add(artifact["path"])
                    if not path.is_file():
                        problems.append(f"{component['id']}: {artifact['path']} is not in the package")
                    elif sha256(path) != artifact["sha256"]:
                        problems.append(f"{component['id']}: {artifact['path']} changed since the notices were written")
        for path in sorted(app.rglob("*")):
            relative = path.relative_to(app).as_posix()
            if path.is_file() and path.suffix.lower() in EXECUTABLE_SUFFIXES and relative not in covered_files and \
                    not any(relative.startswith(folder) for folder in covered_folders):
                problems.append(f"an executable file no part accounts for: {relative}")

    for problem in problems:
        print(f"FAILED: {problem}", file=sys.stderr)
    if not problems:
        print(f"release check: {app} passes ({sum(1 for p in app.rglob('*') if p.is_file())} files)")
    return 1 if problems else 0


def main() -> int:
    arguments = sys.argv[1:]
    checking = bool(arguments) and arguments[0] == "--check"
    if checking:
        arguments = arguments[1:]
    if len(arguments) != 1 or not Path(arguments[0]).is_dir():
        print("usage: stage-notices.py [--check] <title folder>", file=sys.stderr)
        return 2
    app = Path(arguments[0]).resolve()
    return check(app) if checking else stage(app)


if __name__ == "__main__":
    sys.exit(main())
