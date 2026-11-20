"""Fill a simulator tile cache from tiles.terminalbay.com.
   python prefill.py <cache_dir> <lat> <lon> <zmin> <zmax> [radius_tiles]"""
import math, os, sys, urllib.request, urllib.error
from concurrent.futures import ThreadPoolExecutor

root, lat, lon = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
z0, z1 = int(sys.argv[4]), int(sys.argv[5])
rad = int(sys.argv[6]) if len(sys.argv) > 6 else 3


def want():
    for z in range(z0, z1 + 1):
        n = 1 << z
        x = int((lon + 180) / 360 * n)
        r = math.radians(lat)
        y = int((1 - math.log(math.tan(r) + 1 / math.cos(r)) / math.pi) / 2 * n)
        for dx in range(-rad, rad + 1):
            for dy in range(-rad, rad + 1):
                if 0 <= y + dy < n:
                    yield z, (x + dx) % n, y + dy


def get(t):
    z, x, y = t
    path = os.path.join(root, str(z), str(x), "%d.mvt" % y)
    if os.path.exists(path):
        return 0
    os.makedirs(os.path.dirname(path), exist_ok=True)
    req = urllib.request.Request("https://tiles.terminalbay.com/v1/%d/%d/%d.mvt" % t, headers={"User-Agent": "tile-prefill/1"})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            body = r.read() if r.status == 200 else b""
    except urllib.error.HTTPError as e:
        print("fail", t, e.code)
        return 0
    with open(path, "wb") as f:
        f.write(body)
    return len(body)


tiles = sorted(set(want()))
with ThreadPoolExecutor(8) as ex:
    sizes = list(ex.map(get, tiles))
print("%d tiles, %d new bytes, largest %d" % (len(tiles), sum(sizes), max(sizes) if sizes else 0))
