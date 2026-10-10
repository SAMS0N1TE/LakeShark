"""Deterministic six-cell fixture with geometry/labels/terrain across a corner."""
import sys
from pathlib import Path
import struct
import zstandard as zstd
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from build_cells import write_map
root=Path(sys.argv[1]);root.mkdir(parents=True,exist_ok=True)
header=bytearray(64);header[:8]=b'CTILE1\0\0';struct.pack_into('<I',header,8,7)
struct.pack_into('<4i',header,12,-740000000,420000000,-690000000,460000000);struct.pack_into('<I',header,36,21)
# One line crossing both tile edges, with a label that repeats in neighboring tiles.
name=b'Crossing Road'; head=bytearray(200);head[:4]=b'CT3\0'
struct.pack_into('<7I',head,4,1,1,1,3,200,208,248)
for cls in range(1,22):struct.pack_into('<II',head,32+(cls-1)*8,0 if cls<=13 else 1,1 if cls==13 else 0)
feature=struct.pack('<BBBBIIIii4hII',13,2,4,0,1,0,1,128,128,-8,100,264,128,0,0)
raw=bytes(head)+struct.pack('<II',268,len(name))+feature+struct.pack('<II',0,3)+struct.pack('<6h',-8,128,264,100,128,100)+name
shade=b'SH1\0\10\10\0\0'+bytes((0x98,0xba))
tile=b'CT6\0'+struct.pack('<III',len(raw),0,len(shade))+raw+shade
frame=zstd.ZstdCompressor(compression_params=zstd.ZstdCompressionParameters.from_level(3,window_log=16,write_checksum=1)).compress(tile)
packed=b'CZ7\0'+struct.pack('<I',len(tile))+frame
(root/'tile.cz7').write_bytes(packed);(root/'tile.ct6').write_bytes(tile)
# Oversized-window, concatenated, truncated and checksum failures are rejected.
badraw=tile+bytes(200000)
bad=zstd.ZstdCompressor(compression_params=zstd.ZstdCompressionParameters.from_level(3,window_log=18,write_checksum=1)).compress(badraw)
(root/'window.cz7').write_bytes(b'CZ7\0'+struct.pack('<I',len(badraw))+bad)
cells=[(x,y) for x in (76,77,78) for y in (92,93)]
groups={0:[]}
for z in range(8):
    for x,y in sorted({(x>>(8-z),y>>(8-z)) for x,y in cells}):groups[0].append((z,x,y,packed))
for x,y in cells:
    id=1+x*256+y;groups[id]=[(8,x,y,packed)]
for x in ((77<<6)-1,77<<6):
    for y in ((93<<6)-1,93<<6):groups[1+(x>>6)*256+(y>>6)].append((14,x,y,packed))
entries=[];alltiles=[]
for id,tiles in groups.items():
    name='overview.ctile' if not id else f'8-{(id-1)//256}-{(id-1)%256}.ctile'
    h,d=write_map(root/name,header,tiles,0 if not id else 8,7 if not id else 14);alltiles+=tiles
    for i in range(len(d)//24):
        r=bytearray(d[i*24:(i+1)*24]);off=struct.unpack_from('<Q',r,12)[0];struct.pack_into('<Q',r,12,((id+1)<<32)|off);entries.append(r)
entries.sort(key=lambda r:(r[0],struct.unpack_from('<II',r,4)))
header[28:30]=bytes((0,14));struct.pack_into('<I',header,32,len(entries));struct.pack_into('<Q',header,48,65538<<32)
(root/'index.cci').write_bytes(header+b''.join(entries));alltiles.sort(key=lambda t:t[:3]);write_map(root/'mono.ctile',header,alltiles,0,14)
print('six cells, overview, v7 decoder and cross-corner fixtures generated')
