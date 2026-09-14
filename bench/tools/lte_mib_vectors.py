"""Receive-only, independently generated PBCH regression vectors; no SDR use."""
import ctypes as c
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
_build=tempfile.TemporaryDirectory(prefix="ls_mib_vectors_")
_library=Path(_build.name)/"lte_mib.dll"
subprocess.run(["gcc", "-O2", "-shared", "-o", str(_library),
                str(root/"components/lakeshark/apps/cell/lte_mib.c"), "-lm"], check=True)
dll=c.CDLL(str(_library))
dll.lte_mib_workspace_size.restype=c.c_size_t
class Mib(c.Structure):
    _fields_=[(name,c.c_int) for name in ("n_rb","ports","phich_duration","phich_resource","sfn","frames","first_frame")]+[("payload",c.c_uint32)]
dll.lte_mib_find.argtypes=[c.c_void_p,c.c_size_t,c.c_int,c.c_int,c.c_int,c.c_void_p,c.POINTER(Mib),c.c_void_p,c.c_void_p]
dll.lte_mib_find.restype=c.c_bool
import numpy as np

def gold(seed, count):
    a=np.zeros(1600+count+31, np.uint8); b=a.copy(); a[0]=1
    b[:31]=[(seed>>i)&1 for i in range(31)]
    for i in range(1600+count):
        a[i+31]=a[i+3]^a[i]
        b[i+31]=b[i+3]^b[i+2]^b[i+1]^b[i]
    return a[1600:1600+count]^b[1600:1600+count]

def frame_bits(payload, ports):
    data=[(payload>>i)&1 for i in range(23,-1,-1)]
    # Polynomial division over GF(2), rather than the firmware CRC register.
    divided=data+[0]*16
    polynomial=[(0x11021>>i)&1 for i in range(16,-1,-1)]
    for i in range(24):
        if divided[i]:
            divided[i:i+17]=[x^y for x,y in zip(divided[i:i+17],polynomial)]
    mask={1:0,2:0xffff,4:0x5555}[ports]
    bits=data+[v^((mask>>(15-i))&1) for i,v in enumerate(divided[24:])]
    memory=bits[-6:][::-1]
    coded=np.zeros((3,40),np.uint8)
    for i,bit in enumerate(bits):
        register=[bit]+memory
        for j,poly in enumerate((0o133,0o171,0o165)):
            coded[j,i]=sum(b*((poly>>(6-k))&1) for k,b in enumerate(register))%2
        memory=register[:6]
    # Five-bit bit reversal followed by the convolutional column rotation.
    columns=[int(f'{i:05b}'[::-1],2) for i in range(32)]
    columns=columns[16:]+columns[:16]
    streams=[]
    for stream in coded:
        padded=np.r_[np.full(24,-1),stream].reshape(2,32)
        ordered=padded[:,columns].T.flatten()
        streams.extend(ordered[ordered>=0])
    return np.tile(np.array(streams,np.uint8),16)

def generate(ports, count=5, base_sfn=1021, bad_crc=False):
    pci=113; timing=400; size=115200
    signal=np.zeros(size,np.complex128)
    channel=np.array([1+.3j,.8-.4j,-.2+.6j,.5+.1j])
    for frame in range(count):
        sfn=(base_sfn+frame)%1024
        payload=(4<<21)|(1<<20)|(1<<18)|((sfn//4)<<10)|0x1a0
        encoded=frame_bits(payload, ports)
        if bad_crc: encoded[::2]^=1
        scrambled=encoded^gold(pci,1920)
        bits=scrambled[(sfn%4)*480:(sfn%4+1)*480].astype(int)
        symbols=((1-2*bits[::2])+1j*(1-2*bits[1::2]))/np.sqrt(2)
        transmitted=np.zeros((4,240),np.complex128)
        if ports==1: transmitted[0]=symbols
        else:
            for i in range(0,240,2):
                a,b=((0,1) if ports==2 else (0,2) if i%4==0 else (1,3))
                transmitted[a,i:i+2]=symbols[i:i+2]/np.sqrt(2)
                transmitted[b,i:i+2]=[-symbols[i+1].conjugate()/np.sqrt(2),symbols[i].conjugate()/np.sqrt(2)]
        grid=np.zeros((5,72),np.complex128)
        n=0
        for l in range(4):
            for sc in range(72):
                if l<2 and sc%3==pci%3: continue
                grid[l,sc]=channel@transmitted[:,n]; n+=1
        for port in range(ports):
            for l in ([0,4] if port<2 else [1]):
                shifts={(0,0):0,(0,4):3,(1,0):3,(1,4):0,(2,1):3,(3,1):0}
                shift=(pci+shifts[port,l])%6
                pn=gold((1<<10)*(7*2+l+1)*(2*pci+1)+2*pci+1,440).astype(int)
                refs=((1-2*pn[::2])+1j*(1-2*pn[1::2]))/np.sqrt(2)
                grid[l,shift::6]=channel[port]*refs[104:116]
        for l in range(5):
            freq=np.zeros(128,np.complex128);freq[-36:]=grid[l,:36];freq[1:37]=grid[l,36:]
            symbol=np.fft.ifft(freq)*128
            start=timing+frame*19200+970+l*137
            cp=10 if l==0 else 9
            signal[start-cp:start+128]=np.r_[symbol[-cp:],symbol]
    signal*=np.exp(2j*np.pi*500*np.arange(size)/1920000)
    rng=np.random.default_rng(934)
    signal+=(rng.normal(size=size)+1j*rng.normal(size=size))*.03
    raw=np.clip(np.rint(np.column_stack((signal.real,signal.imag))*2+127.5),0,255).astype(np.uint8).ravel()
    return raw,pci,timing

for ports in (1,2,4):
    raw,pci,timing=generate(ports)
    ws=c.create_string_buffer(dll.lte_mib_workspace_size());result=Mib()
    ok=dll.lte_mib_find(raw.ctypes.data,len(raw)//2,pci,timing,500,ws,c.byref(result),None,None)
    print(ports,ok,{k:getattr(result,k) for k,_ in result._fields_})
    assert ok and result.ports==ports and result.n_rb==75 and result.sfn==1021 and result.frames==5
    raw,pci,timing=generate(ports,bad_crc=True)
    assert not dll.lte_mib_find(raw.ctypes.data,len(raw)//2,pci,timing,500,ws,c.byref(result),None,None)

if hasattr(c, "windll"):
    import _ctypes
    _ctypes.FreeLibrary(dll._handle)
_build.cleanup()
print("PASS: 1/2/4 ports, SFN wrap, nonzero extension bits, corrupted codewords rejected")
