"""Render the translated screens with lssim in English and Ukrainian.

Every view is drawn portrait, landscape, and on the TFT with its camera hole,
in both languages, to bench/build-uk/<view>-<lang>-<shape>.png with the cell
dump beside it. With --compare BASE_LSSIM the English renders are also drawn
by a baseline lssim (a build of the parent commit) and compared byte for byte.

  python bench/tools/render_uk.py [--only NAME] [--compare path/to/lssim.exe --base-root DIR]
"""
import argparse
import os
import subprocess
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
EXE = ROOT / "bench/build/lssim.exe"
OUT = ROOT / "bench/build-uk"

# name, app, extra args. Keys: -K named keys, -k characters, -x taps.
VIEWS = [
    ("home-radio", "home", []),
    ("home-field", "home", ["-K", "f2"]),
    ("home-system", "home", ["-K", "f3"]),
    ("home-user", "home", ["-K", "f4"]),
    ("set", "set", []),
    ("set-display", "set", ["-K", "down,down,down,down,down,enter"]),
    ("set-sound", "set", ["-K", "down,down,down,down,down,down,enter"]),
    ("set-device", "set", ["-K", "down,down,down,down,down,down,down,enter"]),
    ("radios", "radios", []),
    ("fm", "fm", []),
    ("p25", "p25", []),
    ("adsb", "adsb", []),
    ("mesh", "mesh", []),
    ("drones", "drones", []),
    ("detector", "detector", []),
    ("notice", "home", ["-n", "MESH: new message"]),
    ("fm-options", "fm", ["-k", "o"]),
    ("p25-options", "p25", ["-k", "o"]),
    ("adsb-options", "adsb", ["-k", "o"]),
    ("mesh-options", "mesh", ["-x", "44:8"]),
    ("drones-options", "drones", ["-k", "o"]),
    ("detector-options", "detector", ["-k", "o"]),
    ("mesh-nodes", "mesh", ["-K", "right"]),
    ("lock", "set", ["-K", "down,down,down,enter"]),
]

SHAPES = {
    "portrait": [],
    "landscape": ["-l"],
    "tft": ["--tft"],
    "tft-landscape": ["--tft", "-l"],
}


def render(exe, root, view, lang, shape, out_dir):
    name, app, extra = view
    env = os.environ.copy()
    env.pop("LSSIM_LANG", None)
    if lang == "uk":
        env["LSSIM_LANG"] = "uk"
        env["LSSIM_I18N_MISSES"] = str(OUT / "misses.bin")
    if name in ("drones",):
        env["LSSIM_RID"] = "1"
    stem = out_dir / f"{name}-{lang}-{shape}"
    bmp = stem.with_suffix(".bmp")
    args = [str(exe), app, "-d", "-o", str(bmp), "-f", "6", *SHAPES[shape], *extra]
    res = subprocess.run(args, cwd=root, env=env, capture_output=True)
    stem.with_suffix(".txt").write_bytes(res.stdout)
    if res.returncode != 0 or not bmp.exists():
        print(f"FAIL {stem.name}: rc {res.returncode} {res.stdout[-200:]!r}")
        return None
    return bmp


def to_png(bmp, shape):
    pic = Image.open(bmp)
    if "landscape" in shape:
        pic = pic.rotate(-90, expand=True)
    png = bmp.with_suffix(".png")
    pic.save(png)
    return png


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only")
    ap.add_argument("--lang", default="en,uk")
    ap.add_argument("--shape", default=",".join(SHAPES))
    ap.add_argument("--compare", help="baseline lssim.exe for the English check")
    ap.add_argument("--base-root", help="the baseline's repository root")
    a = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    views = [v for v in VIEWS if not a.only or v[0].startswith(a.only)]
    same = differ = 0
    for view in views:
        for shape in a.shape.split(","):
            for lang in a.lang.split(","):
                bmp = render(EXE, ROOT, view, lang, shape, OUT)
                if not bmp:
                    continue
                print(to_png(bmp, shape))
                if lang == "en" and a.compare:
                    base_dir = OUT / "base"
                    base_dir.mkdir(exist_ok=True)
                    b = render(Path(a.compare), Path(a.base_root), view, "en", shape, base_dir)
                    if b and b.read_bytes() == bmp.read_bytes():
                        same += 1
                    else:
                        differ += 1
                        print(f"ENGLISH DIFFERS: {view[0]} {shape}")
    miss = OUT / "misses.bin"
    if miss.exists():
        words = sorted(set(w for w in miss.read_bytes().decode("utf-8", "replace").split("\0") if w))
        (OUT / "misses.txt").write_text("\n".join(words), encoding="utf-8")
        miss.unlink()
        print(f"{len(words)} strings drawn without a translation: {OUT / 'misses.txt'}")
    if a.compare:
        print(f"English byte-identical to the baseline: {same}, different: {differ}")
        return 1 if differ else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
