"""The standalone builder flag preserves states and emits the canonical LSOUT1."""
import hashlib,json,struct,subprocess,sys,tempfile
from pathlib import Path
builder=Path(sys.argv[1]);evidence=Path(sys.argv[2])
with tempfile.TemporaryDirectory(prefix='outline-builder-',dir=evidence) as tmp:
    root=Path(tmp);deploy=root/'deploy';(deploy/'cells').mkdir(parents=True)
    states={'NH':{'name':'New Hampshire','cells':[[77,93]],'tier':'free','bytes':0}}
    (deploy/'cells/states.json').write_text(json.dumps(states))
    ring=[[-72,43],[-71,43],[-71,44],[-72,44],[-72,43]]
    geom=dict(type='Polygon',coordinates=[ring])
    a0=root/'admin0.json';a1=root/'admin1.json'
    a0.write_text(json.dumps(dict(features=[dict(properties=dict(ISO_A2='US',NAME='United States'),geometry=geom)])))
    a1.write_text(json.dumps(dict(features=[])))
    cmd=[sys.executable,str(builder),'--outlines-only','--root',str(root),'--admin0',str(a0),'--admin1',str(a1)]
    subprocess.run(cmd,check=True,capture_output=True)
    data=(deploy/'places/outlines.lso').read_bytes();meta=json.loads((deploy/'cells/states.json').read_text())
    assert meta.pop('outlines')==dict(file='places/outlines.lso',bytes=len(data),sha256=hashlib.sha256(data).hexdigest())
    assert meta==states
    magic,regions,rings,points=struct.unpack('<8sIII',data[:20])
    assert magic==b'LSOUT1\0\0' and regions==1 and rings==1
    assert len(data)==20+regions*58+rings*8+points*4
    subprocess.run(cmd,check=True,capture_output=True)
    assert (deploy/'places/outlines.lso').read_bytes()==data
    assert not (root/'build.lock').exists() and not list(deploy.rglob('*.part'))
    print('PASS: standalone outlines builder, SHA/size metadata, state preservation and repeatability')
