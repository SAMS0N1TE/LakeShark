"""Read-only C6/P4 lssim matrix. The C6 action is a logging stub, never OTA."""
import os, re, subprocess, sys
from pathlib import Path
from PIL import Image, ImageOps, ImageDraw
ROOT=Path(__file__).resolve().parents[1]
phase=sys.argv[1]
out=ROOT/'bench/evidence/c6-polish'/phase
out.mkdir(parents=True, exist_ok=True)
exe=ROOT/'bench/build/lssim.exe'
states=[('idle',-1,0,0,''),('validating',0,0,1,'Validating SD image'),('stopping',1,0,1,'Stopping Wi-Fi/BLE; kept off until P4 reboot'),('preparing',2,0,1,'Preparing inactive C6 slot'),('transferring',3,46,1,'Writing inactive C6 slot'),('verifying',4,100,1,'C6 validating written image'),('activating',5,100,1,'Activating C6 image'),('reconnecting',6,100,1,'Waiting for a new C6 boot (45 seconds)'),('done',7,100,1,'2.12.8 -> 2.12.9; verified. Reboot P4 to use radios')]
errors=[]
for src in ['main/ls_c6_core.c','main/ls_c6_update.c']:
    text=(ROOT/src).read_text()
    errors+=re.findall(r'(?:return |preflight_failure\()"([^"\n]+)"',text)
    errors+=re.findall(r'fail\(io, r, "([^"\n]+)"',text)
errors+=['Unexpected version: 2.12.8 -> 2.12.8; expected 2.12.9','Busy or radios inhibited; reboot P4 before another update']
for i,msg in enumerate(dict.fromkeys(errors)):
    latched=any(word in msg for word in ('quiesce','OTA begin','transfer timed','write failed','OTA end','activate failed','reboot/version','Unexpected version','inhibited'))
    pct=100 if any(word in msg for word in ('OTA end','activate failed','reboot/version','Unexpected version')) else 46 if any(word in msg for word in ('transfer timed','write failed')) else 0
    states.append((f'error-{i:02}',8,pct,int(latched),msg))
manifest=[]
for name,stage,pct,busy,msg in states:
    for orient in ['portrait','landscape']:
        env=os.environ.copy(); env.update(LSSIM_C6='1',LSSIM_OTA='0',LSSIM_C6_STAGE=str(stage),LSSIM_C6_PCT=str(pct),LSSIM_C6_BUSY=str(busy),LSSIM_C6_MESSAGE=msg or 'SD image checked; ready to review installation',LSSIM_C6_BOOT=str(2 if stage in [6,7] or 'reboot/version' in msg or 'Unexpected version' in msg else 1 if 'activate failed' in msg else 0))
        dest=out/f'{name}-{orient}'
        args=[str(exe),'update','--tft','-k','c','-o',str(dest.with_suffix('.bmp')),'-d']
        if orient=='landscape': args+=['-l']
        p=subprocess.run(args,env=env,capture_output=True,text=True,check=True)
        dest.with_suffix('.txt').write_text(p.stdout)
        im=Image.open(dest.with_suffix('.bmp')); im=im.rotate(270,expand=True) if orient=='landscape' else im; im.save(dest.with_suffix('.png'))
        dest.with_suffix('.bmp').unlink()
        manifest.append(f'{name}-{orient}: {msg}')
for stage in range(10):
    for orient in ['portrait','landscape']:
        env=os.environ.copy(); env['LSSIM_OTA']=str(stage)
        dest=out/f'p4-{stage}-{orient}'
        args=[str(exe),'update','--tft','-o',str(dest.with_suffix('.bmp'))]
        if orient=='landscape': args+=['-l']
        subprocess.run(args,env=env,capture_output=True,check=True)
        im=Image.open(dest.with_suffix('.bmp')); im=im.rotate(270,expand=True) if orient=='landscape' else im; im.save(dest.with_suffix('.png')); dest.with_suffix('.bmp').unlink()
if phase == 'after':
    for name,keys in [('confirm','ci'),('options','co'),('image-options','co'),('policy-options','co')]:
        for orient in ['portrait','landscape']:
            env=os.environ.copy(); env.update(LSSIM_C6='1',LSSIM_OTA='0',LSSIM_C6_BUSY='0',LSSIM_C6_STAGE='-1')
            dest=out/f'{name}-{orient}'
            args=[str(exe),'update','--tft','-k',keys,'-o',str(dest.with_suffix('.bmp')),'-d']
            if orient=='landscape': args+=['-l']
            if name=='image-options': args+=['-x','@enter']
            if name=='policy-options': args+=['-x','@down,@enter']
            result=subprocess.run(args,env=env,capture_output=True,text=True,check=True)
            dest.with_suffix('.txt').write_text(result.stdout)
            im=Image.open(dest.with_suffix('.bmp')); im=im.rotate(270,expand=True) if orient=='landscape' else im
            im.save(dest.with_suffix('.png')); dest.with_suffix('.bmp').unlink()
(out/'manifest.txt').write_text('\n'.join(manifest))
# Contact sheets retain separate full-resolution images for inspection.
files=[f for f in out.glob('*.png') if not f.stem.startswith('contact-')]
for page in range(0,len(files),12):
    sheet=Image.new('RGB',(1200,((min(12,len(files)-page)+3)//4)*430),'#111827')
    draw=ImageDraw.Draw(sheet)
    for j,f in enumerate(files[page:page+12]):
        im=Image.open(f); im.thumbnail((290,395))
        x=(j%4)*300; y=(j//4)*430
        sheet.paste(im,(x+(300-im.width)//2,y+25)); draw.text((x+5,y+5),f.stem,fill='white')
    sheet.save(out/f'contact-{page//12:02}.png')
print(f'{len(files)} renders saved to {out}')
