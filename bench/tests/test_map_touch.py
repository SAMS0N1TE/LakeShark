"""Tap production MAP controls in both postures; retain reviewable lssim stills."""
from pathlib import Path
import os, re, subprocess, sys, tempfile
from PIL import Image
root=Path(__file__).resolve().parents[2]
exe=Path(sys.argv[1]).resolve()
out=Path(os.environ.get('LSSIM_TEST_OUT',root/'bench/build-blocks/map-touch'))
out.mkdir(parents=True,exist_ok=True)
def run(name,wide,script='',received=False):
    tag=name+('-landscape' if wide else '-portrait')
    with tempfile.TemporaryDirectory(prefix='map-touch-') as tmp:
        env=dict(os.environ,TEMP=tmp,TMP=tmp)
        env.pop('LSSIM_MAP_RECEIVED',None)
        if received: env['LSSIM_MAP_RECEIVED']='1'
        args=[str(exe),'map','--tft','-f','8','-d','-o',str(out/(tag+'.bmp'))]
        if wide: args+=['-l']
        if script: args+=['-x',script]
        p=subprocess.run(args,cwd=root,env=env,text=True,capture_output=True,check=True)
    text=p.stdout+p.stderr
    (out/(tag+'.txt')).write_text(text)
    im=Image.open(out/(tag+'.bmp'))
    if wide: im=im.transpose(Image.Transpose.ROTATE_270)
    im.save(out/(tag+'.png'))
    (out/(tag+'.bmp')).unlink()
    return text

def map_tap(text,label):
    for line in text.splitlines():
        m=re.match(r'\s*(\d+) (.*)',line)
        if m and label in m[2] and '|' not in m[2]:
            return f'{m[2].index(label)}:{m[1]}'
    raise AssertionError(label)

def tap(text,label):
    hits=[]
    for line in text.splitlines():
        m=re.match(r'\s*(\d+) (.*)',line)
        if m and label in m[2] and '|' in m[2]:
            hits.append(f'{m[2].index(label)+len(label)//2}:{m[1]}')
    assert hits,(label,text)
    return hits[-1]

for wide in (False,True):
    browse=run('browse',wide)
    peer_script=map_tap(browse,'NORTHFIELD')
    peer=run('peer-selected',wide,peer_script)
    assert 'CENTRE' in peer and 'MESSAGE' in peer
    message=run('peer-message',wide,peer_script+','+tap(peer,'MESSAGE'))
    assert 'MESH DM' in message
    marker_tap=run('marker-tapped',wide,map_tap(browse,'MARK 1'))
    assert 'MOVE HERE' in marker_tap
    mark=tap(browse,'MARK')
    selected=run('marker-selected',wide,mark)
    assert all(x in selected for x in ('PREV','NEXT','RENAME','MOVE HERE','DELETE','SEND','DONE'))
    renamed=run('rename',wide,mark+','+tap(selected,'RENAME'))
    assert 'RENAME' in renamed and 'DONE' in renamed
    moved=run('move-here',wide,mark+',@right,'+tap(selected,'MOVE HERE'))
    assert 'MOVE HERE' in moved
    select_script=tap(browse,'SELECT')
    selection=run('select',wide,select_script)
    assert 'RENAME' in selection
    for step in range(3):
        select_script+=','+tap(selection,'NEXT')
        selection=run('next',wide,select_script)
    assert 'MOVE HERE' not in selection and 'SEND' in selection
    delete=mark+','+tap(selected,'DELETE')
    confirm=run('delete-confirm',wide,delete)
    assert 'SURE?' in confirm
    deleted=run('deleted',wide,delete+','+tap(confirm,'SURE?'))
    assert 'ZOOM+' in deleted and 'SURE?' not in deleted
    send=run('send-picker',wide,mark+','+tap(selected,'SEND'))
    assert 'SEND TO' in send and 'TRANSMIT disarmed' in send
    send_script=mark+','+tap(selected,'SEND')
    refused=run('send-refused',wide,send_script+','+tap(send,'NORTHFIELD'))
    assert 'SEND TO' in refused and 'TRANSMIT disarmed' in refused
    channels=run('send-channels',wide,send_script+','+tap(send,'PAGE DN')) if wide else send
    assert 'CHANNEL' in channels
    draw_script=tap(browse,'DRAW')
    draw=run('draw-empty',wide,draw_script)
    draw_script+=','+tap(draw,'ADD')
    draw=run('draw-mode',wide,draw_script)
    assert '1 POINTS' in draw and all(x in draw for x in ('UNDO','FINISH','CANCEL','ZOOM+'))
    undone=run('draw-undo',wide,draw_script+','+tap(draw,'UNDO'))
    assert '0 POINTS' in undone
    # A map tap adds a second, distinct point; FINISH selects the line.
    two_script=draw_script+(',90:10' if wide else ',40:15')
    two=run('draw-two',wide,two_script)
    assert '2 POINTS' in two
    finished=run('line-selected',wide,two_script+','+tap(two,'FINISH'))
    assert 'SEND' in finished and 'MOVE HERE' not in finished
    cancelled=run('draw-cancel',wide,draw_script+','+tap(draw,'CANCEL'))
    assert 'SELECT' in cancelled
    received=run('received-item',wide,received=True)
    assert all(x in received for x in ('SAVE','DELETE','DONE','RECEIVED CAMP'))
    saved=run('received-saved',wide,tap(received,'SAVE'),True)
    assert 'SEND' in saved and 'MOVE HERE' in saved
print('PASS: touch SELECT/NEXT, RENAME, MOVE, FINISH, MARK, DELETE/SURE?, SEND refusal, DRAW/ADD/UNDO/CANCEL, received SAVE in both postures')
