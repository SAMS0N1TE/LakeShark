#!/usr/bin/env python3
"""World country outlines + US state outlines for the TILES globe (LSOUT1).

Input: Natural Earth 1:10m admin-0 and admin-1 GeoJSON (public domain).
Output: one little-endian file the board reads into PSRAM:

  header   8s magic "LSOUT1\\0\\0", u32 regions, u32 rings, u32 points
  regions  [regions] 8s code ("US", "US-NH"), u8 level (0 country, 1 state),
           u8 reserved, i16 west,south,east,north (deg*100)  [58 bytes a row],
           u32 first_ring, u16 ring_count, u16 reserved, 32s name
  rings    [rings]   u32 first_point, u32 point_count
  points   [points]  i16 lon, i16 lat (deg*100)

Rings are simplified (Douglas-Peucker in degrees) and tiny islands dropped,
so the whole world plus the states stays around a few hundred KB.

usage: make_outlines.py ADMIN0.geojson ADMIN1.geojson OUT.lso
"""
import json
import math
import struct
import sys

COUNTRY_TOL, STATE_TOL = 0.08, 0.02      # degrees
COUNTRY_MIN_AREA, STATE_MIN_AREA = 0.05, 0.004  # square degrees


def area(ring):
    a = 0.0
    for i in range(len(ring) - 1):
        a += ring[i][0] * ring[i + 1][1] - ring[i + 1][0] * ring[i][1]
    return abs(a) / 2


def dp(points, tol):
    """Douglas-Peucker, iterative so long coastlines do not recurse deep."""
    if len(points) < 3:
        return points
    keep = [False] * len(points)
    keep[0] = keep[-1] = True
    stack = [(0, len(points) - 1)]
    while stack:
        a, b = stack.pop()
        ax, ay = points[a]
        bx, by = points[b]
        dx, dy = bx - ax, by - ay
        norm = math.hypot(dx, dy) or 1e-12
        best, idx = -1.0, -1
        for i in range(a + 1, b):
            px, py = points[i]
            d = abs(dy * px - dx * py + bx * ay - by * ax) / norm
            if d > best:
                best, idx = d, i
        if best > tol:
            keep[idx] = True
            stack.append((a, idx))
            stack.append((idx, b))
    return [p for p, k in zip(points, keep) if k]


def dp_ring(ring, tol):
    """A closed ring starts and ends on one point, which gives DP a zero-length
    base line; split at the point farthest from the start and simplify halves."""
    if len(ring) < 4:
        return ring
    x0, y0 = ring[0]
    k = max(range(len(ring)), key=lambda i: (ring[i][0] - x0) ** 2 + (ring[i][1] - y0) ** 2)
    if k == 0:
        return ring
    return dp(ring[:k + 1], tol)[:-1] + dp(ring[k:], tol)


def rings_of(geom):
    if geom['type'] == 'Polygon':
        polys = [geom['coordinates']]
    elif geom['type'] == 'MultiPolygon':
        polys = geom['coordinates']
    else:
        return []
    return [poly[0] for poly in polys]   # outer rings only: the globe draws borders


def q(v):
    return max(-32767, min(32767, int(round(v * 100))))


def main():
    a0, a1, out = sys.argv[1:4]
    regions = []  # (code, level, name, rings)

    for f in json.load(open(a0, encoding='utf-8'))['features']:
        p = f['properties']
        iso = p.get('ISO_A2_EH') or p.get('ISO_A2') or ''
        if len(iso) != 2 or iso == '-9':
            iso = (p.get('ADM0_A3') or 'XX')[:2]
        rings = []
        for r in rings_of(f['geometry']):
            if area(r) < COUNTRY_MIN_AREA:
                continue
            s = dp_ring([(x, y) for x, y in r], COUNTRY_TOL)
            if len(s) >= 4:
                rings.append(s)
        if rings:
            regions.append((iso, 0, p.get('NAME', iso), rings))
            if iso == 'PR':  # Puerto Rico is also one of the map's "states"
                regions.append(('US-PR', 1, 'Puerto Rico', rings))

    for f in json.load(open(a1, encoding='utf-8'))['features']:
        p = f['properties']
        if p.get('iso_a2') != 'US':
            continue
        code = p.get('iso_3166_2') or ''
        if not code.startswith('US-'):
            continue
        rings = []
        for r in rings_of(f['geometry']):
            if area(r) < STATE_MIN_AREA:
                continue
            s = dp_ring([(x, y) for x, y in r], STATE_TOL)
            if len(s) >= 4:
                rings.append(s)
        if rings:
            regions.append((code, 1, p.get('name', code), rings))

    ring_rows, points = [], []
    reg_rows = []
    for code, level, name, rings in regions:
        xs = [x for r in rings for x, _ in r]
        ys = [y for r in rings for _, y in r]
        first = len(ring_rows)
        for r in rings:
            ring_rows.append((len(points), len(r)))
            points.extend((q(x), q(y)) for x, y in r)
        reg_rows.append((code, level, name, (q(min(xs)), q(min(ys)), q(max(xs)), q(max(ys))), first, len(rings)))

    with open(out, 'wb') as o:
        o.write(struct.pack('<8sIII', b'LSOUT1\0\0', len(reg_rows), len(ring_rows), len(points)))
        for code, level, name, bb, first, n in reg_rows:
            o.write(struct.pack('<8sBB4hIHH32s', code.encode()[:8], level, 0, *bb, first, n, 0,
                                name.encode('utf-8')[:31]))
        for first, n in ring_rows:
            o.write(struct.pack('<II', first, n))
        for x, y in points:
            o.write(struct.pack('<hh', x, y))
    states = sum(1 for r in reg_rows if r[1] == 1)
    print('%d regions (%d states), %d rings, %d points, %d bytes' %
          (len(reg_rows), states, len(ring_rows), len(points),
           20 + len(reg_rows) * 58 + len(ring_rows) * 8 + len(points) * 4))


if __name__ == '__main__':
    main()
