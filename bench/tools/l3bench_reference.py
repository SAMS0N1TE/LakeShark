"""Write bench/tools/l3bench_reference.h from l3bench's frame dumps.

    l3bench bench/build-lap3d bench/build-lap3d/dumps
    python bench/tools/l3bench_reference.py bench/build-lap3d/dumps <revision>

Each dump is dots then tags. Tokens: 0..127 = literal length-1 then the bytes;
128..255 = a match of length-3 then a little-endian 16-bit backward distance.
"""
import sys
import re
import argparse
from pathlib import Path


def encode(data):
    out, lit, i, n = bytearray(), bytearray(), 0, len(data)
    table = {}

    def flush():
        while lit:
            chunk = lit[:128]
            out.append(len(chunk) - 1)
            out.extend(chunk)
            del lit[:128]

    while i < n:
        best_len, best_dist = 0, 0
        if i + 3 <= n:
            key = bytes(data[i:i + 3])
            for j in reversed(table.get(key, [])[-16:]):
                d = i - j
                if d > 65535:
                    break
                k = 0
                while i + k < n and k < 130 and data[j + k] == data[i + k]:
                    k += 1
                if k > best_len:
                    best_len, best_dist = k, d
            table.setdefault(key, []).append(i)
        if best_len >= 3:
            flush()
            out.append(128 + best_len - 3)
            out += bytes((best_dist & 255, best_dist >> 8))
            for k in range(1, best_len):
                if i + k + 3 <= n:
                    table.setdefault(bytes(data[i + k:i + k + 3]), []).append(i + k)
            i += best_len
        else:
            lit.append(data[i])
            i += 1
    flush()
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dumps', type=Path)
    parser.add_argument('revision')
    parser.add_argument('--scenes', nargs='+', type=int, default=list(range(7)),
                        help='Refresh only these scene numbers; retain all other arrays verbatim')
    args = parser.parse_args()
    dumps, revision = args.dumps, args.revision
    old = Path('bench/tools/l3bench_reference.h').read_text()
    lines = ['/* Host-only lossless oracle at %s.' % revision,
             ' * Tokens: 0..127 = literal length-1; 128..255 = match length-3,',
             ' * followed by a little-endian 16-bit backward distance. No firmware data. */',
             '#ifndef L3BENCH_REFERENCE_H', '#define L3BENCH_REFERENCE_H', '#include <stdint.h>']
    names = []
    for o in range(2):
        row = []
        for s in range(7):
            name = 'l3_reference_%d_%d' % (o, s)
            row.append(name)
            if s not in args.scenes:
                lines.append(re.search(r'static const uint8_t ' + name + r'\[\] = \{.*?\n\};', old, re.S).group())
                continue
            data = (dumps / ('%d-%d.bin' % (o, s))).read_bytes()
            enc = encode(data)
            lines.append('static const uint8_t %s[] = {' % name)
            for k in range(0, len(enc), 24):
                lines.append('    ' + ','.join(str(b) for b in enc[k:k + 24]) + ',')
            lines.append('};')
        names.append(row)
    lines.append('static const uint8_t *const l3_reference[2][7] = {')
    for row in names:
        lines.append('    {' + ', '.join(row) + '},')
    lines.append('};')
    lines.append('#endif')
    Path('bench/tools/l3bench_reference.h').write_text('\n'.join(lines) + '\n')


if __name__ == '__main__':
    main()
