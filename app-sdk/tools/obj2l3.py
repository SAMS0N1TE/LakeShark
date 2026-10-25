"""Turn an OBJ model (Blender: File > Export > Wavefront, Y up) into C for lap3d.

    python app-sdk/tools/obj2l3.py ship.obj -o apps/myapp/ship_model.c --name ship --size 2

writes `const l3_mesh ship` plus its arrays. Faces with more than three corners
are split into triangles. Each `usemtl` becomes a material index, in the order
they first appear; the file lists them, so the app can make a material for
each:  m[0] = l3_material(...); ... then draw with mats mapped through m[].
--size scales the model so its longest side is that many world units, and
--center moves its middle to the origin.
"""
import argparse
import sys


def load(path):
    verts, tris, mats, names = [], [], [], []
    current = 0
    for line in open(path, encoding='utf-8', errors='replace'):
        part = line.split()
        if not part:
            continue
        if part[0] == 'v' and len(part) >= 4:
            verts.append(tuple(float(c) for c in part[1:4]))
        elif part[0] == 'usemtl':
            name = ' '.join(part[1:])
            if name not in names:
                names.append(name)
            current = names.index(name)
        elif part[0] == 'f' and len(part) >= 4:
            idx = []
            for corner in part[1:]:
                i = int(corner.split('/')[0])
                idx.append(i - 1 if i > 0 else len(verts) + i)
            for k in range(1, len(idx) - 1):        # a fan: (0, k, k+1)
                tris.append((idx[0], idx[k], idx[k + 1]))
                mats.append(current)
    return verts, tris, mats, names


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('obj')
    ap.add_argument('-o', '--out', required=True)
    ap.add_argument('--name', default='model')
    ap.add_argument('--size', type=float, default=0, help='longest side in world units (0 keeps the file\'s units)')
    ap.add_argument('--center', action='store_true')
    a = ap.parse_args()

    verts, tris, mats, names = load(a.obj)
    if not verts or not tris:
        sys.exit('%s: no vertices or faces' % a.obj)
    if len(verts) > 65535:
        sys.exit('%s: %d vertices; lap3d meshes take up to 65,535' % (a.obj, len(verts)))
    lo = [min(v[k] for v in verts) for k in range(3)]
    hi = [max(v[k] for v in verts) for k in range(3)]
    mid = [(lo[k] + hi[k]) / 2 if a.center else 0 for k in range(3)]
    span = max(hi[k] - lo[k] for k in range(3)) or 1
    s = a.size / span if a.size else 1
    verts = [tuple((v[k] - mid[k]) * s for k in range(3)) for v in verts]

    n = a.name
    out = ['/* %s: made by app-sdk/tools/obj2l3.py from %s. */' % (n, a.obj.replace('\\', '/').split('/')[-1]),
           '#include "lap3d.h"', '',
           '/* Materials, by index: %s */' % (', '.join('%d %s' % (i, m) for i, m in enumerate(names)) or 'none'),
           'static const l3_vec %s_v[%d] = {' % (n, len(verts))]
    for v in verts:
        out.append('    { %.5ff, %.5ff, %.5ff },' % v)
    out.append('};')
    out.append('static const uint16_t %s_idx[%d] = {' % (n, len(tris) * 3))
    for k in range(0, len(tris), 6):
        out.append('    ' + ' '.join('%d, %d, %d,' % t for t in tris[k:k + 6]))
    out.append('};')
    mats_ref = 'NULL'
    if len(names) > 1:
        out.append('static const uint8_t %s_mats[%d] = {' % (n, len(mats)))
        for k in range(0, len(mats), 24):
            out.append('    ' + ', '.join(str(m) for m in mats[k:k + 24]) + ',')
        out.append('};')
        mats_ref = '%s_mats' % n
    out.append('const l3_mesh %s = { %s_v, %s_idx, %s, %d, %d };' % (n, n, n, mats_ref, len(verts), len(tris)))
    open(a.out, 'w', encoding='utf-8', newline='\n').write('\n'.join(out) + '\n')
    print('%s: %d vertices, %d triangles, %d materials -> %s' % (n, len(verts), len(tris), len(names), a.out))


if __name__ == '__main__':
    main()
