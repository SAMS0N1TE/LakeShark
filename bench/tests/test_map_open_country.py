"""Bare ground over a downloaded cell is open country, not "NO TILES HERE"."""
from pathlib import Path
import os, subprocess, sys, tempfile
root=Path(__file__).resolve().parents[2]
exe=Path(sys.argv[1]).resolve();cells=Path(sys.argv[2]).resolve()
def run(centre):
    env={k:v for k,v in os.environ.items() if not k.startswith('LSSIM_')}
    env.update(LSSIM_CELLS=str(cells),LSSIM_CARTOCORE='picture',LSSIM_CARTO_CENTER=centre,LSSIM_CARTO_ZOOM='15')
    with tempfile.TemporaryDirectory(prefix='map-country-') as tmp:
        p=subprocess.run([str(exe),'map','-d','-f','14','-o',str(Path(tmp)/'out.bmp')],
                         cwd=root/'bench',env=env,text=True,capture_output=True,check=True)
    return p.stdout+p.stderr
# inside cell 8-77-93 of the fixture, where it has nothing to draw at z15
text=run('43.5,-71.0')
assert 'carto open /sdcard/maps/cells/' in text and 'MARK HERE 43.50000,-71.00000' in text,text
assert 'NO TILES HERE' not in text,text
# one cell south, which the fixture does not hold: still a missing map
text=run('42.5,-71.0')
assert 'NO TILES HERE' in text,text
print('PASS: empty ground in a downloaded cell is not reported as a missing map')
