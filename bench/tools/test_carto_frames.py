"""Every-presented-frame zoom/pan regression, including a busy worker and poisoned failure."""
from pathlib import Path
import os,subprocess,struct
from PIL import Image
root=Path(__file__).resolve().parents[2]
core=Path(os.getenv('CARTOCORE_DIR',root.parent/'CartoCore-wip'))
out=root/'bench/out/map-visual/cartomap-tests';exe=root/'bench/build/lssim.exe'
script=','.join(['+=,~12']*5+['+-,~12']*5+['@right,~12','@down,~12','@left,~12','@up,~12'])
for mode in ('smooth','braille'):
    for layout in ('portrait','landscape'):
        dest=out/f'frames-{mode}-{layout}';dest.mkdir(parents=True,exist_ok=True)
        env=dict(os.environ,LSSIM_CARTOCORE=mode,LSSIM_CTILE=str(core/'data/franklin_compact.ctile'),
                 LSSIM_CARTO_CENTER='43.45,-71.65',LSSIM_CARTO_ZOOM='11',LSSIM_CAPTURE_FRAMES=str(dest),
                 LSSIM_CARTO_SLOW_WORKER='1',LSSIM_CARTO_FAIL_FRAME='1',LSSIM_CARTO_CHECK_COMPLETE='1')
        args=[str(exe),'map','-e','-f','16','-A','500','-x',script,'-o',str(dest/'final.bmp')]
        if layout=='landscape':args+=['-l']
        p=subprocess.run(args,cwd=root,env=env,capture_output=True,text=True,check=True)
        assert 'injected incomplete render' in p.stdout,p.stdout
        (dest/'run.txt').write_text(p.stdout+p.stderr)
        previous=None;zooms=set();held=0;complete=0
        for file in sorted(dest.glob('*.cells')):
            header,raw=file.read_bytes().split(b'\n',1)
            ready,fresh,z,generation,x,y,w,h=map(int,header.split())
            if ready:
                assert len(raw)==w*h*3
                cells=list(struct.iter_unpack('<HB',raw))
                assert all(c!=0xdead for c,a in cells),file
                assert len(set(cells))>8,(file,'blank/incomplete map')
                assert sum(c not in (0,32) for c,a in cells)>w,(file,'blank map')
                if not fresh:
                    assert previous is not None and raw==previous,(file,'pending map changed/partially presented')
                    held+=1
                else:zooms.add(z);complete+=1
                previous=raw
            else:assert previous is None,(file,'opening placeholder after first complete frame')
            bmp=file.with_suffix('.bmp');im=Image.open(bmp).convert('RGB')
            if layout=='landscape':im=im.transpose(Image.Transpose.ROTATE_270)
            im.save(bmp.with_suffix('.png'));bmp.unlink()
        (dest/'final.bmp').unlink()
        assert zooms==set(range(11,17)),zooms
        assert held>30 and complete>10,(held,complete)
        print(f'PASS {mode}/{layout}: {complete} complete, {held} unchanged pending presentations, all z11..16; poisoned render hidden')
# Identical views to the pre-change renders.
for mode in ('smooth','braille'):
    for z in (12,14,16):
        bmp=out/f'lines-after-{mode}-z{z}.bmp'
        env=dict(os.environ,LSSIM_CARTOCORE=mode,LSSIM_CTILE=str(core/'data/franklin_compact.ctile'),
                 LSSIM_CARTO_CENTER='43.45,-71.65',LSSIM_CARTO_ZOOM=str(z))
        subprocess.run([str(exe),'map','-e','-f','8','-o',str(bmp)],cwd=root,env=env,check=True,stdout=subprocess.DEVNULL)
        Image.open(bmp).convert('RGB').save(bmp.with_suffix('.png'));bmp.unlink()

# Console uses the same publisher; cover SD and deferred embedded cell-cache fills.
for source in ('sd','embedded'):
    env=dict(os.environ,LSSIM_CARTO_CONSOLE_TEST='1',LSSIM_CARTO_FAIL_FRAME='1',LSSIM_CARTO_CHECK_COMPLETE='1')
    env.pop('LSSIM_CTILE',None)
    if source=='sd':env['LSSIM_CTILE']=str(core/'data/franklin_compact.ctile')
    p=subprocess.run([str(exe),'home','-f','8','-o',str(out/'console-test.bmp')],cwd=root,env=env,capture_output=True,text=True,check=True)
    assert 'PASS: carto show zoom/pan' in p.stdout,p.stdout
    (out/f'frames-console-{source}.txt').write_text(p.stdout+p.stderr)
    (out/'console-test.bmp').unlink()
    print(f'PASS console {source}: complete publication, busy locks and failed render retention')
