"""MAP pans steadily: lssim drags the production MAP back and forth with a slow CartoCore worker and
reads its frame trace (LSSIM_MAP_FRAMES, 'carto frames' on a board).
  - the engine never changes during a pan, even while the worker holds its list of cells
  - outlines never stand in for the part of a moved picture that is still rendering
  - detail never goes back on ground already shown, and nothing redraws once the view has settled
usage: test_map_pan.py LSSIM CELLS_FIXTURE"""
from pathlib import Path
import math, os, re, shutil, subprocess, sys, tempfile
root = Path(__file__).resolve().parents[2]
exe = Path(sys.argv[1]).resolve(); cells = Path(sys.argv[2]).resolve()
MF = re.compile(r"^mf (\d+) v=(\w+) lk=(\w) z=(\d+) x=(-?\d+) y=(-?\d+) pic=(\d+) off=(-?\d+) pend=(-?\d+) "
                r"fill=(-?\d+) lost=(-?\d+) lab=(-?\d+) wait=(-?\d+) under=(-?\d+) sw=(.) fl=(\d+)")
KEYS = ("t", "v", "lk", "z", "x", "y", "pic", "off", "pend", "fill", "lost", "lab", "wait", "under", "sw", "fl")
LOST_PM = 150


def run(script, cellsdir, **env_extra):
    env = {k: v for k, v in os.environ.items() if not k.startswith('LSSIM_')}
    env.update(LSSIM_MAP_FRAMES='1', LSSIM_CARTO_SLOW_WORKER='1')
    if cellsdir:
        env['LSSIM_CELLS'] = str(cellsdir)
    env.update({k: str(v) for k, v in env_extra.items()})
    with tempfile.TemporaryDirectory(prefix='map-pan-') as tmp:
        args = [str(exe), 'map', '-f', '6', '-A', '40', '-o', str(Path(tmp) / 'out.bmp'), '-x', script]
        p = subprocess.run(args, cwd=root / 'bench', env=env, text=True, capture_output=True, check=True)
    frames = []
    for line in (p.stdout + p.stderr).splitlines():
        m = MF.match(line)
        if m:
            d = dict(zip(KEYS, m.groups()))
            for k in KEYS:
                if k not in ('v', 'lk', 'sw'):
                    d[k] = int(d[k])
            frames.append(d)
    assert frames, p.stdout[-2000:]
    return frames


def judge(frames, what):
    """After the first frame the map is drawn: no engine change, no outlines over a pending edge, no lost detail,
    no new picture once the view has settled."""
    start = next(i for i, f in enumerate(frames) if f['off'] == 0 and f['pend'] == 0)
    seq = frames[start + 1:]
    flips = [f for f in seq if f['sw'] != '-' or f['v'] != frames[start]['v']]
    edge = [f for f in seq if f['fill'] > 0 and (f['pend'] > 0 or f['off'] > 0)]
    lost = [f for f in seq if f['lost'] >= LOST_PM]
    late, settled = [], None
    for f in seq:
        if f['fl']:
            settled = None
            continue
        if settled is None:
            if f['off'] == 0 and f['pend'] == 0 and f['wait'] == 0:
                settled = f
            continue
        if f['pic'] != settled['pic'] or f['fill'] != settled['fill']:
            late.append(f)
            settled = f
    assert not flips, (what, 'engine changed during pans', flips[:3])
    assert not edge, (what, 'outlines over a pending edge', edge[:3])
    assert not lost, (what, 'detail lost on ground already shown', lost[:3])
    assert not late, (what, 'redrawn after the view settled', late[:3])
    return len(seq)


drag = 'S48:30>2:30,~45,S2:30>48:30,~45'
pans = '~30,' + ','.join([drag] * 3) + ',~30'
# 1. Auto over downloaded cells offshore (no state list says "on the card"), the worker holding its list
#    three frames in four: the card's engine keeps the view through every pan, in each look
for look in (2, 3, 4):
    judge(run(pans, cells, LSSIM_MAP_SOURCE=0, LSSIM_MAP_LOOK=look, LSSIM_MAP_AT='43.1,-69.2', LSSIM_MAP_ZOOM=12),
          'auto look %d' % look)
# 2. the card's engine pinned, each style
for style in ('picture', 'smooth', 'braille'):
    judge(run(pans, cells, LSSIM_CARTOCORE=style, LSSIM_CARTO_ZOOM=12), style)
print('PASS: MAP pans without engine changes, edge outlines, lost detail or late redraws')
