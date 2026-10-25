"""CARRIER ship hulls: each ship is a few convex parts (points on the +x side are
mirrored), hulled with scipy, faces tagged with shared materials, written as C.
usage: python bench/tools/carrier_hulls.py  ->  app-sdk/apps/carrier/hulls.inc
Nose is -Z, up is +Y. Materials: the scene creates the shared ones first, so a
face's number is fixed: 0 engine glow, 1 canopy glass, 2 dark panel, 3 metal,
4 docking slot, 5 lamp; 255 is the ship's own (faction) hull."""
import math, os, random
import numpy as np
from scipy.spatial import ConvexHull

HULL, ENGINE, GLASS, DARK, METAL, SLOT, LAMP = 255, 0, 1, 2, 3, 4, 5
OUT = os.path.join(os.path.dirname(__file__), '..', '..', 'app-sdk', 'apps', 'carrier', 'hulls.inc')


def mirror(pts):
    out = []
    for p in pts:
        out.append(tuple(p))
        if abs(p[0]) > 1e-6:
            out.append((-p[0], p[1], p[2]))
    return out


def box(x0, x1, y0, y1, z0, z1):
    return [(x, y, z) for x in (x0, x1) for y in (y0, y1) for z in (z0, z1)]


def prism(n, r, y, z0, z1, rot=0.0, scale1=1.0):
    """An n-sided ring at z0 and a scaled one at z1, around the z axis."""
    pts = []
    for k in range(n):
        a = rot + 2 * math.pi * k / n
        pts.append((r * math.cos(a), y + r * math.sin(a), z0))
        pts.append((r * scale1 * math.cos(a), y + r * scale1 * math.sin(a), z1))
    return pts


class Part:
    def __init__(self, pts, mat=HULL, sym=True, rear=None, under=None, front=None, pair=False):
        # sym: one hull of the points and their mirror image (a body on the centre line);
        # pair: two hulls, the points and their mirror image apart (wings, fins, pods).
        self.pts = mirror(pts) if sym and not pair else [tuple(p) for p in pts]
        self.mat, self.rear, self.under, self.front, self.pair = mat, rear, under, front, pair


def expand(parts):
    out = []
    for part in parts:
        out.append(part)
        if part.pair:
            twin = Part([(-x, y, z) for (x, y, z) in part.pts], part.mat, False, part.rear, part.under, part.front)
            out.append(twin)
    return out


def hull(parts):
    verts, tris, mats = [], [], []
    for part in expand(parts):
        p = np.array(part.pts, dtype=float)
        h = ConvexHull(p)
        centre = p.mean(axis=0)
        base = len(verts)
        used = sorted(set(h.simplices.flatten()))
        remap = {v: base + i for i, v in enumerate(used)}
        verts += [tuple(p[v]) for v in used]
        zmax = p[:, 2].max()
        for s in h.simplices:
            a, b, c = p[s[0]], p[s[1]], p[s[2]]
            n = np.cross(b - a, c - a)
            if np.dot(n, (a + b + c) / 3 - centre) < 0:
                s = [s[0], s[2], s[1]]
                n = -n
            n = n / (np.linalg.norm(n) or 1)
            m = part.mat
            mid = (a + b + c) / 3
            if part.rear is not None and n[2] > 0.85 and mid[2] > zmax - 0.05:
                m = part.rear
            if part.under is not None and n[1] < -0.7:
                m = part.under
            if part.front is not None and n[2] < -0.85:
                m = part.front
            tris.append([remap[s[0]], remap[s[1]], remap[s[2]]])
            mats.append(m)
    # Same winding as the engine expects (counter-clockwise seen from outside).
    return verts, tris, mats


def engines(xs, y, half_w, half_h, z0, z1):
    return [Part(box(x - half_w, x + half_w, y - half_h, y + half_h, z0, z1), DARK, sym=False, rear=ENGINE)
            for x in xs]


SHIPS = {}

# Cobra Mk III: the wide flat wedge, short nose edge, swept wings, twin engines.
SHIPS['cobra'] = [
    Part([(0.32, 0.02, -1.45), (0.32, -0.16, -1.45), (0.62, 0.36, 0.05), (0.62, -0.3, 0.0),
          (2.15, -0.04, 0.72), (1.95, 0.06, 0.82), (0.95, 0.36, 0.84), (0.95, -0.3, 0.84),
          (1.75, -0.12, 0.82)], rear=DARK),
    Part([(0.0, 0.33, -0.75), (0.2, 0.33, -0.55), (0.0, 0.47, -0.25), (0.17, 0.44, -0.12),
          (0.0, 0.44, 0.15), (0.14, 0.37, 0.15)], GLASS),
    *engines([-0.55, 0.55], 0.03, 0.22, 0.12, 0.8, 0.92),
    Part(box(1.98, 2.04, -0.04, 0.32, 0.45, 0.8), METAL, rear=DARK, pair=True),
]

# Sidewinder: wide lip in front, narrow tail, very flat.
SHIPS['sidewinder'] = [
    Part([(0.85, 0.0, -0.9), (0.85, -0.1, -0.85), (1.45, -0.04, -0.15), (0.55, 0.18, 0.75),
          (0.55, -0.16, 0.75), (0.0, 0.34, 0.1), (0.0, -0.22, 0.2), (0.0, 0.1, -0.95)], rear=DARK),
    Part([(0.0, 0.3, -0.25), (0.22, 0.27, -0.05), (0.0, 0.4, 0.15), (0.15, 0.3, 0.3)], GLASS),
    *engines([-0.28, 0.28], 0.0, 0.16, 0.09, 0.72, 0.8),
]

# Viper: police interceptor; needle nose, delta wings, twin tail fins.
SHIPS['viper'] = [
    Part([(0.0, 0.05, -2.05), (0.42, 0.28, 0.1), (0.42, -0.24, 0.1), (0.42, 0.28, 1.0),
          (0.42, -0.24, 1.0), (0.0, 0.4, 0.3), (0.0, -0.32, 0.4)], rear=DARK),
    Part([(0.38, 0.02, -0.5), (1.45, -0.04, 0.88), (0.38, 0.06, 1.0), (0.38, -0.06, 1.0),
          (1.4, -0.08, 1.0)], METAL, pair=True),
    Part([(0.3, 0.28, 0.35), (0.38, 0.28, 0.35), (0.3, 0.95, 1.0), (0.38, 0.95, 1.0),
          (0.3, 0.28, 1.0), (0.38, 0.28, 1.0)], pair=True),
    Part([(0.0, 0.3, -0.7), (0.17, 0.27, -0.45), (0.0, 0.46, -0.05), (0.14, 0.34, 0.05)], GLASS),
    *engines([-0.2, 0.2], 0.02, 0.15, 0.14, 0.98, 1.08),
]

# Krait: pirate fighter, a body between two long forward prongs.
SHIPS['krait'] = [
    Part([(0.0, 0.3, -0.85), (0.48, 0.0, -0.45), (0.48, 0.02, 0.95), (0.0, 0.32, 0.95),
          (0.0, -0.24, 0.95), (0.0, -0.22, -0.6)], rear=DARK),
    Part(box(1.08, 1.24, -0.07, 0.07, -1.7, 0.85), METAL, rear=DARK, front=LAMP, pair=True),
    Part([(0.45, 0.04, -0.2), (1.15, 0.03, -0.35), (1.15, 0.03, 0.8), (0.45, 0.04, 0.9),
          (0.45, -0.04, -0.2), (1.15, -0.03, -0.35), (1.15, -0.03, 0.8), (0.45, -0.04, 0.9)], pair=True),
    Part([(0.0, 0.28, -0.55), (0.15, 0.22, -0.35), (0.0, 0.4, -0.05)], GLASS),
    *engines([0.0], 0.04, 0.24, 0.12, 0.93, 1.02),
]

# Adder: a small boxy hauler with stub wings.
SHIPS['adder'] = [
    Part([(0.34, 0.14, -1.15), (0.34, -0.16, -1.15), (0.7, 0.26, -0.3), (0.7, -0.22, -0.3),
          (0.7, 0.26, 0.95), (0.7, -0.22, 0.95), (0.28, 0.48, -0.05), (0.28, 0.48, 0.95),
          (0.28, -0.36, 0.95), (0.28, -0.36, -0.2)], rear=DARK),
    Part(box(0.68, 1.15, -0.06, 0.04, 0.2, 0.9), METAL, pair=True),
    Part([(0.0, 0.42, -0.45), (0.22, 0.4, -0.25), (0.0, 0.54, -0.05), (0.2, 0.5, 0.05)], GLASS),
    *engines([-0.35, 0.35], 0.05, 0.2, 0.14, 0.93, 1.02),
]

# Python: a heavy trader, a fat diamond with a high spine.
SHIPS['python'] = [
    Part([(0.0, 0.0, -2.45), (0.25, -0.1, -2.3), (1.15, 0.0, -0.15), (0.0, 0.92, 0.2),
          (0.0, 0.82, 1.65), (0.0, -0.72, 0.4), (1.0, 0.32, 1.65), (1.0, -0.3, 1.65),
          (0.45, -0.55, 1.65)], rear=DARK),
    Part([(0.0, 0.62, -0.7), (0.22, 0.52, -0.55), (0.0, 0.9, -0.05), (0.2, 0.75, 0.1)], GLASS),
    *engines([-0.55, 0.0, 0.55], 0.0, 0.17, 0.2, 1.62, 1.74),
]

# Anaconda: the long freighter with a raised bridge at the back.
SHIPS['anaconda'] = [
    Part([(0.25, 0.18, -3.25), (0.25, -0.3, -3.25), (0.78, 0.46, -1.0), (0.78, -0.56, -1.0),
          (0.78, 0.46, 2.8), (0.78, -0.56, 2.8), (0.42, 0.68, 2.8), (0.42, 0.68, -0.6),
          (0.42, -0.72, 2.8), (0.42, -0.72, -0.8)], rear=DARK),
    Part([(0.0, 0.66, 1.2), (0.38, 0.66, 1.3), (0.0, 1.12, 1.75), (0.32, 1.08, 1.85),
          (0.0, 1.08, 2.6), (0.32, 1.05, 2.6), (0.38, 0.66, 2.6)], DARK),
    Part([(0.0, 0.98, 1.55), (0.3, 0.95, 1.65), (0.0, 1.14, 1.78), (0.28, 1.1, 1.85)], GLASS),
    Part(box(0.76, 0.84, -0.4, 0.3, -0.6, 2.2), METAL),
    *engines([-0.45, 0.0, 0.45], -0.05, 0.16, 0.22, 2.77, 2.9),
]

# Boa: a hexagonal freighter with three engine pods.
SHIPS['boa'] = [
    Part(prism(6, 0.36, 0.0, -2.35, -2.35) + prism(6, 0.9, 0.0, -0.7, 1.55, rot=math.pi / 6),
         sym=False, rear=DARK),
    *[Part(prism(8, 0.26, 0.0, 1.3, 2.0, scale1=0.85), DARK, sym=False, rear=ENGINE)
      for _ in range(0)],
]
for k in range(3):
    a = math.pi / 2 + 2 * math.pi * k / 3
    cx, cy = 0.62 * math.cos(a), 0.62 * math.sin(a)
    pod = [(cx + x, cy + y, z) for (x, y, z) in prism(6, 0.26, 0.0, 1.0, 1.9, scale1=0.8)]
    SHIPS['boa'].append(Part(pod, DARK, sym=False, rear=ENGINE))
SHIPS['boa'].append(Part([(0.0, 0.45, -1.45), (0.2, 0.38, -1.25), (0.0, 0.7, -0.8), (0.24, 0.58, -0.75)], GLASS))

# Moray: a rounded star boat with ventral fins.
SHIPS['moray'] = [
    Part([(0.3, 0.08, -1.65), (0.3, -0.12, -1.6), (0.92, 0.24, -0.6), (1.15, -0.02, 0.6),
          (0.62, 0.46, 0.25), (0.0, 0.62, 0.35), (0.55, -0.32, 0.9), (0.92, -0.1, 1.1),
          (0.6, 0.3, 1.15), (0.0, -0.36, 0.2)], rear=DARK),
    Part([(0.55, -0.22, 0.1), (0.6, -0.22, 0.1), (0.55, -0.85, 0.95), (0.6, -0.85, 0.95),
          (0.55, -0.25, 1.0), (0.6, -0.25, 1.0)], METAL, pair=True),
    Part([(0.0, 0.48, -0.55), (0.25, 0.4, -0.4), (0.0, 0.64, 0.0), (0.24, 0.52, 0.1)], GLASS),
    *engines([-0.4, 0.4], 0.08, 0.2, 0.14, 1.08, 1.18),
]

# Asp Mk II: an explorer, a flattened hexagon with a short nose.
SHIPS['asp'] = [
    Part([(0.25, 0.0, -1.65), (1.0, 0.05, -0.8), (1.62, 0.0, 0.3), (1.0, 0.05, 1.1),
          (0.32, 0.36, -0.5), (0.32, 0.36, 0.95), (0.32, -0.26, -0.5), (0.32, -0.26, 0.95),
          (1.0, -0.06, 1.1)], rear=DARK),
    Part([(0.0, 0.3, -0.95), (0.25, 0.3, -0.75), (0.0, 0.46, -0.35), (0.22, 0.42, -0.25)], GLASS),
    *engines([-0.55, 0.0, 0.55], 0.02, 0.16, 0.12, 1.06, 1.16),
]

# Worm: a tiny boxy fighter with side pods.
SHIPS['worm'] = [
    Part([(0.24, 0.18, -0.95), (0.24, -0.14, -0.95), (0.44, 0.26, 0.55), (0.44, -0.2, 0.55),
          (0.2, 0.36, 0.55), (0.2, 0.34, -0.3)], rear=DARK),
    Part(box(0.5, 0.72, -0.12, 0.1, -0.5, 0.6), METAL, rear=ENGINE, front=LAMP, pair=True),
    Part(box(0.4, 0.52, -0.03, 0.03, -0.2, 0.4), METAL, pair=True),
    Part([(0.0, 0.32, -0.45), (0.14, 0.3, -0.3), (0.0, 0.42, -0.05)], GLASS),
]

# Probe: a beacon, an octahedron core with spars and two solar sails.
SHIPS['probe'] = [
    Part([(0.0, 0.0, -0.75), (0.0, 0.0, 0.65), (0.42, 0.0, 0.0), (0.0, 0.42, 0.0), (0.0, -0.42, 0.0)]),
    Part(box(0.35, 1.5, -0.03, 0.03, -0.03, 0.03), METAL, pair=True),
    Part(box(-0.03, 0.03, 0.35, 1.3, -0.03, 0.03), METAL, sym=False),
    Part(box(0.85, 1.45, -0.02, 0.02, -0.38, 0.38), GLASS, pair=True),
    Part(box(-0.06, 0.06, 1.25, 1.37, -0.06, 0.06), LAMP, sym=False),
]

# Thargoid-like drone: an octagonal saucer that glows underneath.
oct_pts = (prism(8, 0.7, 0.0, 0, 0)[::2], prism(8, 1.5, 0.0, 0, 0)[::2], prism(8, 1.0, 0.0, 0, 0)[::2])
drone_pts = []
for ring, y in zip(oct_pts, (0.36, 0.0, -0.3)):
    drone_pts += [(x, y, yy) for (x, yy, _) in ring]
SHIPS['drone'] = [Part(drone_pts, sym=False, under=ENGINE),
                  Part([(x * 0.35, 0.36 + 0.14, z * 0.35) for (x, _, z) in drone_pts[:8]] +
                       [(x * 0.5, 0.36, z * 0.5) for (x, _, z) in drone_pts[:8]], GLASS, sym=False)]

# Coriolis: the cuboctahedron, the docking slot on its -Z face, lamps either side.
cubo = [(1, 1, 0), (1, -1, 0), (1, 0, 1), (1, 0, -1), (0, 1, 1), (0, 1, -1), (0, -1, 1), (0, -1, -1)]
SHIPS['station'] = [
    Part(cubo),
    Part(box(-0.45, 0.45, -0.12, 0.12, -1.02, -0.98), SLOT, sym=False),
    Part(box(0.52, 0.6, -0.04, 0.04, -1.03, -0.97), LAMP, pair=True),
    Part(box(-0.04, 0.04, 0.72, 0.8, -1.03, -0.97), LAMP, sym=False),
    Part(box(0.25, 0.4, 0.95, 1.06, -0.15, 0.15), METAL, pair=True),
    Part(box(-0.08, 0.08, -0.08, 0.08, 1.0, 1.25), METAL, sym=False),
]


def rock(seed, n=16, rough=0.35):
    r = random.Random(seed)
    pts = []
    for _ in range(n):
        z = r.uniform(-1, 1); a = r.uniform(0, 2 * math.pi); s = math.sqrt(1 - z * z)
        k = 1 - rough * r.random()
        pts.append((s * math.cos(a) * k * 1.2, s * math.sin(a) * k * 0.85, z * k))
    return [Part(pts, sym=False)]


SHIPS['rock1'] = rock(11)
SHIPS['rock2'] = rock(23, 18)
SHIPS['rock3'] = rock(37, 14, 0.45)

# Not ship classes, the art's own: cargo canister, missile, dodecahedral station.
SHIPS['canister'] = [Part(prism(8, 0.5, 0.0, -0.7, 0.7, rot=math.pi / 8), METAL, sym=False),
                     Part(prism(8, 0.52, 0.0, -0.1, 0.1, rot=math.pi / 8), LAMP, sym=False)]
SHIPS['missile'] = [Part(prism(6, 0.12, 0.0, -0.6, 0.7) + [(0, 0, -1.0)], METAL, sym=False, rear=ENGINE),
                    Part(box(0.0, 0.32, -0.015, 0.015, 0.35, 0.7), DARK),
                    Part(box(-0.015, 0.015, 0.0, 0.32, 0.35, 0.7), DARK, sym=False)]
phi = (1 + 5 ** 0.5) / 2
dode = [(x, y, z) for x in (-1, 1) for y in (-1, 1) for z in (-1, 1)]
for a, b in ((0, 1), (1, 2), (2, 0)):
    for s1 in (-1, 1):
        for s2 in (-1, 1):
            v = [0.0, 0.0, 0.0]; v[a] = s1 / phi; v[b] = s2 * phi; dode.append(tuple(v))
# Turn a face to -Z: face centres point along (0, +-1, +-phi) and friends.
fc = np.array([0.0, 1.0, phi]); fc /= np.linalg.norm(fc)
axis = np.cross(fc, [0, 0, -1.0]); ang = math.acos(np.dot(fc, [0, 0, -1.0])); axis /= np.linalg.norm(axis)
K = np.array([[0, -axis[2], axis[1]], [axis[2], 0, -axis[0]], [-axis[1], axis[0], 0]])
R = np.eye(3) + math.sin(ang) * K + (1 - math.cos(ang)) * K @ K
dode = [tuple(R @ np.array(p) / 1.4) for p in dode]
zf = min(p[2] for p in dode)
SHIPS['dodeca'] = [
    Part(dode, sym=False),
    Part(box(-0.38, 0.38, -0.1, 0.1, zf - 0.02, zf + 0.02), SLOT, sym=False),
    Part(box(0.46, 0.54, -0.04, 0.04, zf - 0.03, zf + 0.03), LAMP, pair=True),
    Part(box(-0.08, 0.08, -0.08, 0.08, -zf - 0.05, -zf + 0.3), METAL, sym=False),
]

ORDER = ['sidewinder', 'adder', 'cobra', 'python', 'anaconda', 'boa', 'viper', 'krait', 'moray', 'asp',
         'worm', 'probe', 'drone', 'station', 'rock1', 'rock2', 'rock3', 'canister', 'missile', 'dodeca']


def main():
    out = ['/* Generated by bench/tools/carrier_hulls.py; edit that, not this. */']
    table = []
    total = 0
    for name in ORDER:
        v, t, m = hull(SHIPS[name])
        total += len(t)
        out.append('static const l3_vec hv_%s[%d] = {%s};' % (name, len(v), ','.join(
            '{%.3ff,%.3ff,%.3ff}' % p for p in v)))
        out.append('static const uint16_t hi_%s[%d] = {%s};' % (name, len(t) * 3, ','.join(
            '%d,%d,%d' % tuple(x) for x in t)))
        out.append('static const uint8_t hm_%s[%d] = {%s};' % (name, len(t), ','.join(str(x) for x in m)))
        table.append('{hv_%s, hi_%s, hm_%s, %d, %d}' % (name, name, name, len(v), len(t)))
        ext = np.array(v)
        print('%-10s %3d verts %3d tris  extent %s' % (name, len(v), len(t),
              np.round(ext.max(axis=0) - ext.min(axis=0), 2)))
    out.append('static const l3_mesh hulls[%d] = {\n    %s};' % (len(ORDER), ',\n    '.join(table)))
    with open(OUT, 'w', newline='\n') as f:
        f.write('\n'.join(out) + '\n')
    print('triangles', total, '->', os.path.normpath(OUT))


if __name__ == '__main__':
    main()
