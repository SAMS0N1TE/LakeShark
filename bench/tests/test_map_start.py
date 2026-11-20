"""Where MAP opens after a boot: the view it was left at, else the saved home."""
from pathlib import Path
import os, subprocess, sys, tempfile
root=Path(__file__).resolve().parents[2]
exe=Path(sys.argv[1]).resolve()
HOME='-36.6734,-60.1133'
def run(script='',**extra):
    env={k:v for k,v in os.environ.items() if not k.startswith('LSSIM_')}
    env.update(extra)
    with tempfile.TemporaryDirectory(prefix='map-start-') as tmp:
        args=[str(exe),'map','-d','-f','8','-o',str(Path(tmp)/'out.bmp')]
        if script: args+=['-x',script]
        p=subprocess.run(args,cwd=root/'bench',env=env,text=True,capture_output=True,check=True)
    return p.stdout+p.stderr
# never placed, home set: opens at home
text=run(LSSIM_MAP_START='1',LSSIM_HOME=HOME)
assert 'MARK HERE -36.67340,-60.11330' in text,text
# the view it was left at wins over home, zoom included
text=run(LSSIM_MAP_START='1',LSSIM_HOME=HOME,LSSIM_MAP_SAVED='-34.6,-58.38,13')
assert 'MARK HERE -34.60000,-58.38000' in text,text
# something placed the map before MAP was opened (a node's MAP button): that stands
text=run(LSSIM_HOME=HOME,LSSIM_MAP_SAVED='-34.6,-58.38,13')
assert 'MARK HERE 43.44450,-71.64730' in text,text
# a pan is kept once the view has stood still, and not before
text=run('@left,=1000,~2',LSSIM_MAP_START='1',LSSIM_HOME=HOME)
assert 'map view saved' not in text,text
text=run('@left,=4000,~2',LSSIM_MAP_START='1',LSSIM_HOME=HOME)
assert text.count('map view saved')==1 and 'map view saved -36.67340,-60.17' in text,text
# opening alone saves nothing
text=run('=5000,~2',LSSIM_MAP_START='1',LSSIM_HOME=HOME)
assert 'map view saved' not in text,text
# CartoCore with no map under home: the view stays at home and says so,
# instead of moving to whatever map file the card has
text=run(LSSIM_MAP_START='1',LSSIM_HOME=HOME,LSSIM_CARTOCORE='picture')
assert 'MARK HERE -36.67340,-60.11330' in text and 'carto MAP: recentred' not in text,text
assert 'NO TILES HERE' in text,text
print('PASS: MAP opens at the kept view, else home; placed views stand; pans are kept; no recentring onto another map')
# --- the mini maps borrow the map; their framing is never MAP's view ---
def adsb(script,**extra):
    env={k:v for k,v in os.environ.items() if not k.startswith('LSSIM_')}
    env.update(LSSIM_MAP_START='1',LSSIM_HOME=HOME,**extra)
    with tempfile.TemporaryDirectory(prefix='map-borrow-') as tmp:
        p=subprocess.run([str(exe),'adsb','-d','-f','6','-o',str(Path(tmp)/'out.bmp'),'-x',script],
                         cwd=root/'bench',env=env,text=True,capture_output=True,check=True)
    return p.stdout+p.stderr
TO_MAP='5:3,~3,18:9,~3,23:34,~4,23:34,~30,=600,~6'      # HOME, FIELD, MAP (group, app)
TO_ADSB='5:3,~3,17:8,~3,5:9,~3,40:20,~4,40:20,~12'      # HOME, BACK, RADIO, ADSB
# ADS-B first, then MAP from the launcher: MAP is where it was left, and that is not saved again
text=adsb(TO_MAP+',=5000,~2',LSSIM_MAP_SAVED='-34.6,-58.38,13')
assert 'MARK HERE -34.60000,-58.38000' in text and 'map view saved' not in text,text
# ... or at home, at MAP's zoom rather than ADS-B's z8
text=adsb(TO_MAP+',=5000,~2')
assert 'MARK HERE -36.67340,-60.11330' in text and 'map view saved' not in text,text
# FULL MAP centres on home and opens MAP: it lands there, and is not kept until MAP moves
text=adsb('+m,~30,=5000,~2',LSSIM_MAP_SAVED='-34.6,-58.38,13')
assert 'MARK HERE -36.67340,-60.11330' in text and 'map view saved' not in text,text
text=adsb('+m,~30,@left,=4000,~2',LSSIM_MAP_SAVED='-34.6,-58.38,13')
assert text.count('map view saved')==1,text
# MAP, zoomed in; ADS-B at its own zoom; MAP again: MAP's own view, which the
# next ZOOM+ steps on from; each kept once
text=adsb(TO_MAP+',5:52,=4000,~2,'+TO_ADSB+',=5000,~2,'+TO_MAP+',=5000,~2,5:52,=4000,~2',LSSIM_MAP_SAVED='-34.6,-58.38,13')
assert 'MARK HERE -34.60000,-58.38000' in text,text
saves=[l for l in text.splitlines() if 'map view saved' in l]
assert len(saves)==2 and saves[0].endswith('-58.38000 z14') and saves[1].endswith('-58.38000 z15'),saves
print('PASS: mini map framing is not MAP\'s view; FULL MAP lands; only moves made in MAP are kept')
