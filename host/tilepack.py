#!/usr/bin/env python3
"""LS-739  Build an offline map tile pack for LakeShark.

Downloads slippy-map tiles around a point and writes them as JPEG in a plain
z/x/y tree, ready to drop on the SD card (upload via `wifi on`, or just put the
card in a reader).

WHY JPEG AND NOT PNG. The ESP32-P4 has a HARDWARE JPEG decoder (esp_driver_jpeg
is already in this build) plus the PPA blitter. PNG would have to be inflated in
software on a CPU that is also demodulating P25. Converting once here costs
nothing and moves the expensive half off the radio.

WHY 256px. That is the slippy-map standard and it divides the 480x800 screen
into a small enough working set that 6 tiles cover the visible area at any pan
position, which is what keeps the SD reads bounded.

TILE SERVER ETIQUETTE IS NOT OPTIONAL. OpenTopoMap is volunteer-run. This
rate-limits to one request at a time with a delay, sends a real User-Agent, and
skips anything already downloaded so a re-run is nearly free. Do not remove
those. Pull a region once, keep the pack, and do not point this at a whole
state - a bulk scrape gets the project blocked for everyone.

Usage:
  ./tilepack.py --lat 34.7759 --lon -111.7625 --radius-km 20 --zoom 11,12,13,14
  ./tilepack.py --lat .. --lon .. --radius-km 8 --zoom 15 --out /tmp/tiles
"""
import argparse, math, os, sys, time, urllib.request, io

try:
    from PIL import Image
except ImportError:
    print("needs pillow:  pip install pillow", file=sys.stderr)
    sys.exit(1)

SOURCES = {
    # Topographic - contours and trails. The right default for on-foot use.
    "topo":  "https://tile.opentopomap.org/{z}/{x}/{y}.png",
    # Plain street map - clearer for roads and towns.
    "osm":   "https://tile.openstreetmap.org/{z}/{x}/{y}.png",
}
UA = "LakeShark-tilepack/1.0 (offline handheld radio; personal use)"


def deg2num(lat, lon, z):
    lat_r = math.radians(lat)
    n = 2.0 ** z
    x = int((lon + 180.0) / 360.0 * n)
    y = int((1.0 - math.asinh(math.tan(lat_r)) / math.pi) / 2.0 * n)
    return x, y


def num2deg(x, y, z):
    n = 2.0 ** z
    lon = x / n * 360.0 - 180.0
    lat = math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * y / n))))
    return lat, lon


def fetch(url, tries=3):
    for i in range(tries):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": UA})
            with urllib.request.urlopen(req, timeout=25) as r:
                return r.read()
        except Exception as e:
            if i == tries - 1:
                print(f"  ! {url}: {e}", file=sys.stderr)
                return None
            time.sleep(2 * (i + 1))
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lat", type=float, required=True)
    ap.add_argument("--lon", type=float, required=True)
    ap.add_argument("--radius-km", type=float, default=15.0)
    ap.add_argument("--zoom", default="11,12,13,14",
                    help="comma list; 12-14 is a good hiking range")
    ap.add_argument("--source", default="topo", choices=sorted(SOURCES))
    ap.add_argument("--out", default="tiles")
    ap.add_argument("--quality", type=int, default=85)
    ap.add_argument("--delay", type=float, default=0.35,
                    help="seconds between requests - do not set to 0")
    ap.add_argument("--max-tiles", type=int, default=3000,
                    help="hard stop, so a typo in radius cannot scrape a state")
    args = ap.parse_args()

    zooms = [int(z) for z in args.zoom.split(",") if z.strip()]
    tmpl = SOURCES[args.source]

    # Bounding box from a radius: 1 deg lat is ~111 km, lon shrinks by cos(lat).
    dlat = args.radius_km / 111.0
    dlon = args.radius_km / (111.0 * math.cos(math.radians(args.lat)))
    north, south = args.lat + dlat, args.lat - dlat
    west,  east  = args.lon - dlon, args.lon + dlon

    plan = []
    for z in zooms:
        x0, y0 = deg2num(north, west, z)     # NW corner -> min x, min y
        x1, y1 = deg2num(south, east, z)     # SE corner -> max x, max y
        for x in range(min(x0, x1), max(x0, x1) + 1):
            for y in range(min(y0, y1), max(y0, y1) + 1):
                plan.append((z, x, y))

    print(f"area {south:.4f}..{north:.4f} N, {west:.4f}..{east:.4f} E")
    print(f"zooms {zooms} -> {len(plan)} tiles from '{args.source}'")
    if len(plan) > args.max_tiles:
        print(f"REFUSING: {len(plan)} tiles exceeds --max-tiles {args.max_tiles}.\n"
              f"Lower --radius-km or drop the highest zoom. Each zoom level "
              f"QUADRUPLES the count.", file=sys.stderr)
        sys.exit(2)

    got = skipped = failed = 0
    total_bytes = 0
    for i, (z, x, y) in enumerate(plan, 1):
        d = os.path.join(args.out, str(z), str(x))
        os.makedirs(d, exist_ok=True)
        path = os.path.join(d, f"{y}.jpg")
        if os.path.exists(path) and os.path.getsize(path) > 0:
            skipped += 1
            total_bytes += os.path.getsize(path)
            continue
        raw = fetch(tmpl.format(z=z, x=x, y=y))
        if not raw:
            failed += 1
            continue
        try:
            im = Image.open(io.BytesIO(raw)).convert("RGB")
            im.save(path, "JPEG", quality=args.quality, optimize=True)
            total_bytes += os.path.getsize(path)
            got += 1
        except Exception as e:
            print(f"  ! convert {z}/{x}/{y}: {e}", file=sys.stderr)
            failed += 1
        time.sleep(args.delay)
        if i % 25 == 0:
            print(f"  {i}/{len(plan)}  new={got} cached={skipped} fail={failed}")

    # A manifest so the board does not have to walk the tree to learn its extent.
    with open(os.path.join(args.out, "pack.txt"), "w") as f:
        f.write(f"source {args.source}\n")
        f.write(f"center {args.lat:.6f} {args.lon:.6f}\n")
        f.write(f"bbox {south:.6f} {west:.6f} {north:.6f} {east:.6f}\n")
        f.write(f"zooms {','.join(str(z) for z in zooms)}\n")
        f.write(f"tiles {got + skipped}\n")

    print(f"\ndone: {got} new, {skipped} cached, {failed} failed, "
          f"{total_bytes/1e6:.1f} MB in {args.out}/")
    print(f"copy {args.out}/ to the SD card root, or upload via `wifi on`")


if __name__ == "__main__":
    main()
