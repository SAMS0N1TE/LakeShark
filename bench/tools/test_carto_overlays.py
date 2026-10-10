"""LR2021 label/overlay regression: exact centre, z12/13, rings, both fills."""
from pathlib import Path
import os,subprocess,re
from PIL import Image
root=Path(__file__).resolve().parents[2];out=root/'bench/out/map-visual/cartomap-tests';out.mkdir(parents=True,exist_ok=True)
core=Path(os.getenv('CARTOCORE_DIR',root.parent/'CartoCore-wip'))
masked=0
for mode in ('smooth','braille'):
    for layout in ('portrait','landscape'):
        for z in (12,13):
            env=dict(os.environ,LSSIM_CARTOCORE=mode,LSSIM_CTILE=os.getenv('LSSIM_CTILE',str(core/'data/franklin_compact.ctile')),
                     LSSIM_CARTO_CENTER='43.45,-71.65',LSSIM_CARTO_ZOOM=str(z))
            for home in (False,True):
                name=f'overlay-labels-{mode}-{layout}-z{z}'+('-home' if home else '');bmp=out/(name+'.bmp')
                args=[str(root/'bench/build/lssim.exe'),'map','-f','8','-d','-o',str(bmp)]
                if home:args+=['-e']
                if layout=='landscape':args+=['-l']
                p=subprocess.run(args,cwd=root,env=env,capture_output=True,text=True,check=True)
                text=p.stdout+p.stderr
                assert re.search(r'(0\.25|0\.5|1|1\.5)nm',text) and f'z{z} ' in text,text
                assert 'CartoCore label audit: 0 broken cells/plates' in text,text
                m=re.search(r'label audit: (\d+) labels, (\d+) overlaps',text)
                assert m and int(m[1])>0 and int(m[2])==0,text
                masked+=int(re.search(r'priority: (\d+) low-priority',text)[1])
                # Minor names now share a quota and shorten at word boundaries.
                # Keep at least one real water name, plus the strict
                # complete-plate and collision audits above.
                assert any(name in text for name in ('Winnipesaukee','Pemigewasset','Cate Brook')),text
                if home:assert 'HOME' in text and 'GPS WAIT' in text,text
                (out/(name+'.txt')).write_text('\n'.join(line.rstrip() for line in text.splitlines())+'\n')
                im=Image.open(bmp).convert('RGB')
                if layout=='landscape':im=im.transpose(Image.Transpose.ROTATE_270)
                im.save(out/(name+'.png'));bmp.unlink()
assert masked==0,'basemap label plates must never erase ring/track dots'
print('PASS: 16 exact-view renders (live GPS/air/marks and HOME/GPS-wait), complete labels/solid plates, zero marker/label overlaps, rings retained')
