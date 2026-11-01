#!/usr/bin/env python3
"""Build, inspect and sign LakeShark native ELF apps. No board connection."""
import argparse
import glob
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent
MAX_BYTES = 512 * 1024
MAGIC = b'LSG1'

def signature_parts(data):
    if data[-4:] != MAGIC:
        return data, None
    if len(data) < 6:
        raise ValueError('truncated signature trailer')
    n = struct.unpack_from('<H', data, len(data)-6)[0]
    if not 8 <= n <= 72 or n + 6 >= len(data):
        raise ValueError('invalid signature length')
    return data[:-n-6], data[-n-6:-6]

class Elf:
    def __init__(self, data):
        if len(data) > MAX_BYTES:
            raise ValueError('larger than 512 KB')
        self.data, self.sig = signature_parts(data)
        d = self.data
        def bounds(o,n):
            if o < 0 or n < 0 or o+n > len(d):
                raise ValueError('ELF range outside file')
        self.bounds = bounds
        bounds(0,52)
        if d[:7] != b'\x7fELF\x01\x01\x01':
            raise ValueError('needs little-endian ELF32')
        h = struct.unpack_from('<HHIIIIIHHHHHH',d,16)
        typ,machine,ver,self.entry,ph,sh,flags,eh,pe,np,se,ns,si = h
        if (typ,machine,ver,eh,se)!=(3,243,1,52,40) or not 1<ns<=128 or not 0<si<ns or np>16 or (np and pe!=32) or sh%4 or ph%4:
            raise ValueError('invalid ELF header')
        bounds(sh,ns*40); bounds(ph,np*32)
        self.sections = [struct.unpack_from('<10I',d,sh+i*40) for i in range(ns)]
        if any(self.sections[0]): raise ValueError('nonzero null section')
        strings=self.sections[si]
        def string(s,index):
            if s[1]!=3 or index>=s[5]: raise ValueError('invalid string index')
            bounds(s[4],s[5]); end=d.find(b'\0',s[4]+index,min(s[4]+s[5],s[4]+index+256))
            if end<0: raise ValueError('unterminated ELF string')
            return d[s[4]+index:end].decode('latin1')
        self.string=string
        self.names=[]
        occupied=[(0,52),(sh,sh+ns*40)] + ([(ph,ph+np*32)] if np else [])
        for s in self.sections:
            name=string(strings,s[0]); self.names.append(name)
            if s[4]>len(d) or s[5]>4*1024*1024 or s[6]>=ns or (s[8] and (s[8]>65536 or s[8]&(s[8]-1))):
                raise ValueError('invalid section')
            if s[1]!=8 and s[5]:
                bounds(s[4],s[5]); a,b=s[4],s[4]+s[5]
                if any(a<y and x<b for x,y in occupied): raise ValueError('overlapping sections/tables')
                occupied.append((a,b))
        if max(y for x,y in occupied)!=len(d): raise ValueError('unrecognized trailing ELF data')
        self.segments=[]; end=0
        for i in range(np):
            p=struct.unpack_from('<8I',d,ph+i*32); t,o,a,_,f,m,fl,al=p
            bounds(o,f)
            if m<f or a+m>4*1024*1024 or (al and (al>65536 or al&(al-1))): raise ValueError('invalid segment')
            if t==1 and m:
                if a<end: raise ValueError('unordered/overlapping segments')
                end=a+m; self.segments.append(p)
        if self.names.count('.lap_app')!=1: raise ValueError('missing/duplicate .lap_app')
        s=self.sections[self.names.index('.lap_app')]
        if s[1]!=1 or s[5]!=228 or not s[2]&2 or s[4]%4: raise ValueError('invalid .lap_app')
        self.prefix=d[s[4]:s[4]+198]
        magic,major,minor=struct.unpack_from('<IHH',self.prefix)
        # Firmware with API 2.2 runs 1.0-1.3 and 2.0-2.2 (ls_lap_fields_valid).
        if magic!=0x3150414c or not ((major==1 and minor<=3) or (major==2 and minor<=2)): raise ValueError('unsupported LAP API')
        self.manifest={'magic':magic,'api_major':major,'api_minor':minor}
        off=8
        for name,n in [('id',32),('name',16),('sub',32),('version',16),('author',24)]:
            field=self.prefix[off:off+n]; off+=n
            if b'\0' not in field: raise ValueError('unterminated '+name)
            self.manifest[name]=field.split(b'\0')[0].decode('utf-8',errors='surrogateescape')
        if not re.fullmatch(r'[a-z0-9_.-]{1,31}',self.manifest['id']) or self.manifest['id'] in ('.','..') or not self.manifest['name']:
            raise ValueError('invalid id/name')
        icon,color,flags,reserved=self.prefix[128:132]
        known=(15 if minor>=1 else 7) if major>=2 else 1   # NETWORK; 2.0 adds RADIO_RX and RADIO_TX; 2.1 SERVICE
        if icon>12 or color>15 or flags & ~known or (flags and major==1 and minor<3) or reserved: raise ValueError('invalid icon/color/reserved')
        art=[]
        for y in range(6):
            row=self.prefix[132+y*11:143+y*11]
            if b'\0' not in row or any(c and not 32<=c<=126 for c in row): raise ValueError('invalid icon art')
            art.append(row.split(b'\0')[0].decode('ascii'))
        self.manifest.update(icon=icon,color=color,flags=flags,reserved=reserved,icon_art=art,signature_present=self.sig is not None,signature_checked=False,size=len(data))

    def mapped(self,a,n,flags=0):
        return any(p[6]&flags==flags and p[2]<=a and a+n<=p[2]+p[5] for p in self.segments)

    def check(self,exports):
        if int.from_bytes(self.data[36:40],'little')&~1!=2: raise ValueError('needs ilp32f RISC-V ABI')
        if any(p[6]&3==3 for p in self.segments): raise ValueError('writable executable segment')
        if self.entry%2 or not self.mapped(self.entry,2,1): raise ValueError('unmapped entry')
        d=self.data
        for s in self.sections:
            if s[2]&2 and not self.mapped(s[3],s[5]): raise ValueError('unmapped section')
            if s[1]==9 or s[2]&0x400: raise ValueError('REL/TLS unsupported')
            if s[1] in (2,11):
                if s[4]%4 or s[9]!=16 or s[5]%16: raise ValueError('invalid symbols')
                st=self.sections[s[6]]
                for o in range(s[4],s[4]+s[5],16):
                    ni,val,size,info,other,ndx=struct.unpack_from('<IIIBBH',d,o)
                    name=self.string(st,ni)
                    if not ndx and val: raise ValueError('nonzero undefined symbol value')
                    if not ndx and name and name not in exports: raise ValueError('needs '+name)
                    if ndx and ndx!=0xfff1 and (ndx>=len(self.sections) or not self.mapped(val,size)): raise ValueError('invalid symbol range')
            if s[1]==4:
                t=self.sections[s[6]]
                if s[4]%4 or s[9]!=12 or s[5]%12 or t[1] not in (2,11) or t[9]!=16 or t[5]%16 or s[7]>=len(self.sections): raise ValueError('bad relocations')
                for o in range(s[4],s[4]+s[5],12):
                    addr,info,addend=struct.unpack_from('<III',d,o); typ=info&255
                    if typ not in (0,1,3,5) or info>>8>=t[5]//16 or (typ and (addr%4 or not self.mapped(addr,4,2))):
                        raise ValueError('unsupported or text relocation')
                    if typ==3 and not self.mapped(addend,0): raise ValueError('bad relative addend')

def compiler():
    exe='riscv32-esp-elf-gcc'+('.exe' if os.name=='nt' else '')
    found=shutil.which(exe)
    if found: return Path(found)
    roots=[os.environ.get('IDF_TOOLS_PATH',''), str(Path.home()/'.espressif'), 'C:/Espressif']
    choices=[]
    for root in filter(None,roots):
        choices+=glob.glob(root+'/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin/'+exe)
        choices+=glob.glob(root+'/riscv32-esp-elf/*/riscv32-esp-elf/bin/'+exe)
    if not choices: raise ValueError('IDF riscv32-esp-elf-gcc not found; set IDF_TOOLS_PATH')
    return Path(sorted(choices)[-1])

class CompileError(ValueError):
    """The compiler or linker said no; .output is what it printed."""
    def __init__(self,output):
        super().__init__('compile failed'); self.output=output

def build(directory,output=None,log=None):
    """Compile every .c in directory into a .lap. The compiler's messages go
    to stderr, or into the list `log` when one is given (lsdk parses them)."""
    directory=Path(directory).resolve(); cc=compiler()
    sources=sorted(directory.glob('*.c'))
    if not sources: raise ValueError('no .c files in '+str(directory))
    output=Path(output) if output else directory/(directory.name+'.lap')
    output.parent.mkdir(parents=True,exist_ok=True)
    fd,temp_name=tempfile.mkstemp(prefix=output.name+'.',suffix='.build',dir=output.parent)
    os.close(fd);temporary=Path(temp_name)
    args=['-march=rv32imafc_zicsr_zifencei','-mabi=ilp32f','-Os','-fPIC','-shared','-nostdlib','-nostartfiles','-e','lap_main',
          '-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-fvisibility=hidden',
          '-Wl,-Bsymbolic,--exclude-libs,ALL,-z,noexecstack,-z,max-page-size=4096', '-I'+str(ROOT/'include'),
          *map(str,sources),'-lgcc','-o',str(temporary)]
    wraps=['-Wl,--wrap='+n.removeprefix('__wrap_') for n in (ROOT/'exports.txt').read_text().splitlines() if n.startswith('__wrap_')]
    try:
        r=subprocess.run([str(cc),*args,*wraps],capture_output=True,text=True,errors='replace')
        said=r.stdout+r.stderr
        if log is not None: log.append(said)
        elif said: sys.stderr.write(said)
        if r.returncode: raise CompileError(said)
        strip=cc.with_name(cc.name.replace('gcc','strip'))
        subprocess.run([str(strip),'--strip-unneeded','--remove-section=.comment','--remove-section=.riscv.attributes',str(temporary)],check=True)
        e=Elf(temporary.read_bytes());e.check(set((ROOT/'exports.txt').read_text().splitlines()))
        temporary.replace(output)
    finally:
        temporary.unlink(missing_ok=True)
    if log is None: print(f'{output}: {len(e.data)} bytes; {e.manifest["id"]} {e.manifest["version"]}')
    return output

def keygen(path):
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.hazmat.primitives import serialization
    p=Path(path).resolve(); repo=ROOT.parent
    if p.is_relative_to(repo): raise ValueError('private key must be outside the repository')
    key=ec.generate_private_key(ec.SECP256R1())
    with p.open('xb') as f:
        if os.name!='nt': os.chmod(p,0o600)
        f.write(key.private_bytes(serialization.Encoding.PEM,serialization.PrivateFormat.PKCS8,serialization.NoEncryption()))
    print(key.public_key().public_bytes(serialization.Encoding.PEM,serialization.PublicFormat.SubjectPublicKeyInfo).decode(),end='')

def sign(path,keypath):
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.hazmat.primitives import serialization,hashes
    p=Path(path); e=Elf(p.read_bytes())
    key=serialization.load_pem_private_key(Path(keypath).read_bytes(),password=None)
    if not isinstance(key,ec.EllipticCurvePrivateKey) or not isinstance(key.curve,ec.SECP256R1): raise ValueError('needs ECDSA P-256 key')
    sig=key.sign(e.data,ec.ECDSA(hashes.SHA256()))
    result=e.data+sig+struct.pack('<H',len(sig))+MAGIC
    if len(result)>MAX_BYTES: raise ValueError('signed file exceeds 512 KB')
    tmp=p.with_suffix(p.suffix+'.tmp');tmp.write_bytes(result);tmp.replace(p)
    print(f'{p}: signed; sha256 {hashlib.sha256(result).hexdigest()}')

def main():
    p=argparse.ArgumentParser(description=__doc__); sub=p.add_subparsers(dest='cmd',required=True)
    b=sub.add_parser('build');b.add_argument('directory');b.add_argument('-o','--output')
    i=sub.add_parser('info');i.add_argument('file')
    k=sub.add_parser('keygen');k.add_argument('path')
    s=sub.add_parser('sign');s.add_argument('file');s.add_argument('--key',required=True)
    a=p.parse_args()
    try:
        if a.cmd=='build': build(a.directory,a.output)
        elif a.cmd=='info': print(json.dumps(Elf(Path(a.file).read_bytes()).manifest,indent=2))
        elif a.cmd=='keygen': keygen(a.path)
        else: sign(a.file,a.key)
    except (ValueError,OSError,subprocess.CalledProcessError) as ex:
        p.exit(1,'lap: '+str(ex)+'\n')

if __name__=='__main__': main()
