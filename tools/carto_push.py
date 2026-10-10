#!/usr/bin/env python3
"""Push one map to a board running `carto recv` (standard library only)."""
import argparse
import hashlib
import shutil
import struct
import zlib
import http.client
from pathlib import Path
import re
import select
import sys
import time


def push_single(args):
    conn = http.client.HTTPConnection(args.board_ip, args.port, timeout=30)
    start = time.monotonic()
    try:
        with args.file.open("rb") as file:
            size = args.file.stat().st_size
            if not size:
                raise ValueError("file is empty")
            target = "/maps/" + args.file.name + ("?f=1" if args.force else "")
            conn.putrequest("PUT", target)
            conn.putheader("X-Carto-Code", args.code.lower())
            conn.putheader("Content-Length", str(size))
            conn.putheader("Content-Type", "application/octet-stream")
            conn.putheader("Connection", "close")
            conn.endheaders()
            sent, next_percent = 0, 10
            reply = None
            # Give header-only rejection a chance to arrive before sending an
            # unread body (which can cause TCP to discard the error on close).
            # Accepted requests wait for the body, so this wait is bounded.
            if select.select([conn.sock], [], [], 0.5)[0]:
                reply = conn.getresponse()
            while reply is None and sent < size:
                # A header/auth rejection can arrive while we are still sending.
                # Read it before further body writes turn it into a socket reset.
                if select.select([conn.sock], [], [], 0)[0]:
                    reply = conn.getresponse()
                    break
                chunk = file.read(min(32 * 1024, size - sent))
                if not chunk:
                    raise ValueError("file truncated during upload")
                try:
                    conn.send(chunk)
                except OSError as send_error:
                    # The server may already have sent the HTTP rejection before
                    # closing with unread request bytes. Preserve that response.
                    try:
                        reply = conn.getresponse()
                    except (OSError, http.client.HTTPException):
                        raise send_error
                    break
                sent += len(chunk)
                percent = sent * 100 // size
                if percent >= next_percent:
                    print(f"\r{sent:,}/{size:,} bytes {percent}%", end="", flush=True)
                    next_percent = (percent // 10 + 1) * 10
            print()
            if reply is None:
                reply = conn.getresponse()
            message = reply.read(4096).decode("utf-8", errors="replace").strip()
            if not 200 <= reply.status < 300:
                raise ValueError(f"HTTP {reply.status} {reply.reason}: {message}")
            if sent != size:
                raise ValueError(f"HTTP {reply.status} {reply.reason}: {message}; server replied before upload completed")
            seconds = time.monotonic() - start
            print(f"HTTP {reply.status}: {message}; {sent:,} bytes, {seconds:.3f} s, {sent / seconds / 1e6:.2f} MB/s")
        return 0
    except (OSError, ValueError, http.client.HTTPException) as exc:
        print(f"\ncarto push failed: {exc}", file=sys.stderr)
        return 1
    finally:
        conn.close()



def digest(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        while chunk:=f.read(1024*1024): h.update(chunk)
    return h.digest()


def asset_timeout(size):
    # Socket operations stay bounded; allow SD verification of large assets
    # at a conservative 64 KiB/s plus two minutes of fixed overhead.
    return 120 + size / (64 * 1024)


def prepare(cells, places, destination):
    """Export the documented web layout and bounded board catalog."""
    if (cells/'CURRENT').is_file():
        generation=(cells/'CURRENT').read_text().strip()
        if not re.fullmatch(r'[A-Za-z0-9_-]+',generation): raise ValueError('unsafe CURRENT')
        cells=cells/generation
    destination.mkdir(parents=True,exist_ok=True)
    records=[]
    for src in sorted(cells.glob('*.ctile')):
        if src.name=='overview.ctile': ident=0;relative='cells/overview.ctile'
        else:
            m=re.fullmatch(r'8-(\d+)-(\d+)\.ctile',src.name)
            if not m: continue
            x,y=map(int,m.groups())
            if x>255 or y>255: raise ValueError('non-z8 cell')
            ident=1+x*256+y;relative=f'cells/8/{x}/{y}.ctile'
        dst=destination/relative;dst.parent.mkdir(parents=True,exist_ok=True)
        shutil.copyfile(src,dst);records.append((ident,src.stat().st_size,digest(src)))
    for src,dst in [(cells/'index.cci',destination/'cells/index.cci')]+[(p,destination/'places'/p.name) for p in places.glob('*.ccpl')]:
        dst.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(src,dst)
    base=destination/'places/places_base.ccpl';index=destination/'cells/index.cci'
    header=bytearray(128);header[:8]=b'CCMAPS1\0'
    struct.pack_into('<II',header,8,len(records),index.stat().st_size);header[16:48]=digest(index)
    struct.pack_into('<I',header,48,base.stat().st_size);header[52:84]=digest(base)
    data=b''.join(struct.pack('<II32s',*r) for r in sorted(records));header[84:116]=hashlib.sha256(data).digest()
    (destination/'cells/catalog.ccm').write_bytes(header+data)
    attribution=places/'ATTRIBUTION.txt'
    if attribution.exists():shutil.copyfile(attribution,destination/'places/ATTRIBUTION.txt')
    print(f'Prepared {len(records)-1} cells, overview, index, catalog and places: {destination}')


def assets(root, country=None):
    # Catalog first, index last: existing cell readers can retain the old index
    # until all new files have been validated and installed.
    paths=[root/'cells/catalog.ccm',root/'places/places_base.ccpl']
    if country:paths.append(root/f'places/places_{country.upper()}.ccpl')
    paths += [root/'cells/overview.ctile']+sorted((root/'cells/8').glob('*/*.ctile'))+[root/'cells/index.cci']
    for path in paths:
        relative=path.relative_to(root).as_posix()
        m=re.fullmatch(r'cells/8/(\d+)/(\d+)\.ctile',relative)
        if m: relative=f'cells/8-{m[1]}-{m[2]}.ctile'
        yield path,relative


def serial_command(port,line,expected,timeout=30):
    port.write((line+'\r\n').encode('ascii'));port.flush();deadline=time.monotonic()+timeout
    while time.monotonic()<deadline:
        response=port.readline().decode('ascii',errors='replace').strip()
        if response.startswith('CARTO-UP ERROR'):raise ValueError(response)
        if response.startswith(expected):return response[len(expected):].strip()
    raise TimeoutError('No upload acknowledgement')


def serial_asset(port,path,relative):
    sha=digest(path).hex();size=path.stat().st_size
    offset=int(serial_command(port,f'carto upload begin {relative} {size} {sha}','CARTO-UP READY'))
    if not 0<=offset<=size:raise ValueError('invalid resume offset')
    with path.open('rb') as f:
        f.seek(offset)
        while chunk:=f.read(32): # existing 128-character console line limit
            command=f'carto upload data {offset} {zlib.crc32(chunk):08x} {chunk.hex()}'
            for retry in range(3):
                try:
                    ack=int(serial_command(port,command,'CARTO-UP ACK'));break
                except TimeoutError:
                    if retry==2:raise
            if ack!=offset+len(chunk):raise ValueError('incorrect ACK offset')
            offset=ack
    serial_command(port,'carto upload end','CARTO-UP SAVED',120)
    print(f'{relative}: verified {size:,} bytes')


def main():
    parser=argparse.ArgumentParser(description='Push maps, or prepare/push a cell and place set.')
    parser.add_argument('file',type=Path)
    parser.add_argument('board_ip',nargs='?')
    parser.add_argument('port',type=int,nargs='?',default=8080)
    parser.add_argument('--force',action='store_true')
    parser.add_argument('--code',help='32-digit code displayed by carto recv')
    parser.add_argument('--prepare',type=Path,metavar='WEB_ROOT',help='export cell generation to server layout')
    parser.add_argument('--places',type=Path,help='places source directory (with --prepare)')
    parser.add_argument('--assets',action='store_true',help='push prepared server directory')
    parser.add_argument('--country',help='also push this country shard, e.g. US')
    parser.add_argument('--serial-port',help='explicit console port; never autodetected')
    parser.add_argument('--baud',type=int,default=115200)
    args=parser.parse_args()
    if args.prepare:
        if not args.places:parser.error('--prepare requires --places')
        prepare(args.file,args.places,args.prepare);return 0
    if args.country and not re.fullmatch('[A-Za-z]{2}',args.country):parser.error('country must be two letters')
    if not 1<=args.port<=65535:parser.error('port must be 1..65535')
    if args.serial_port:
        if not args.assets:parser.error('serial upload requires --assets')
        try:
            import serial  # optional; no port enumeration
        except ImportError:
            parser.error('serial upload requires pyserial: python -m pip install pyserial')
        with serial.Serial(args.serial_port,args.baud,timeout=1) as port:
            for path,relative in assets(args.file,args.country):serial_asset(port,path,relative)
        return 0
    if not args.board_ip or not args.code or not re.fullmatch('[0-9a-fA-F]{32}',args.code):parser.error('WiFi upload requires board_ip and --code (32 hex digits)')
    if not args.assets:
        if len(args.file.name)>127 or not re.fullmatch(r'[A-Za-z0-9._-]*\.ctile',args.file.name):parser.error('invalid .ctile filename')
        return push_single(args)
    for path,relative in assets(args.file,args.country):
        size=path.stat().st_size
        conn=http.client.HTTPConnection(args.board_ip,args.port,timeout=asset_timeout(size),blocksize=32*1024)
        try:
            with path.open('rb') as f:
                conn.request('PUT','/maps/'+relative,body=f,headers={'X-Carto-Code':args.code.lower(),'X-Carto-SHA256':digest(path).hex(),'Content-Length':str(size),'Connection':'close'})
                reply=conn.getresponse();message=reply.read(4096).decode(errors='replace')
                if reply.status!=200:raise ValueError(f'{relative}: HTTP {reply.status}: {message}')
                print(f'{relative}: {message}')
        finally:conn.close()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, http.client.HTTPException) as exc:
        print(f"carto push failed: {exc}",file=sys.stderr)
        sys.exit(1)
