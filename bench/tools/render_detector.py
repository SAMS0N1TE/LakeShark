"""Reproducible TFT evidence; production UI, deterministic host observations."""
import os, subprocess, sys
from pathlib import Path
from PIL import Image, ImageOps, ImageDraw
root=Path(__file__).resolve().parents[2]
out=root/'bench/evidence/detector-polish'/sys.argv[1]
if sys.argv[1]=='before' and any(out.glob('*.png')):
    raise SystemExit('Baseline already archived; refusing to replace before images')
out.mkdir(parents=True,exist_ok=True)
cases={'idle':'','active':'','detail':'+d','options':'+o',
       'sources':'+o,@enter','rules':'+o,@down,@enter',
       'alerts':'+o,@down,@down,@enter','logging':'+o,@down,@down,@down,@enter',
       'sinks-test':'+d,@esc','banner':''}
for orientation in ('portrait','landscape'):
    thumbs=[]
    for name,script in cases.items():
        env=os.environ.copy();env['LSSIM_DETECTOR']='1'
        if name=='idle':env['LSSIM_DETECTOR_IDLE']='1'
        if name in ('sinks-test','banner'):env['LSSIM_DETECTOR_TEST']='1'
        if name=='banner':env['LSSIM_DETECTOR_BANNER']='1'
        stem=out/f'{name}-{orientation}'
        cmd=[str(root/'bench/build/lssim.exe'),'detector','--tft','-o',str(stem.with_suffix('.bmp')),'-d']
        if orientation=='landscape':cmd+=['-l']
        if script:cmd+=['-x',script]
        result=subprocess.run(cmd,cwd=root,env=env,capture_output=True,text=True,check=True)
        stem.with_suffix('.txt').write_text(result.stdout)
        im=Image.open(stem.with_suffix('.bmp'))
        if orientation=='landscape':im=im.transpose(Image.Transpose.ROTATE_270)
        im.save(stem.with_suffix('.png'))
        stem.with_suffix('.bmp').unlink()
        tw,th=(600,310) if orientation=='landscape' else (290,615)
        im.thumbnail((tw-20,th-30)); tile=Image.new('RGB',(tw,th),'#20242b')
        tile.paste(im,(10,25));ImageDraw.Draw(tile).text((10,5),name,fill='white');thumbs.append(tile)
    nc=2 if orientation=='landscape' else 5
    sheet=Image.new('RGB',(tw*nc,th*((len(thumbs)+nc-1)//nc)),'#20242b')
    for i,im in enumerate(thumbs):sheet.paste(im,((i%nc)*tw,(i//nc)*th))
    sheet.save(out/f'contact-{orientation}.png')
# Additional pages in the shared tree, including exact numeric entry.
if sys.argv[1]=='after':
    for orientation in ('portrait','landscape'):
        for name,script in {'rules-tail':'+o,@down,@enter,'+','.join(['@down']*8),
                            'cooldown-entry':'+o,@down,@enter,@down,@down,@enter',
                            'touch-test':('43:64' if orientation=='portrait' else '105:26')+',~12,@esc'}.items():
            env=os.environ.copy();env['LSSIM_DETECTOR']='1'
            stem=out/f'{name}-{orientation}'
            cmd=[str(root/'bench/build/lssim.exe'),'detector','--tft','-A','40','-o',str(stem.with_suffix('.bmp')),'-d','-x',script]
            if orientation=='landscape':cmd+=['-l']
            result=subprocess.run(cmd,cwd=root,env=env,capture_output=True,text=True,check=True)
            stem.with_suffix('.txt').write_text(result.stdout)
            im=Image.open(stem.with_suffix('.bmp'))
            if orientation=='landscape':im=im.transpose(Image.Transpose.ROTATE_270)
            im.save(stem.with_suffix('.png'));stem.with_suffix('.bmp').unlink()
print(out)
