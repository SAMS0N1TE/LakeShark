"""A view CartoCore cannot draw: said once, shown on the screen, retried slowly."""
from pathlib import Path
import os, subprocess, sys, tempfile
root=Path(__file__).resolve().parents[2]
exe=Path(sys.argv[1]).resolve();cells=Path(sys.argv[2]).resolve()
env={k:v for k,v in os.environ.items() if not k.startswith('LSSIM_')}
env.update(LSSIM_CELLS=str(cells),LSSIM_CARTOCORE='picture',LSSIM_CARTO_CENTER='43.5,-71.0',
           LSSIM_CARTO_ZOOM='14',LSSIM_CARTO_FAIL_ZOOM='13')
def run(script):
    with tempfile.TemporaryDirectory(prefix='map-failed-') as tmp:
        p=subprocess.run([str(exe),'map','-d','-f','10','-o',str(Path(tmp)/'out.bmp'),'-x',script],
                         cwd=root/'bench',env=env,text=True,capture_output=True,check=True)
    return p.stdout+p.stderr
# z14 draws; ZOOM- to z13, which never does. Over 50 s: one line on the console
# (it was one every 5 s for as long as MAP stayed open) and a notice over the
# old picture, which used to sit there frozen and unexplained.
text=run('15:52,=3000,~4,=20000,~4,=27000,~4')
assert text.count('carto MAP: failed step=picture')==1,text
assert 'TOO MUCH MAP TO DRAW AT THIS ZOOM: ZOOM IN' in text,text
# zooming back in is a new request: it draws and the notice goes
text=run('15:52,=3000,~4,5:52,=3000,~6')
assert 'TOO MUCH MAP' not in text,text
print('PASS: a failed view is reported once, explained on screen and cleared by the next view')
