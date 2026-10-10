"""Real worker + screen integration. No board connection, ports or firmware build."""
from pathlib import Path
import hashlib,importlib.util,os,re,shutil,struct,subprocess,sys,tempfile,zlib
from PIL import Image
root=Path(__file__).resolve().parents[2]
out=root/'bench/out/cells';out.mkdir(parents=True,exist_ok=True)
engine=Path(os.environ.get('CARTOCORE_DIR',root.parent/'CartoCore-wip'))
data=Path(os.environ.get('CARTOCORE_DATA',root.parent/'CartoCore/data'))
spec=importlib.util.spec_from_file_location('push',root/'tools/carto_push.py');push=importlib.util.module_from_spec(spec);spec.loader.exec_module(push)
server=out/'server'
if not (server/'cells/catalog.ccm').exists():push.prepare(data/'cells-nh',data/'places/out/z8',server)
exe=root/'bench/build/lssim.exe'

def run(name,sd,script='',app='tiles',wide=False,frames=3,extra=None):
    env=dict(os.environ,LSSIM_CARTO_DIR=str(sd),LSSIM_CELLS_SERVER='file://'+server.as_posix())
    env.update(extra or {})
    args=[str(exe),app,'-f',str(frames),'-d','-o',str(out/(name+'.bmp'))]
    if script:args+=['-x',script]
    if wide:args+=['-l']
    p=subprocess.run(args,cwd=root,env=env,capture_output=True,text=True,timeout=180)
    log=p.stdout+p.stderr;(out/(name+'.log')).write_text(log)
    assert p.returncode==0,(name,p.returncode,log[-2000:])
    assert 'failed step=' not in log,(name,log)
    im=Image.open(out/(name+'.bmp'))
    if wide:im=im.transpose(Image.Transpose.ROTATE_270)
    im.save(out/(name+'.png'))
    return log

# Fresh isolated SD roots. Never modify the source NH set.
with tempfile.TemporaryDirectory(prefix='cells-',dir=root/'bench/build') as temp:
    sd=Path(temp)/'maps';sd.mkdir()
    log=run('search',sd,'+sNew_Hampshire,~80');assert '> New Hampshire' in log and 'STATE / United States' in log
    log=run('preview',sd,'+sNew_Hampshire,~30,@enter,~30');assert '7 cells / 0 installed / 0 unavailable' in log and '58.75 MB' in log
    run('queued',sd,'+sNew_Hampshire,~30,@enter,~15,@enter,~24')
    partial=list((sd/'cells').glob('*.ctile.part'));assert partial,'queue did not leave a resumable cell'
    prior={p.name:p.read_bytes() for p in partial}
    # Restart the process with no search page. Persistent queue must finish.
    run('resume-progress',sd,'+s,@f5,~10',frames=1)
    run('resume-background',sd,'~1300',frames=1)
    for name,content in prior.items():assert (sd/'cells'/name.removesuffix('.part')).read_bytes().startswith(content)
    for p in (sd/'cells').glob('8-*.ctile'):
        x,y=map(int,re.findall(r'\d+',p.stem)[1:]);assert push.digest(p)==push.digest(server/f'cells/8/{x}/{y}.ctile')
    assert len(list((sd/'cells').glob('8-*.ctile')))==7
    records=list((sd/'installed').glob('*.ccpi'));assert len(records)==1 and struct.unpack_from('<I',records[0].read_bytes(),8)[0]==0
    log=run('installed',sd,'+s,@f2');assert 'New Hampshire' in log and '[queued]' not in log
    log=run('search-city',sd,'+sConcord_NH,~80',wide=True);assert 'CITY / New Hampshire' in log
    assert (sd/'places/places_US.ccpl').is_file()
    log=run('city-preview',sd,'+sConcord_NH,~40,@enter,~10,@right,~10',wide=True);assert 'City radius: 30 km' in log
    run('keyboard',sd,'+s,@f1,+Concord,~20')
    log=run('info',sd,'+s,@f4',wide=True);assert 'CC BY' in log and 'GeoNames' in log
    cfg=dict(LSSIM_CARTOCORE='smooth',LSSIM_CARTO_CENTER='43.45,-71.71875',LSSIM_CARTO_ZOOM='12',LSSIM_CELLS_MEMORY='1',LSSIM_CARTO_CHECK_COMPLETE='1')
    log=run('map-go-to',sd,'+f,@enter,+Concord_NH,~40,@enter,~8',app='map',wide=True,frames=8,extra=cfg);assert 'SEARCH PLACES' not in log
    missing=Path(temp)/'missing';(missing/'cells').mkdir(parents=True)
    for name in ['index.cci','overview.ctile','8-77-93.ctile']:shutil.copyfile(sd/'cells'/name,missing/'cells'/name)
    for fill in ['smooth','braille']:
        cfg['LSSIM_CARTOCORE']=fill
        for name,source,z in [('edge',sd,12),('missing',missing,12),('missing-overview',missing,8),('nh-z8',sd,8),('nh-z16',sd,16)]:
            cfg['LSSIM_CARTO_ZOOM']=str(z)
            run(name+'-'+fill,source,app='map',wide=True,frames=8,extra=cfg)
        cfg['LSSIM_CARTO_ZOOM']='12'
        run('crossing-'+fill,sd,'@left,~3,@left,~3,@right,~3,@right,~3',app='map',wide=True,frames=8,extra=cfg)
    run('console-cells',sd,app='map',frames=8,extra=dict(cfg,LSSIM_CARTO_CONSOLE_TEST='1'))
    tx=Path(temp)/'transactions';(tx/'cells').mkdir(parents=True);shutil.copyfile(server/'cells/catalog.ccm',tx/'cells/catalog.ccm')
    log=run('transactions',tx,frames=1,extra={'LSSIM_CELLS_TEST':'1'});assert 'PASS: real worker upload CRC' in log

# Exercise the actual Python serial protocol without importing pyserial/opening a port.
class FakeSerial:
    def __init__(self):self.data=bytearray();self.reply=b'';self.sha='';self.size=0;self.calls=0
    def write(self,raw):
        assert len(raw)<=128,('console line overflow',len(raw));a=raw.decode().split();assert a[:2]==['carto','upload'];self.calls+=1
        if a[2]=='begin':self.size=int(a[4]);self.sha=a[5];self.reply=f'CARTO-UP READY {len(self.data)}\n'.encode()
        elif a[2]=='data':
            off=int(a[3]);chunk=bytes.fromhex(a[5]);assert zlib.crc32(chunk)==int(a[4],16)
            if off==len(self.data):self.data.extend(chunk)
            else:assert self.data[off:off+len(chunk)]==chunk
            self.reply=f'CARTO-UP ACK {len(self.data)}\n'.encode()
        elif a[2]=='end':
            assert len(self.data)==self.size and hashlib.sha256(self.data).hexdigest()==self.sha
            self.reply=b'CARTO-UP SAVED\n'
    def flush(self):pass
    def readline(self):r=self.reply;self.reply=b'';return r
fake=FakeSerial();sample=server/'cells/catalog.ccm';fake.data.extend(sample.read_bytes()[:32])
push.serial_asset(fake,sample,'cells/catalog.ccm');assert bytes(fake.data)==sample.read_bytes()
print('PASS: search/BLE/live keyboard, country shard, shape/radius preview, persisted queue/restart/Range seek/SHA, installed library, MAP GO TO, both fills, cell crossing/missing overview, upload transactions and serial simulator')
