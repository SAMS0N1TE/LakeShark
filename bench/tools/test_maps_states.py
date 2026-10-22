"""Real nationwide worker contract. Uses isolated file host/SD, no board/network."""
import hashlib,json,os,struct,subprocess,sys,tempfile
from pathlib import Path
exe=Path(sys.argv[1]);fixture=Path(sys.argv[2]);part_bytes=2*1024*1024
with tempfile.TemporaryDirectory(prefix='maps-state-',dir=exe.parent) as tmp:
    root=Path(tmp);server=root/'server';sd=root/'maps'
    (server/'cells/8/77').mkdir(parents=True);(sd/'cells').mkdir(parents=True)
    overview=(fixture/'overview.ctile').read_bytes()
    cell=bytearray((fixture/'8-77-93.ctile').read_bytes())
    cell.extend(b'\0'*(25*part_bytes+8192-len(cell)));struct.pack_into('<Q',cell,48,len(cell))
    # This is a valid v7 map with unchanged directory/blob offsets and trailing padding.
    for i,at in enumerate(range(0,len(cell),part_bytes)):(server/f'cells/8/77/93.ctile.p{i}').write_bytes(cell[at:at+part_bytes])
    (server/'cells/overview.ctile').write_bytes(overview);(sd/'cells/overview.ctile').write_bytes(overview)
    catalog=bytearray(128);catalog[:8]=b'CCMAPS2\0';struct.pack_into('<I',catalog,8,2);struct.pack_into('<I',catalog,48,128)
    for cid,data,ps,pc in [(0,overview,0,1),(1+77*256+93,cell,part_bytes,26)]:
        catalog.extend(struct.pack('<II32sII',cid,len(data),hashlib.sha256(data).digest(),ps,pc))
    states={st:dict(code=st,name=name,tier=tier,cells=[[77,93]],bytes=len(cell),partial=[[77,93]]) for st,name,tier in [('NH','New Hampshire','free'),('CO','Colorado','free')]}
    for target in [server,sd]:
        (target/'cells/catalog.ccm').write_bytes(catalog);(target/'cells/states.json').write_text(json.dumps(states))
    codes='AL AK AZ AR CA CO CT DE DC FL GA HI ID IL IN IA KS KY LA ME MD MA MI MN MS MO MT NE NV NH NJ NM NY NC ND OH OK OR PA PR RI SC SD TN TX UT VT VA WA WV WI WY'.split()
    full={st:dict(name='State '+st,tier='free' if st in ('NH','VT','ME','MA') else 'plus',bytes=0,
        cells=[[i//256,i%256] for i in range(12000 if st=='AK' else 100)],partial=[[0,0]]) for st in codes}
    full_path=root/'full-states.json';full_path.write_text(json.dumps(full,indent=2))
    assert full_path.stat().st_size>512*1024
    env=dict(os.environ,LSSIM_CARTO_DIR=str(sd),LSSIM_CELLS_SERVER='file://'+server.as_posix()+'/',LSSIM_MAPS_TEST='1',LSSIM_MAPS_STATES_FIXTURE=str(full_path))
    run=subprocess.run([str(exe),'tiles','-f','1','-o',str(root/'unused.bmp')],env=env,capture_output=True,text=True,timeout=180)
    (exe.parent/'maps-state-test.log').write_text(run.stdout+run.stderr)
    assert run.returncode==0,(run.returncode,(run.stdout+run.stderr)[-4000:])
    assert 'PASS: CCMAPS2 catalog' in run.stdout,run.stdout[-4000:]
    # Keep the original CCMAPS1 worker upload/recovery/ownership contract exercised.
    legacy=root/'legacy';(legacy/'cells').mkdir(parents=True)
    old=bytearray(128);old[:8]=b'CCMAPS1\0';struct.pack_into('<II',old,8,2,128);struct.pack_into('<I',old,48,128)
    for cid,data in [(0,overview),(1+77*256+93,cell)]:old.extend(struct.pack('<II32s',cid,len(data),hashlib.sha256(data).digest()))
    (legacy/'cells/catalog.ccm').write_bytes(old)
    env.pop('LSSIM_MAPS_TEST');env.update(LSSIM_CELLS_TEST='1',LSSIM_CARTO_DIR=str(legacy))
    run=subprocess.run([str(exe),'tiles','-f','1','-o',str(root/'legacy.bmp')],env=env,capture_output=True,text=True,timeout=180)
    assert run.returncode==0,(run.returncode,(run.stdout+run.stderr)[-4000:])
    assert 'PASS: real worker upload CRC' in run.stdout
    # A second isolated run uses the new base/detail catalog and tier-free metadata.
    split_sd=root/'split-maps';(split_sd/'cells').mkdir(parents=True)
    def layer(data,low,high):
        count=struct.unpack_from('<I',data,32)[0];rows=[];blobs=[];offset=64
        for i in range(count):
            r=bytearray(data[64+i*24:88+i*24])
            if low<=r[0]<=high:
                off,n=struct.unpack_from('<QI',r,12);rows.append(r);blobs.append(data[off:off+n])
        offset+=len(rows)*24
        for r,b in zip(rows,blobs):struct.pack_into('<Q',r,12,offset);offset+=len(b)
        h=bytearray(data[:64]);h[28:30]=bytes((low,high));struct.pack_into('<I',h,32,len(rows));struct.pack_into('<Q',h,48,offset)
        return h+b''.join(rows)+b''.join(blobs)
    original=(fixture/'8-77-93.ctile').read_bytes()
    base=layer(original,8,12);detail=layer(original,13,16)
    detail.extend(bytes(part_bytes+8192-len(detail)));struct.pack_into('<Q',detail,48,len(detail))
    split_catalog=bytearray(catalog[:128]);struct.pack_into('<I',split_catalog,8,3)
    cid=1+77*256+93
    for key,data,name in [(0,overview,'cells/overview.ctile'),(cid,base,'cells/8/77/93.ctile'),(cid+65536,detail,'cells/8/77/93.detail.ctile')]:
        ps=0 if not key else part_bytes;pc=1 if not key else (len(data)+ps-1)//ps
        split_catalog.extend(struct.pack('<II32sII',key,len(data),hashlib.sha256(data).digest(),ps,pc))
        if key:
            for i,at in enumerate(range(0,len(data),ps)):(server/(name+f'.p{i}')).write_bytes(data[at:at+ps])
    (split_sd/'cells/overview.ctile').write_bytes(overview)
    split_states={'free_max_zoom':12,'CO':dict(name='Colorado',cells=[[77,93]],bytes=len(base)+len(detail),base_bytes=len(base),detail_bytes=len(detail))}
    (split_sd/'cells/catalog.ccm').write_bytes(split_catalog);(split_sd/'cells/states.json').write_text(json.dumps(split_states))
    env.pop('LSSIM_CELLS_TEST');env.update(LSSIM_MAPS_TEST='1',LSSIM_MAPS_SPLIT_TEST='1',LSSIM_CARTO_DIR=str(split_sd))
    run=subprocess.run([str(exe),'tiles','-f','1','-o',str(root/'split.bmp')],env=env,capture_output=True,text=True,timeout=180)
    assert run.returncode==0,(run.returncode,(run.stdout+run.stderr)[-4000:])
    assert 'PASS: split base all states' in run.stdout,run.stdout[-4000:]
    print('PASS: legacy and split states, all-state base, token-only detail, revoke/resume, part SHA, shared ownership')
