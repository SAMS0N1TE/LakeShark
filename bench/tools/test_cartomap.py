"""Render the production MAP screen and shared CartoCore path in the real lssim.
Run after configuring bench with -DCARTOCORE_DIR=... and building lssim.
LSSIM_CTILE optionally selects a terrain-enabled map; default is embedded mini.
"""
from pathlib import Path
import os, subprocess
from PIL import Image, ImageChops
root=Path(__file__).resolve().parents[2]
out=root/'bench/out/map-visual/cartomap-tests';out.mkdir(parents=True,exist_ok=True)
exe=root/'bench/build/lssim.exe'
for mode in ('smooth','braille'):
    for layout in ('portrait','landscape'):
        base=None
        for scenario,extra in (
            ('',[]),('pan',['-K','right']),('zoom',['-k','-']),
            ('touch',['-x','18:18,~3']),
            ('tracks',['-g',str(root/'bench/fixtures/route.gpx')])):
            name=f'{mode}-{layout}'+(f'-{scenario}' if scenario else '')
            env=dict(os.environ,LSSIM_CARTOCORE=mode)
            if scenario=='tracks':env['LSSIM_RS41']='1'
            bmp=out/f'{name}.bmp'
            args=[str(exe),'map','-o',str(bmp),'-f','8','-d']
            if layout=='landscape':args+=['-l']
            p=subprocess.run(args+extra,cwd=root,env=env,capture_output=True,text=True,check=True)
            (out/f'{name}.txt').write_text(p.stdout)
            assert f'CartoCore {mode}' in p.stdout,p.stdout
            assert 'CartoCore map unavailable' not in p.stdout,p.stdout
            im=Image.open(bmp).convert('RGB')
            if layout=='landscape':im=im.transpose(Image.Transpose.ROTATE_270)
            im.save(out/f'{name}.png');bmp.unlink()
            if not scenario:base=im.copy()
            elif scenario in ('pan','zoom','touch'):
                assert ImageChops.difference(base,im).getbbox(),f'{name}: input did not change the image'
            if scenario=='zoom':assert 'z13' in p.stdout,p.stdout
        # Leaving MAP invokes the production shared lifetime cleanup.
        p=subprocess.run([str(exe),'map','-K','esc','-f','3','-d','-o',str(out/'leave.bmp')],
                         cwd=root,env=dict(os.environ,LSSIM_CARTOCORE=mode),capture_output=True,text=True,check=True)
        assert 'MAP leave released source and arenas on worker' in p.stdout,p.stdout
        (out/'leave.bmp').unlink()
print('PASS: MAP smooth/braille, portrait/landscape, pan, zoom, touch, GPX/RS41 tracks, leave; PNGs in '+str(out))
