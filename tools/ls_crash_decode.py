#!/usr/bin/env python3
"""Decode the crash in a LakeShark report (or `crash` console output).

The board reads its stored core dump itself (ls_crash_parse_elf) and puts the
faulting task, pc, ra, the panic reason, the crashed image's ELF sha and the
code addresses found on the crashed task's stack into `crash` and into every
report sent with `report send`. This turns those addresses into functions and
lines with addr2line and the ELF of the build that crashed.

    python tools/ls_crash_decode.py --report 0mwwa3t1x-c6e88b
    python tools/ls_crash_decode.py report.json --elf build_tdp4/lakeshark.elf
    python tools/ls_crash_decode.py crash.txt          # pasted `crash` output
    python tools/ls_crash_decode.py --report 0mwwa3t1x-c6e88b --release auto

--release fetches the ELFs a GitHub release keeps (`lakeshark-<version>-elf.zip`,
attached by tools/release_elfs.py) into build_release/elf-archive/, where the
search below finds them from then on. `auto` takes the tag from the report's
version (3.1.0-g5d4c0f3b85a9 -> v3.1.0).

--report fetches the report from the hub's R2 bucket with wrangler (needs a
`wrangler login` with R2 access). Without --elf the ELF is found by its sha
among build*/elf-archive/, build*/lakeshark.elf and --search folders.

The stack scan lists every code address left on the crashed task's stack,
newest first: return addresses of the callers, and sometimes a stale one from
an earlier call. Read it as a list of suspects, top down.
"""
import argparse
import glob
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RELEASE_REPO = "SAMS0N1TE/LakeShark"
RELEASE_ELFS = os.path.join(ROOT, "build_release", "elf-archive")


def fetch_report(report_id):
    if not re.fullmatch(r"[0-9a-z]{9}-[0-9a-f]{6}", report_id):
        sys.exit(f"not a report id: {report_id}")
    wrangler = shutil.which("wrangler") or shutil.which("wrangler.cmd")
    if not wrangler:
        sys.exit("wrangler not found; download the report another way and pass the file")
    out = os.path.join(tempfile.mkdtemp(prefix="lsreport-"), report_id + ".json")
    r = subprocess.run([wrangler, "r2", "object", "get", f"lakeshark-reports/r/{report_id}",
                        "--remote", "--file", out], cwd=os.path.join(ROOT, "bench", "hub_worker"),
                       capture_output=True, text=True)
    if r.returncode or not os.path.exists(out) or not os.path.getsize(out):
        sys.exit("wrangler could not fetch the report (run `wrangler login` with R2 access):\n"
                 + (r.stdout + r.stderr)[-800:])
    return out


def crash_text(path):
    raw = open(path, encoding="utf-8", errors="replace").read()
    try:
        j = json.loads(raw)
    except ValueError:
        return raw, {}
    return j.get("crash", ""), j


def parse(text):
    f = {}
    m = re.search(r'crash: task "([^"]*)" pc=0x([0-9a-f]{8}) ra=0x([0-9a-f]{8})', text)
    if m:
        f["task"], f["pc"], f["ra"] = m.group(1), int(m.group(2), 16), int(m.group(3), 16)
    m = re.search(r"elf-sha=([0-9a-f]+)", text)
    if m:
        f["sha"] = m.group(1)
    m = re.search(r"reason: (.*)", text)
    if m:
        f["reason"] = m.group(1).strip()
    m = re.search(r"backtrace(?: \(stack scan\))?:((?: 0x[0-9a-f]{8})+)", text)
    f["bt"] = [int(a, 16) for a in m.group(1).split()] if m else []
    return f


def elf_sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def find_elf(sha, extra):
    pats = [os.path.join(ROOT, "build*", "elf-archive", "*.elf"), os.path.join(ROOT, "build*", "lakeshark.elf")]
    for d in extra:
        pats += [os.path.join(d, "**", "*.elf")]
    seen = set()
    for pat in pats:
        for p in glob.glob(pat, recursive=True):
            p = os.path.abspath(p)
            if p in seen:
                continue
            seen.add(p)
            # elf-archive names a file by its sha's first 12 characters
            name = os.path.splitext(os.path.basename(p))[0]
            if len(name) == 12 and not (name.startswith(sha[:12]) or sha.startswith(name)):
                continue
            if elf_sha(p).startswith(sha):
                return p
    return None


def release_tag(arg, version):
    if arg != "auto":
        return arg if arg.startswith("v") else "v" + arg
    m = re.match(r"(\d+\.\d+\.\d+)", version or "")
    if not m:
        sys.exit(f"--release auto: no version in the report ({version!r}); pass the tag")
    return "v" + m.group(1)


def fetch_release_elfs(tag):
    """Download the release's *-elf.zip assets and file each ELF as <sha12>.elf."""
    tmp = tempfile.mkdtemp(prefix="lself-")
    zips = []
    try:
        req = urllib.request.Request(f"https://api.github.com/repos/{RELEASE_REPO}/releases/tags/{tag}",
                                     headers={"Accept": "application/vnd.github+json"})
        with urllib.request.urlopen(req, timeout=30) as r:
            assets = json.load(r).get("assets", [])
        for a in assets:
            if a["name"].endswith("-elf.zip"):
                dest = os.path.join(tmp, os.path.basename(a["name"]))
                print(f"fetching {a['name']} ({a['size'] >> 20} MB) from {RELEASE_REPO} {tag}")
                urllib.request.urlretrieve(a["browser_download_url"], dest)
                zips.append(dest)
    except Exception as e:  # rate limit or no network: gh may still get through
        gh = shutil.which("gh")
        print(f"GitHub API: {e}" + ("; trying gh" if gh else ""))
        if gh and not subprocess.run([gh, "release", "download", tag, "--repo", RELEASE_REPO,
                                      "-p", "*-elf.zip", "-D", tmp]).returncode:
            zips = glob.glob(os.path.join(tmp, "*-elf.zip"))
    if not zips:
        print(f"{RELEASE_REPO} {tag} keeps no ELFs (releases before the ELF zip was added do not)")
        return
    os.makedirs(RELEASE_ELFS, exist_ok=True)
    for zp in zips:
        with zipfile.ZipFile(zp) as z:
            for n in z.namelist():
                if not n.lower().endswith(".elf"):
                    continue
                data = z.read(n)
                sha = hashlib.sha256(data).hexdigest()
                # named by our own hash, never by the member's path
                with open(os.path.join(RELEASE_ELFS, sha[:12] + ".elf"), "wb") as fh:
                    fh.write(data)
                print(f"  {sha[:12]}.elf -> {os.path.relpath(RELEASE_ELFS, ROOT)}")


def addr2line():
    for name in ("riscv32-esp-elf-addr2line", "riscv32-esp-elf-addr2line.exe"):
        p = shutil.which(name)
        if p:
            return p
    hits = sorted(glob.glob(os.path.join(os.environ.get("IDF_TOOLS_PATH", r"C:\Espressif"), "tools",
                                         "riscv32-esp-elf", "*", "riscv32-esp-elf", "bin",
                                         "riscv32-esp-elf-addr2line*")))
    return hits[-1] if hits else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file", nargs="?", help="report JSON, or text holding `crash` output")
    ap.add_argument("--report", help="hub report id to fetch with wrangler")
    ap.add_argument("--elf", help="the ELF of the build that crashed")
    ap.add_argument("--search", action="append", default=[], help="more folders to look for the ELF in")
    ap.add_argument("--release", metavar="TAG", help="fetch the ELFs kept on this GitHub release (vX.Y.Z, "
                                                     "or auto for the report's version) when none is found here")
    a = ap.parse_args()
    if not a.file and not a.report:
        ap.error("give a file or --report ID")
    path = fetch_report(a.report) if a.report else a.file
    text, meta = crash_text(path)
    if meta:
        print(f"report: {meta.get('board', '?')} {meta.get('version', '?')}  up {meta.get('uptime_s', '?')} s")
    f = parse(text)
    if "pc" not in f:
        print(text.strip() or "(no crash section)")
        sys.exit("no decodable crash in it: the board did not read the dump (older firmware, or see the line above)")

    print(f"task {f['task'] or '?'}" + (f"  - {f['reason']}" if f.get("reason") else ""))
    elf = a.elf or (find_elf(f["sha"], a.search) if f.get("sha") else None)
    if not elf and a.release and f.get("sha"):
        fetch_release_elfs(release_tag(a.release, meta.get("version")))
        elf = find_elf(f["sha"], a.search)
    if not elf:
        print(f"no ELF with sha {f.get('sha', '?')} found: pass --elf, --release vX.Y.Z, "
              "or --search the folder that keeps it")
        for k in ("pc", "ra"):
            print(f"  {k} 0x{f[k]:08x}")
        for x in f["bt"]:
            print(f"  .. 0x{x:08x}")
        sys.exit(2)
    if f.get("sha") and not elf_sha(elf).startswith(f["sha"]):
        print(f"WARNING: {elf} is not the build that crashed (sha {elf_sha(elf)[:16]} vs {f['sha']}); lines may be wrong")
    tool = addr2line()
    if not tool:
        sys.exit("riscv32-esp-elf-addr2line not found: run from an ESP-IDF shell")
    addrs = [f["pc"], f["ra"]] + f["bt"]
    # A return address points after the call; one byte back lands on the call.
    q = [f"0x{f['pc']:08x}"] + [f"0x{x - 1:08x}" for x in addrs[1:]]
    out = subprocess.run([tool, "-pfiaC", "-e", elf] + q, capture_output=True, text=True).stdout
    frames = re.split(r"(?m)^(?=0x[0-9a-f]{8}: )", out.strip())
    print(f"elf  {elf}")
    frames = [s for s in frames if s]
    # The first frame in LakeShark's own code, not IDF's or newlib's: the
    # usual place to start reading.
    for fr in frames:
        m = re.search(r"^0x[0-9a-f]{8}: (\S+) at (\S+)", fr)
        if m and "/esp-idf/" not in m.group(2) and "/builds/idf/" not in m.group(2) and "??" not in m.group(2):
            src = m.group(2).replace("\\", "/")
            src = src.split("/ls-wdmap/")[-1] if "/ls-wdmap/" in src else src
            print(f"suspect  {m.group(1)}  {src}")
            break
    labels = ["pc", "ra"] + [f"#{i}" for i in range(len(f["bt"]))]
    for lab, x, fr in zip(labels, addrs, frames):
        body = re.sub(r"^0x[0-9a-f]{8}: ", "", fr.strip()).replace("\n", "\n                ")
        print(f"  {lab:>3} 0x{x:08x}  {body}")


if __name__ == "__main__":
    main()
