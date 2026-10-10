#!/usr/bin/env python3
"""Build a portable CartoCore catalog from CTILE v2-v7 headers.

Schema: {"version":1,"regions":[{"name":str,"file":basename,"bytes":int,
"bbox":[west,south,east,north],"zmin":int,"zmax":int,"description":str,
"sha256":64 lowercase hex characters}]}. Files are siblings of catalog.json.
"""
import argparse
import hashlib
import json
import re
import struct
from pathlib import Path


CTILE_VERSION_MIN = 2
CTILE_VERSION_MAX = 7

def header_check(header, size):
    if len(header) < 64 or size < 64 or header[:8] != b'CTILE1\0\0':
        return False
    version, = struct.unpack_from('<I', header, 8)
    count, classes = struct.unpack_from('<2I', header, 32)
    declared, = struct.unpack_from('<Q', header, 48)
    return (CTILE_VERSION_MIN <= version <= CTILE_VERSION_MAX and
            header[28] <= header[29] <= 22 and classes == 21 and
            declared == size and count <= (size - 64) // 24)

def region(path):
    size = path.stat().st_size
    with path.open('rb') as f:
        header = f.read(64)
        if len(header) != 64 or header[:8] != b'CTILE1\0\0':
            raise ValueError(f'{path}: invalid CTILE magic/header')
        version, = struct.unpack_from('<I', header, 8)
        bbox = [v / 1e7 for v in struct.unpack_from('<4i', header, 12)]
        zmin, zmax = header[28:30]
        declared, = struct.unpack_from('<Q', header, 48)
        if not header_check(header, size):
            raise ValueError(f'{path}: unsupported version, size or zoom range')
        w, s, e, n = bbox
        if not (-180 <= w < e <= 180 and -85.051129 <= s < n <= 85.051129):
            raise ValueError(f'{path}: invalid bbox')
        if not re.fullmatch(r'[A-Za-z0-9_][A-Za-z0-9_.-]*\.ctile', path.name) or len(path.name) >= 128:
            raise ValueError(f'{path}: filename is not device-safe')
        digest = hashlib.sha256(header)
        for chunk in iter(lambda: f.read(1024 * 1024), b''):
            digest.update(chunk)
    return dict(name=path.stem.replace('_', ' ').replace('-', ' ').title(),
                file=path.name, bytes=size, bbox=bbox, zmin=zmin, zmax=zmax,
                description='', sha256=digest.hexdigest())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('folder', type=Path)
    parser.add_argument('-o', '--output', type=Path)
    parser.add_argument('--metadata', type=Path, help='JSON keyed by filename with name/description overrides')
    args = parser.parse_args()
    metadata = json.loads(args.metadata.read_text(encoding='utf-8')) if args.metadata else {}
    rows = [region(p) for p in sorted(args.folder.glob('*.ctile'))]
    if len(rows) > 48:
        parser.error('device catalog limit is 48 regions')
    for row in rows:
        for key in ('name', 'description'):
            if key in metadata.get(row['file'], {}):
                row[key] = str(metadata[row['file']][key])
    output = args.output or args.folder / 'catalog.json'
    output.write_text(json.dumps(dict(version=1, regions=rows), indent=2) + '\n', encoding='utf-8')
    print(f'{output}: {len(rows)} regions, {sum(r["bytes"] for r in rows):,} bytes')


if __name__ == '__main__':
    main()
