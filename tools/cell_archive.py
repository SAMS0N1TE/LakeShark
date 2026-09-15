"""Download CELL WATCH metadata once and build a local, provenance-preserving index.
Raw files are opt-in. Existing local downloads are reused. No SD files are changed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
from cell_console import Console


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--raw', action='append', default=[])
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    console = Console(args.port)
    records=[]
    try:
        listing=console.command('sd ls /sdcard/cell')
        (args.out/'listing.txt').write_text(listing, encoding='utf-8')
        names=re.findall(r'^  (\S+)\s+(\d+)\s*$',listing,re.M)
        for name,size in names:
            if not (name.endswith(('.json','.jsonl')) or name in args.raw):continue
            destination=args.out/name
            if destination.exists() and destination.stat().st_size==int(size):
                data=destination.read_bytes()
            else:
                data=console.download(name,destination)
            records.append(dict(file=name,bytes=len(data),sha256=hashlib.sha256(data).hexdigest()))
            if name.endswith('.json'):
                obj=json.loads(data)
                lte=obj.get('lte',{}); mib=lte.get('mib',{})
                print(json.dumps(dict(file=name,hz=obj.get('frequency_hz'),rate=obj.get('rate'),
                    gps=obj.get('gps_valid'),imu=obj.get('imu_valid'),mag=obj.get('mag_valid'),
                    pci=lte.get('pci'),sync=lte.get('found'),mib=mib.get('found'),crc_frames=mib.get('crc_frames'))),flush=True)
    finally:
        console.close()
        (args.out/'manifest.json').write_text(json.dumps(records,indent=2),encoding='utf-8')


if __name__=='__main__':main()
