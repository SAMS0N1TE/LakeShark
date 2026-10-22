"""Isolated outline metadata/worker contract; no board or network."""
import hashlib,json,os,struct,subprocess,sys,tempfile
from pathlib import Path
exe=Path(sys.argv[1]);fixture=Path(sys.argv[2])
with tempfile.TemporaryDirectory(prefix='outlines-',dir=exe.parent) as tmp:
    root=Path(tmp);sd=root/'maps';server=root/'server'
    (sd/'cells').mkdir(parents=True);(server/'places').mkdir(parents=True)
    data=struct.pack('<8sIII',b'LSOUT1\0\0',1,1,5)+struct.pack('<8sBB4hIHH32s',b'US',0,0,0,0,100,100,0,1,0,b'United States')+struct.pack('<II',0,5)+struct.pack('<10h',0,0,100,0,100,100,0,100,0,0)
    (server/'places/outlines.lso').write_bytes(data)
    entry=dict(file='places/outlines.lso',bytes=len(data),sha256=hashlib.sha256(data).hexdigest())
    states=dict(NH=dict(name='New Hampshire',tier='free',bytes=0,cells=[[77,93]]))
    (root/'old.json').write_text(json.dumps(states))
    codes='AL AK AZ AR CA CO CT DE DC FL GA HI ID IL IN IA KS KY LA ME MD MA MI MN MS MO MT NE NV NH NJ NM NY NC ND OH OK OR PA PR RI SC SD TN TX UT VT VA WA WV WI WY'.split()
    (root/'full.json').write_text(json.dumps(dict({st:states['NH'] for st in codes},outlines=entry)))
    (sd/'cells/states.json').write_text(json.dumps(dict(states,outlines=entry)))
    for name,patch in [('bad-file',dict(file='../outlines.lso')),('bad-bytes',dict(bytes=1.5)),('bad-sha',dict(sha256='z'*64))]:
        (root/(name+'.json')).write_text(json.dumps(dict(states,outlines=dict(entry,**patch))))
    (root/'not-object.json').write_text(json.dumps(dict(states,outlines=[])))
    (root/'duplicate.json').write_text(json.dumps(dict(states,outlines=entry))[:-1]+',"outlines":'+json.dumps(entry)+'}')
    env=dict(os.environ,LSSIM_CARTO_DIR=str(sd),LSSIM_CELLS_SERVER='file://'+server.as_posix()+'/',LSSIM_OUTLINES_TEST='1',LSSIM_OUTLINES_FIXTURES=str(root))
    run=subprocess.run([str(exe),'tiles','-f','1','-o',str(root/'unused.bmp')],env=env,capture_output=True,text=True,timeout=90)
    (exe.parent/'maps-outlines-test.log').write_text(run.stdout+run.stderr)
    assert run.returncode==0,(run.returncode,(run.stdout+run.stderr)[-5000:])
    assert 'PASS: outlines parse' in run.stdout,run.stdout[-5000:]
    print('PASS: outline metadata and real worker download decisions/transactions')
