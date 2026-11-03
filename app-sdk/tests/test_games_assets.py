"""SDK 2.5: decoder corpus + headless Studio templates. No board/desktop UI.
Build via test_example_contracts.py first; run python app-sdk/tests/test_games_assets.py.
"""
import json
import os
from pathlib import Path
import random
import struct
import subprocess
import sys
import unittest
import zlib
SDK=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(SDK))
import api_registry
import apidoc
import lap
import lsdk
BUILD=SDK.parent/'bench/build'
EXE=BUILD/('test_lap_games_assets.exe' if os.name=='nt' else 'test_lap_games_assets')
SIM=BUILD/('lssim.exe' if os.name=='nt' else 'lssim')
WORK=BUILD/'games-assets-scenarios'
INKS=[0,0xa800,0x0540,0xad40,0x0015,0xa815,0x0555,0xad55,0x52aa,0xfaaa,0x57ea,0xffea,0x52bf,0xfabf,0x57ff,0xffff]
RGB=[(v>>11)*255//31 for v in INKS]
PALETTE=[((v>>11)*255//31,((v>>5)&63)*255//63,(v&31)*255//31) for v in INKS]
def chunk(tag,data): return struct.pack('>I',len(data))+tag+data+struct.pack('>I',zlib.crc32(tag+data))
def png(w,h,depth,kind,raw,extra=b'',level=9,strategy=zlib.Z_DEFAULT_STRATEGY):
    z=zlib.compressobj(level,zlib.DEFLATED,15,8,strategy); data=z.compress(raw)+z.flush()
    return b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,depth,kind,0,0,0))+extra+chunk(b'IDAT',data[:len(data)//2])+chunk(b'IDAT',data[len(data)//2:])+chunk(b'IEND',b'')
def filtered(rows,bpp):
    out=bytearray(); prev=bytes(len(rows[0]))
    for y,row in enumerate(rows):
        mode=y%5; out.append(mode)
        for x,v in enumerate(row):
            a=row[x-bpp] if x>=bpp else 0; b=prev[x]; c=prev[x-bpp] if x>=bpp else 0
            p=a+b-c; distances=[abs(p-a),abs(p-b),abs(p-c)]; paeth=(a,b,c)[distances.index(min(distances))]
            out.append((v-(0,a,b,(a+b)//2,paeth)[mode])&255)
        prev=row
    return bytes(out)
def nearest(rgba):
    r,g,b,a=rgba
    if a<128:return 255
    return min(range(16),key=lambda k:sum((x-y)**2 for x,y in zip((r,g,b),PALETTE[k])))

class GamesAssets(unittest.TestCase):
    @classmethod
    def setUpClass(cls): WORK.mkdir(parents=True,exist_ok=True)
    def native(self,args):
        r=subprocess.run([str(EXE),*map(str,args)],env=lsdk.host_env(),capture_output=True,text=True,timeout=30)
        self.assertEqual(r.returncode,0,r.stdout+r.stderr+' '+str(args))
    def decode(self,name,data,expected=None,w=0,h=0,kind='png',error=-200):
        path=WORK/(name+'.'+kind);path.write_bytes(data)
        if expected is None:self.native([kind,path,error])
        else:
            raw=WORK/(name+'.raw');raw.write_bytes(expected)
            self.native([kind,path,0,raw,w,h])
    def test_native_boundaries_and_golden_blit(self): self.native([])
    def test_png_modes_filters_and_deflate(self):
        rng=random.Random(119)
        for mode in (0,2,4,6):
            w,h=27,15; channels={0:1,2:3,4:2,6:4}[mode]
            rows=[]; expected=bytearray()
            for y in range(h):
                row=bytearray()
                for x in range(w):
                    r,g,b=[rng.randrange(256) for _ in range(3)];a=rng.choice((0,127,128,255))
                    if mode in (0,4):g=b=r
                    row.extend((r,) if mode==0 else (r,g,b) if mode==2 else (r,a) if mode==4 else (r,g,b,a))
                    expected.append(nearest((r,g,b,a if mode in (4,6) else 255)))
                rows.append(row)
            for level,strategy in ((0,zlib.Z_DEFAULT_STRATEGY),(9,zlib.Z_FIXED),(9,zlib.Z_DEFAULT_STRATEGY)):
                self.decode(f'mode{mode}_{level}_{strategy}',png(w,h,8,mode,filtered(rows,channels),level=level,strategy=strategy),expected,w,h)
        for depth in (1,2,4,8):
            w,h=29,10; np=min(16,1<<depth);rows=[];expected=bytearray()
            for y in range(h):
                row=bytearray((w*depth+7)//8)
                for x in range(w):
                    i=(x+y)%np; row[x*depth//8]|=i<<(8-depth-x*depth%8);expected.append(255 if i==0 else i)
                rows.append(row)
            extra=chunk(b'PLTE',bytes(c for rgb in PALETTE[:np] for c in rgb))+chunk(b'tRNS',bytes([0]+[255]*(np-1)))
            self.decode('indexed'+str(depth),png(w,h,depth,3,filtered(rows,1),extra),expected,w,h)
        for kind,raw,key in ((0,b'\0\x7f',b'\0\x7f'),(2,b'\0\xff\0\0',b'\0\xff\0\0\0\0')):
            self.decode('trns'+str(kind),png(1,1,8,kind,raw,chunk(b'tRNS',key)),b'\xff',1,1)
    def test_corrupt_assets_and_budgets(self):
        good=png(2,1,8,6,b'\0'+b'\xff\0\0\xff'*2)
        self.decode('truncated',good[:-1]);self.decode('trailing',good+b'bad')
        damaged=bytearray(good);damaged[-1]^=1;self.decode('crc',damaged)
        for name,raw in [('filter',b'\5'+bytes(8)),('short',bytes(3)),('long',bytes(10))]:self.decode(name,png(2,1,8,6,raw))
        self.decode('budget',png(4096,4096,8,6,b'\0'),error=-201)
        # Good PNG CRC around a bad zlib checksum still must fail.
        raw=zlib.compress(b'\0'+bytes(8));raw=raw[:-1]+bytes([raw[-1]^1])
        self.decode('adler',b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',2,1,8,6,0,0,0))+chunk(b'IDAT',raw)+chunk(b'IEND',b''))
        goodmesh=(SDK/'templates/flight3d/assets/ship.l3m').read_bytes()
        for name,data in [('meshtruncated',goodmesh[:-1]),('meshtrailing',goodmesh+b'x'),('meshversion',goodmesh[:4]+b'\2'+goodmesh[5:]),('meshcount',goodmesh[:8]+b'\xff'*4+goodmesh[12:])]:self.decode(name,data,kind='mesh')
    def run_template(self,name,scenario,hardware,script='',asset=None):
        folder=WORK/(name+'_'+scenario);folder.mkdir(exist_ok=True)
        app='template_'+name
        if asset is not None:
            dest=folder/'sdcard/apps_data'/app;dest.mkdir(parents=True,exist_ok=True)
            (dest/('sheet.png' if name=='arcade2d' else 'ship.l3m')).write_bytes(asset)
        env=dict(lsdk.host_env(),LAPSIM=app,LAPSIM_HW=hardware,LAPSIM_CONSENT='grant',LAPSIM_SCRIPT=script,LAPSIM_RADIO='none')
        for key in ('LAPSIM_DLL','LSSIM_FRAMES'):env.pop(key,None)
        r=subprocess.run([str(SIM),'lap','-A','40','-f','100','-d'],cwd=folder,env=env,capture_output=True,text=True,errors='replace',timeout=90,
             creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)
        text=r.stdout+r.stderr;(folder/'run.log').write_text(text,encoding='utf-8')
        self.assertEqual(r.returncode,0,text[-4000:]);self.assertNotIn('simulator contract:',text)
        return folder,text
    def test_headless_assets_input_music_saves(self):
        for name in ('arcade2d','flight3d'):
            with self.subTest(app=name):
                self.run_template(name,'none','none','2:keydown=@right;6:keyup=@right;10:focus=0;15:focus=1;20:resize=320,240')
                asset=(SDK/f'templates/{name}/assets'/('sheet.png' if name=='arcade2d' else 'ship.l3m')).read_bytes()
                script='0:mixpcm=music.s16le;1:keydown=@right;1:keydown=@up;4:keyup=@up;13:keyup=@right;15:key=p;24:key=n;27:key=p;35:buttondown=200,100,0;40:pointer=80,60,1;45:buttonup=80,60,0;50:focus=0;55:focus=1;65:hide;70:show'
                folder,text=self.run_template(name,'loaded','sd,speaker,desktop',script,asset)
                if name=='arcade2d':
                    data=(folder/'music.s16le').read_bytes();self.assertGreater(len(data),1000);self.assertTrue(any(data))
                    for slot in ('save_score0','save_score1'):self.assertTrue((folder/'sdcard/apps_data/template_arcade2d'/slot).exists(),slot)
                self.run_template(name,'corrupt','sd,desktop','',b'bad asset')
                self.run_template(name,'oversized','sd,desktop','',bytes(20000))
    def test_registry_templates_and_abi(self):
        calls=apidoc.entries();helpers=[e for e in calls if e.get('sdk_since')=='2.5' and e['kind']=='function']
        self.assertGreater(len(helpers),20)
        self.assertTrue(all(e['simulator']['status']=='real' and not e.get('exported') for e in helpers))
        self.assertEqual(api_registry.version(SDK),(2,6))
        for target in ('arcade2d','flight3d'):
            p=BUILD/'sdk-contracts'/('template_'+target+'.lap');elf=lap.Elf(p.read_bytes())
            self.assertEqual(elf.manifest['api_minor'],lap.sdk_api_version(SDK)[1]);self.assertLess(len(elf.data),512*1024)
            self.assertEqual(elf.sections[elf.names.index('.lap_app')][5],228)
        folder=WORK/'legacy24';folder.mkdir(exist_ok=True)
        (folder/'main.c').write_text('#define LAP_TARGET_API_24\n#include "lakeshark_app.h"\nLAP_APP(.id="legacy24",.name="LEGACY",.version="1");\n')
        path=lap.build(folder,folder/'legacy.lap',log=[]);self.assertEqual(lap.Elf(path.read_bytes()).manifest['api_minor'],4)
    def test_obj_converter_rejects_bad_indices(self):
        path=WORK/'bad.obj';path.write_text('v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 99\n')
        r=subprocess.run([sys.executable,str(SDK/'tools/obj2l3.py'),str(path),'--binary','-o',str(WORK/'bad.l3m')],capture_output=True,text=True)
        self.assertNotEqual(r.returncode,0);self.assertIn('invalid face index',r.stderr)
if __name__=='__main__':unittest.main()
