"""Real MAP/cellset renders under a board-sized PSRAM allocation limit."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import sys
import math

exe = Path(sys.argv[1]).resolve()
out = exe.parent / 'carto-psram'
out.mkdir(exist_ok=True)
engine = Path(os.environ['CARTOCORE_DIR'])
cells = Path(os.environ.get('CARTOCORE_CELLS', engine.parent / 'CartoCore/data/cells-nh'))
if (cells / 'CURRENT').exists():
    cells /= (cells / 'CURRENT').read_text().strip()
region = Path(os.environ['CARTOCORE_REGION'])
base = {k: v for k, v in os.environ.items() if not k.startswith('LSSIM_')}
base.update(LSSIM_CARTO_NO_EMBEDDED='1', LSSIM_CARTO_PSRAM_FREE='20890868',
            LSSIM_CELLS_MEMORY='1', LSSIM_CARTO_CHECK_COMPLETE='1')
peaks, remaining = [], []
cases=0
cell_totals=[]
single_totals=[]

def run(name, extra, wide=False, script='', success=True):
    global cases
    cases+=1
    env = dict(base, **extra)
    args = [str(exe), 'map', '-f', '8', '-d', '-o', str(out / (name + '.bmp'))]
    if wide:
        args += ['-l']
    if script:
        args += ['-x', script]
    p = subprocess.run(args, env=env, capture_output=True, text=True, timeout=90)
    log = p.stdout + p.stderr
    (out / (name + '.log')).write_text(log)
    assert p.returncode == 0, (name, log[-2000:])
    if success:
        if 'LSSIM_CARTO_EXHAUST_FRAME' not in extra:
            assert 'failed step=' not in log, (name, log)
        matches = re.findall(r'cells render: arena peak=(\d+)/(\d+).*free_psram=(\d+)', log)
        assert matches, (name, log)
        for peak, capacity, free in matches:
            if 'LSSIM_CTILE' not in extra:
                assert int(capacity) <= 19 * 256 * 1024, (name, capacity)
                assert 'decoded=1572864 cache=0 idle=0' in log, (name, log)
            # Complete source, renderer, handles, and both publication buffers.
            assert 20890868 - int(free) <= 6.5 * 1024 * 1024, (name, free)
            assert int(free) >= 3 * 1024 * 1024, (name, free)
            (single_totals if 'LSSIM_CTILE' in extra else cell_totals).append(20890868-int(free))
            peaks.append(int(peak)); remaining.append(int(free))
    else:
        assert 'Not enough memory for map' in log, (name, log)
        assert 'Download a region in TILES' not in log, (name, log)
    return log

for wide in (False, True):
    for fill in ('smooth', 'braille'):
        for z in (8, 12, 14, 16):
            run(f'cells-{wide}-{fill}-z{z}', dict(LSSIM_CELLS=str(cells),
                LSSIM_CARTOCORE=fill, LSSIM_CARTO_ZOOM=str(z),
                LSSIM_CARTO_CENTER='43.45,-71.71875'), wide)
run('cells-pan-zoom', dict(LSSIM_CELLS=str(cells), LSSIM_CARTOCORE='smooth',
    LSSIM_CARTO_ZOOM='12'), True, '@left,~3,@right,~3,@up,~3,10:25,~3,30:25,~3')
run('cells-touch-tracks', dict(LSSIM_CELLS=str(cells), LSSIM_CARTOCORE='smooth',
    LSSIM_CARTO_ZOOM='14', LSSIM_RS41='1'), True, '50:15,~3,10:25,~3,30:25,~3')
run('cells-console', dict(LSSIM_CELLS=str(cells), LSSIM_CARTOCORE='smooth',
    LSSIM_CARTO_CONSOLE_TEST='1', LSSIM_CARTO_ZOOM='14'))
log=run('cells-exhaustion', dict(LSSIM_CELLS=str(cells), LSSIM_CARTOCORE='smooth',
    LSSIM_CARTO_CONSOLE_TEST='1', LSSIM_CARTO_EXHAUST_FRAME='1'))
assert 'real arena exhaustion for one frame' in log and 'previous view retained' in log
assert 'poisoned failed frame retain complete cells' in log
assert re.search(r'carto hw cold=\d+ decoded=\d+ idle=\d+ hot=\d+.* reset',log)
# z8 cell edges surrounding the 17,278,861-byte 8-77-93 cell. Exercise
# both sides of longitude/latitude borders and pan along each in both layouts.
assert (cells / '8-77-93.ctile').stat().st_size > 17_000_000
for wide in (False,True):
    for z in range(8,17):
        for x,y in ((77,93),(78,93),(77,94),(78,94)):
            lon=x/256*360-180
            lat=math.degrees(math.atan(math.sinh(math.pi*(1-2*y/256))))
            run(f'sweep-{wide}-z{z}-{x}-{y}',dict(LSSIM_CELLS=str(cells),
                LSSIM_CARTOCORE='smooth',LSSIM_CARTO_ZOOM=str(z),
                LSSIM_CARTO_CENTER=f'{lat},{lon}'),wide,
                '@left,~3,@right,~3,@right,~3,@down,~3,@up,~3')
missing = out / 'missing'
(missing / 'cells').mkdir(parents=True, exist_ok=True)
for name in ('index.cci', 'overview.ctile', '8-77-93.ctile'):
    shutil.copyfile(cells / name, missing / 'cells' / name)
run('cells-missing-neighbor', dict(LSSIM_CARTO_DIR=str(missing), LSSIM_CARTOCORE='braille',
    LSSIM_CARTO_ZOOM='12', LSSIM_CARTO_CENTER='43.45,-71.71875'), True)
run('cells-low-memory', dict(LSSIM_CELLS=str(cells), LSSIM_CARTOCORE='smooth',
    LSSIM_CARTO_PSRAM_FREE=str(6 * 1024 * 1024)), success=False)
run('cells-fragmented', dict(LSSIM_CELLS=str(cells), LSSIM_CARTOCORE='smooth',
    LSSIM_CARTO_PSRAM_LARGEST=str(5 * 1024 * 1024), LSSIM_CARTO_ZOOM='14'))
for wide in (False, True):
    log = run(f'single-{wide}', dict(LSSIM_CTILE=str(region), LSSIM_CARTOCORE='smooth',
        LSSIM_CARTO_ZOOM='14'), wide)
    assert 'cold=2097152 decoded=2097152 cache=0 idle=2097152' in log
print(f'PASS: {cases} real MAP cases, complete-frame comparisons, low memory, fragmentation; '
      f'max cold peak={max(peaks)}; minimum free PSRAM={min(remaining)}; '
      f'cell total={max(cell_totals)}; single total={max(single_totals)}')
# Expanded border views can hit the cap and must publish complete coarser frames.
assert any('coarse=1' in p.read_text() for p in out.glob('sweep-*.log'))
