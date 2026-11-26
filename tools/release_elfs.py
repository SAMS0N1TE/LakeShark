#!/usr/bin/env python3
"""Keep the ELF of every image a LakeShark release ships, on the GitHub release.

A core dump decodes only with the exact ELF of the build that wrote it, and a
release build's ELF exists nowhere else once its build folder is gone. Every
release therefore carries one more asset, `lakeshark-<version>-elf.zip`,
which holds each shipped image's ELF as `<sha12>.elf` plus ELFS.txt (full
sha256, image version, the release files it belongs to). The board reports
that sha (`elf-sha=` in `crash` and in a report), and
`tools/ls_crash_decode.py --release v<version>` fetches the zip and finds it.

    python tools/release_elfs.py pack --version 3.1.0 --out dist <images and ELFs ...>
    python tools/release_elfs.py pack --version 3.1.0 --out dist <...> --upload v3.1.0

Inputs are files or folders, searched recursively:
  - images: `*.bin` that start with an ESP app image (lakeshark.bin, the
    flat `tdp4lr-3.1.0-g...-lakeshark.bin` release copies, a gate's
    `images/<edition>/lakeshark.bin`);
  - ELFs: `*.elf`, or a zip holding one (`lakeshark.elf.zip` in a gate's
    artifacts branch), or a build folder's `elf-archive/`.
Every image's app descriptor carries the sha256 of the ELF it was linked
from (esp_app_desc_t.app_elf_sha256), so each image is matched to its ELF by
that hash, never by name or folder. An image without its ELF stops the pack:
publish nothing rather than a release whose crashes cannot be read. Images
that are byte-identical (3.x ships one image as tdp4, tdp4lr, tdp4net and
tdp4u) share one ELF. An image carried over from an older release (tdp4tft
2.23.2 on v3.1.0) is skipped with --carried NAME: its ELF belongs to the
release that built it.

--upload checks before it attaches anything that the ELFs give away nothing
the public repo does not already hold: the image's commit (from its version,
`3.1.0-g5d4c0f3b85a9`) must differ from the public tag only outside the
firmware sources (bench/, docs/, tools/, assets/, top-level .md). ELF symbols
and DWARF name every compiled function, file and path, so a build from a
private-only tree would leak it. Since 3.0.0 the public release tree is the
private release tree, so this passes; it guards against that changing.
"""
import argparse
import hashlib
import io
import json
import os
import re
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = "SAMS0N1TE/LakeShark"
# Paths whose content never reaches a firmware image (host tools, tests, docs).
# Kept narrow on purpose: a path missing here only makes the check fail safe.
NOT_FIRMWARE = re.compile(r"^(bench|docs|tools|flipper-app|\.github|assets)/|^[^/]+\.md$|^LICENSE$")


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def app_desc(image):
    """(version, elf_sha256) from an ESP app image, or None if it is not one."""
    # image header (24), first segment header (8), then esp_app_desc_t:
    # magic +0, version +16, project_name +48, ..., app_elf_sha256 +144.
    if len(image) < 256 or image[0] != 0xE9 or image[32:36] != bytes.fromhex("3254cdab"):
        return None
    version = image[48:80].split(b"\0", 1)[0].decode("ascii", "replace")
    return version, image[176:208].hex()


def walk(paths):
    for p in paths:
        if os.path.isdir(p):
            for d, _, names in os.walk(p):
                for n in sorted(names):
                    yield os.path.join(d, n)
        elif os.path.isfile(p):
            yield p
        else:
            sys.exit(f"not found: {p}")


def elves(path):
    """ELF byte strings found in a file: the file itself, or members of a zip."""
    low = path.lower()
    if low.endswith(".elf"):
        with open(path, "rb") as fh:
            data = fh.read()
        if data[:4] == b"\x7fELF":
            yield path, data
    elif low.endswith(".zip"):
        try:
            z = zipfile.ZipFile(path)
        except zipfile.BadZipFile:
            return
        with z:
            for n in z.namelist():
                if n.lower().endswith(".elf"):
                    data = z.read(n)
                    if data[:4] == b"\x7fELF":
                        yield f"{path}!{n}", data


def collect(paths, carried):
    images, found = {}, {}
    for p in walk(paths):
        low = p.lower()
        if low.endswith(".bin"):
            with open(p, "rb") as fh:
                data = fh.read()
            desc = app_desc(data)
            if desc and os.path.basename(p) not in carried:
                images.setdefault(sha256(data), (desc, []))[1].append(p)
        else:
            for src, data in elves(p):
                found.setdefault(sha256(data), (src, data))
    return images, found


def check_public(version, tag, remote):
    m = re.search(r"-g([0-9a-f]{7,40})(-dirty)?$", version)
    if not m:
        sys.exit(f"{version}: no commit in the version, cannot check it against the public tree")
    if m.group(2):
        sys.exit(f"{version}: built from a dirty tree; a release image must come from a commit")
    def git(*args):
        r = subprocess.run(["git", "-C", ROOT, *args], capture_output=True, text=True)
        return r.returncode, r.stdout.strip(), r.stderr.strip()
    rc, commit, _ = git("rev-parse", "--verify", "-q", m.group(1) + "^{commit}")
    if rc:
        sys.exit(f"commit {m.group(1)} is not in this clone: fetch the private remote first")
    rc, _, err = git("fetch", "-q", remote, "tag", tag)
    if rc:
        sys.exit(f"cannot fetch tag {tag} from '{remote}': {err}")
    rc, out, err = git("diff", "--name-only", commit, tag + "^{commit}")
    if rc:
        sys.exit(err)
    leak = [p for p in out.splitlines() if not NOT_FIRMWARE.search(p)]
    if leak:
        sys.exit(f"{version} was built from sources the public {tag} does not have, and its ELF would "
                 f"name them. Differing firmware paths:\n  " + "\n  ".join(leak[:40]))
    print(f"public check: {commit[:12]} and {tag} hold the same firmware sources")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                 epilog=__doc__.split("\n", 2)[2], formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    pk = sub.add_parser("pack", help="match images to ELFs and write lakeshark-<version>-elf.zip")
    pk.add_argument("inputs", nargs="+", help="image .bin files, .elf/.elf.zip files, or folders")
    pk.add_argument("--version", required=True, help="release version, e.g. 3.1.0")
    pk.add_argument("--out", default=".", help="folder for the zip")
    pk.add_argument("--carried", action="append", default=[],
                    help="file name of an image carried over from an older release (no ELF needed)")
    pk.add_argument("--sums", metavar="FILE", help="add the zip's line to this SHA256SUMS (uploaded too with --upload)")
    pk.add_argument("--upload", metavar="TAG", help=f"attach the zip to this release of {REPO} with gh")
    pk.add_argument("--remote", default="public", help="git remote of the public repo (for --upload's check)")
    pk.add_argument("--skip-public-check", action="store_true",
                    help="upload without comparing the build commit to the public tag (owner's call)")
    a = ap.parse_args()

    images, found = collect(a.inputs, set(a.carried))
    if not images:
        sys.exit("no ESP app images among the inputs")
    by_elf, missing = {}, []
    for img_sha, ((version, elf_sha), files) in sorted(images.items(), key=lambda kv: kv[1][1][0]):
        # flat release copies are named by edition; a build's lakeshark.bin by its folder
        names = sorted({os.path.basename(f) if os.path.basename(f) != "lakeshark.bin"
                        else os.path.basename(os.path.dirname(os.path.abspath(f))) + "/lakeshark.bin"
                        for f in files})
        if elf_sha not in found:
            missing.append(f"{version} ({', '.join(names)}) needs ELF {elf_sha[:12]}")
            continue
        by_elf.setdefault(elf_sha, {"version": version, "images": []})["images"] += names
    if missing:
        sys.exit("no ELF for:\n  " + "\n  ".join(missing)
                 + "\npass the folder that holds it, or --carried NAME for an image from an older release")
    versions = sorted({e["version"] for e in by_elf.values()})
    stray = [v for v in versions if not v.startswith(a.version + "-")]
    if stray:
        sys.exit(f"images of another version than {a.version}: {', '.join(stray)} (use --carried for those)")

    os.makedirs(a.out, exist_ok=True)
    out = os.path.join(a.out, f"lakeshark-{a.version}-elf.zip")
    index = io.StringIO()
    index.write(f"# LakeShark {a.version}: ELF per shipped image. Decode a crash with\n"
                f"#   python tools/ls_crash_decode.py <report> --release v{a.version}\n"
                f"# <elf sha256>  <image version>  <release files>\n")
    def put(z, name, data):
        # A fixed date: the same ELFs always give the same zip and SHA256SUMS line.
        info = zipfile.ZipInfo(name, date_time=(2020, 1, 1, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        z.writestr(info, data, compresslevel=9)

    with zipfile.ZipFile(out, "w") as z:
        for elf_sha, e in sorted(by_elf.items()):
            src, data = found[elf_sha]
            put(z, f"{elf_sha[:12]}.elf", data)
            index.write(f"{elf_sha}  {e['version']}  {' '.join(e['images'])}\n")
            print(f"{elf_sha[:12]}.elf  {e['version']}  <- {src}\n    for {', '.join(e['images'])}")
        put(z, "ELFS.txt", index.getvalue())
        put(z, "ELFS.json", json.dumps(by_elf, indent=2, sort_keys=True) + "\n")
    with zipfile.ZipFile(out) as z:
        if z.testzip():
            sys.exit(f"{out}: CRC check failed")
    with open(out, "rb") as fh:
        line = f"{sha256(fh.read())} *{os.path.basename(out)}"
    print(f"wrote {out}\nSHA256SUMS line: {line}")
    if a.sums:
        old = open(a.sums, encoding="utf-8").read().splitlines() if os.path.exists(a.sums) else []
        keep = [l for l in old if not l.endswith("*" + os.path.basename(out))]
        with open(a.sums, "w", encoding="utf-8", newline="\n") as fh:
            fh.write("\n".join(keep + [line]) + "\n")
        print(f"added to {a.sums}")

    if a.upload:
        if not a.skip_public_check:
            for v in versions:
                check_public(v, a.upload, a.remote)
        files = [out] + ([a.sums] if a.sums else [])
        subprocess.run(["gh", "release", "upload", a.upload, "--clobber", "--repo", REPO, *files], check=True)
        print(f"attached to {REPO} {a.upload}: " + ", ".join(os.path.basename(f) for f in files))
        if not a.sums:
            print("the release's SHA256SUMS does not list the zip yet: rerun with --sums <its SHA256SUMS>")


if __name__ == "__main__":
    main()
