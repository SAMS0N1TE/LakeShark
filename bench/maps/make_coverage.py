#!/usr/bin/env python3
"""coverage.json for the terminalbay.com maps page, from a map-host deploy tree.

What each state needs (cells/states.json) against what the tree actually holds
(cells/8/<x>/<y>.ctile or its .p0 parts), so the page shows built vs planned
from the same files the boards download.

usage: make_coverage.py DEPLOY_DIR OUT_DIR
writes OUT_DIR/coverage.json and copies places/outlines.lso to OUT_DIR/outlines.lso
"""
import json
import shutil
import sys
import time
from pathlib import Path


def built(tree, x, y):
    base = tree / 'cells' / '8' / str(x)
    return (base / ('%d.ctile' % y)).is_file() or (base / ('%d.ctile.p0' % y)).is_file()


def main():
    tree, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    meta = json.loads((tree / 'cells' / 'states.json').read_text(encoding='utf-8'))
    states, cells = [], {}
    for code, s in sorted(meta.items()):
        if len(code) != 2 or not isinstance(s, dict):
            continue
        need = [tuple(c) for c in s.get('cells', [])]
        have = [c for c in need if built(tree, *c)]
        for c in need:
            cells[c] = cells.get(c, False) or c in have
        states.append({
            'code': code,
            'name': s.get('name', code),
            'base_bytes': s.get('base_bytes', s.get('bytes', 0)),
            'detail_bytes': s.get('detail_bytes', 0),
            'need': len(need),
            'built': len(have),
            'bytes': s.get('bytes', 0),
            'partial': len(s['partial']) if isinstance(s.get('partial'), list) else int(s.get('partial', 0) or 0),
            'bbox': s.get('bbox'),
        })
    doc = {
        'free_max_zoom': meta.get('free_max_zoom'),
        'generated': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
        'states': states,
        # z8 cells as [x, y, built]: the lattice the globe shades.
        'cells': [[x, y, 1 if b else 0] for (x, y), b in sorted(cells.items())],
    }
    (out / 'coverage.json').write_text(json.dumps(doc, separators=(',', ':')), encoding='utf-8')
    src = tree / 'places' / 'outlines.lso'
    if src.is_file():
        shutil.copyfile(src, out / 'outlines.lso')
    nb = sum(1 for c in cells.values() if c)
    print('%d states, %d cells (%d built) -> %s' % (len(states), len(cells), nb, out))


if __name__ == '__main__':
    main()
