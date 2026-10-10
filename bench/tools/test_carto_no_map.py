"""Firmware no-map behavior via the shared renderer, with host fixture disabled."""
from pathlib import Path
import os, shutil, subprocess, tempfile
from PIL import Image
root=Path(__file__).resolve().parents[2]
out=root/'bench/out/no-map';out.mkdir(parents=True,exist_ok=True)
with tempfile.TemporaryDirectory(dir=root/'bench/build',prefix='no-map-') as tmp:
    sd=Path(tmp)/'maps';sd.mkdir()
    env=dict(os.environ,LSSIM_CARTO_DIR=str(sd),LSSIM_CARTO_NO_EMBEDDED='1',LSSIM_CARTO_NO_MAP_TEST='1')
    for name,app,script,extra in [
        ('smooth','map','',{'LSSIM_CARTOCORE':'smooth'}),
        ('braille','map','',{'LSSIM_CARTOCORE':'braille'}),
        ('missing-saved','map','',{'LSSIM_CARTOCORE':'smooth','LSSIM_CTILE':str(sd/'missing.ctile')}),
        ('tiles','tiles','+s,@f2',{})]:
        args=[str(root/'bench/build/lssim.exe'),app,'-f','8','-d','-o',str(out/(name+'.bmp'))]
        if script:args+=['-x',script]
        p=subprocess.run(args,cwd=root,env=dict(env,**extra),capture_output=True,text=True,timeout=90)
        log=p.stdout+p.stderr;(out/(name+'.log')).write_text(log)
        assert p.returncode==0,(name,log[-2000:])
        assert 'PASS: no-map console and missing SD bench fixture' in log
        assert ('No downloaded places yet' if app=='tiles' else 'No map installed') in log and 'Download a region in TILES > SEARCH' in log,(name,log)
        Image.open(out/(name+'.bmp')).save(out/(name+'.png'))
    shutil.copyfile(root/'data/franklin_mini.ctile',sd/'franklin_mini.ctile')
    env.pop('LSSIM_CARTO_NO_MAP_TEST');env['LSSIM_CARTO_SD_BENCH_TEST']='1'
    p=subprocess.run([str(root/'bench/build/lssim.exe'),'map','-f','2'],cwd=root,env=env,capture_output=True,text=True,timeout=90)
    (out/'sd-bench.log').write_text(p.stdout+p.stderr)
    assert p.returncode==0 and 'PASS: bench loads SD fixture' in p.stdout,p.stdout+p.stderr
print('PASS: empty SD, missing saved map, both fills, TILES empty library, console and SD bench')
