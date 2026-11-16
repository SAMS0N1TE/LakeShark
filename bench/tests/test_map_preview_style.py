"""ADS-B's mini map draws with the saved MAP STYLE without MAP being opened first."""
from pathlib import Path
import os, subprocess, sys, tempfile
root=Path(__file__).resolve().parents[2]
exe=Path(sys.argv[1]).resolve()
def run(**extra):
    env={k:v for k,v in os.environ.items() if not k.startswith('LSSIM_')}
    env.update(extra)
    with tempfile.TemporaryDirectory(prefix='map-preview-') as tmp:
        p=subprocess.run([str(exe),'adsb','-d','-f','10','-o',str(Path(tmp)/'out.bmp')],
                         cwd=root/'bench',env=env,text=True,capture_output=True,check=True)
    return p.stdout+p.stderr
for style in ('smooth','braille','picture'):
    text=run(LSSIM_CARTOCORE=style)
    assert 'MINI MAP' in text and 'carto arenas' in text,(style,text)
# the picture styles still draw the picture engine
text=run()
assert 'MINI MAP' in text and 'carto arenas' not in text,text
print('PASS: the mini map follows the saved CartoCore style on its own')
# SET HOME > "Save map center as home" under a CartoCore style: there is no
# PMTiles archive open then, and it used to answer NO MAP OPEN
env={k:v for k,v in os.environ.items() if not k.startswith('LSSIM_')}
env.update(LSSIM_CARTOCORE='picture',LSSIM_CARTO_CENTER='43.45,-71.64')
with tempfile.TemporaryDirectory(prefix='map-home-') as tmp:
    p=subprocess.run([str(exe),'map','-d','-f','6','-x','8:66,~2,10:35,~2','-o',str(Path(tmp)/'out.bmp')],
                     cwd=root/'bench',env=env,text=True,capture_output=True,check=True)
assert 'HOME SAVED' in p.stdout and '43.45000, -71.64000' in p.stdout and 'NO MAP OPEN' not in p.stdout,p.stdout
print('PASS: the map centre saves as home under CartoCore')
