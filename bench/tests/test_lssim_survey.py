"""Production lssim SURVEY render and tap regression checks (no hardware)."""
from pathlib import Path
import os, re, subprocess
from PIL import Image
ROOT = Path(__file__).resolve().parents[2]
OUT = Path(os.environ.get('LSSIM_TEST_OUT',ROOT/'bench/build-blocks/survey-polish'))
OUT.mkdir(parents=True,exist_ok=True)
EXE = Path(os.environ.get('LSSIM_TEST_EXE',ROOT/'bench/build-blocks/lssim.exe'))

def render(name, state='idle', wide=False, keys='v', script='', frames=3, event=False):
    env = dict(os.environ, LSSIM_SURVEY=state)
    if event: env['LSSIM_SURVEY_EVENT']='1'
    cmd=[str(EXE),'link','--tft','-k',keys,'-d','-A','100','-f',str(frames),'-o',str(OUT/(name+'.bmp'))]
    if wide: cmd += ['-l']
    if script: cmd += ['-x',script]
    result=subprocess.run(cmd,env=env,capture_output=True,check=True)
    text=result.stdout.decode('utf-8',errors='replace')
    (OUT/(name+'.txt')).write_text(text)
    im=Image.open(OUT/(name+'.bmp'))
    if wide: im=im.rotate(-90,expand=True)
    im.save(OUT/(name+'.png'))
    return text,im

def tap(text,label):
    for row,line in reversed(re.findall(r'^\s*(\d+) (.*)$',text,re.M)):
        if label in line:
            return f'{line.index(label)+len(label)//2}:{row}'
    raise AssertionError(f'missing tap target {label}')

checks=0
for wide in (False,True):
    orientation='landscape' if wide else 'portrait'
    for state in ('idle','active','summary','busy','wait','detail','options','radios','recording'):
        fixture= 'active' if state=='detail' else 'idle' if state in ('options','radios','recording') else state
        keys='vd' if state=='detail' else 'vo' if state in ('options','radios','recording') else 'v'
        script='@enter' if state=='radios' else '@down,@enter' if state=='recording' else ''
        text,im=render(f'after-{state}-{orientation}',fixture,wide,keys,script)
        assert 'SURVEY' in text
        if state in ('idle','active','summary','busy','wait'): assert 'CH 11' in text or 'CH --' in text
        if state in ('options','radios','recording'): assert 'RECORDING PLAN' in text
        checks+=1
    base,_=render('test-base-'+orientation,wide=wide)
    started,_=render('test-start-'+orientation,wide=wide,script=tap(base,'START'))
    assert 'SURVEY / LIVE' in started
    stopped,_=render('test-stop-'+orientation,wide=wide,script=tap(base,'START')+','+tap(started,'STOP'))
    assert 'SURVEY / LIVE' not in stopped
    busy,_=render('test-busy-'+orientation,'busy',wide,script=tap(base,'START'))
    assert 'SURVEY / SAVING' in busy
    active,_=render('test-active-'+orientation,'active',wide)
    detail,_=render('test-detail-'+orientation,'active',wide,script=tap(active,'DETAIL'))
    assert 'BEST SIGNAL' in detail and 'sightings' in detail
    sort,_=render('test-sort-'+orientation,'active',wide,script=tap(active,'SORT'))
    assert 'Sort NEWEST' in sort
    paged,_=render('test-page-'+orientation,'active',wide,script=tap(active,'NEXT'))
    assert 'Lakeside AP 01' not in paged
    opt,_=render('test-options-'+orientation,wide=wide,script=tap(base,'OPTIONS'))
    radios,_=render('test-radios-'+orientation,wide=wide,script=tap(base,'OPTIONS')+','+tap(opt,'RADIOS'))
    assert 'SCAN INTERVAL' in radios
    # Steppers and keypad remain the shared OPTIONS flow.
    changed,_=render('test-step-'+orientation,wide=wide,keys='vo',script='@enter,@down,@down,@right')
    assert 'Scan every 11s' in changed
    pad,_=render('after-keypad-'+orientation,wide=wide,keys='vo',script='@enter,@down,@down,@enter')
    assert 'SCAN INTERVAL' in pad
    checks+=9
    _,still=render('test-still-'+orientation,wide=wide,frames=20)
    _,initial=render('test-initial-'+orientation,wide=wide,frames=3)
    assert still.tobytes()==initial.tobytes(), 'idle screen must remain still'
    _,pulse=render('after-event-'+orientation,'active',wide,frames=4,event=True)
    _,settled=render('after-settled-'+orientation,'active',wide,frames=20,event=True)
    assert pulse.tobytes()!=settled.tobytes(), 'observation must pulse'
    _,later=render('test-later-'+orientation,'active',wide,frames=30,event=True)
    assert settled.tobytes()==later.tobytes(), 'one event must settle, not loop'
    checks+=3
print(f'{checks} SURVEY render/interaction checks passed')

