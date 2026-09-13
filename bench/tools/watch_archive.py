"""Inspect a Sub-GHz archive and optionally export one representative capture."""
import argparse
import json
from pathlib import Path
import struct
import zlib

HEADER = struct.Struct('<IIIIQQ')
CATALOG = struct.Struct('<QQII')
EVENT = struct.Struct('<IIIII4xQQQIHBBi4x')
RECORD_BYTES = EVENT.size + 4096 * 4
CATALOG_BYTES = CATALOG.size + 16 * RECORD_BYTES

def load(directory):
    candidates = []
    for name in ('watch0.bin', 'watch1.bin'):
        try:
            data = (directory / name).read_bytes()
        except FileNotFoundError:
            continue
        if len(data) != HEADER.size + CATALOG_BYTES:
            continue
        magic, version, size, crc, generation, archive_id = HEADER.unpack_from(data)
        body = data[HEADER.size:]
        if (magic, version, size) != (0x5752534c, 2, CATALOG_BYTES):
            continue
        if zlib.crc32(body) != crc or CATALOG.unpack_from(body)[:2] != (archive_id, generation):
            continue
        candidates.append((generation, body))
    if not candidates:
        raise ValueError('No complete, supported archive generation')
    _, body = max(candidates, key=lambda item: item[0])
    records = []
    for i in range(16):
        offset = CATALOG.size + i * RECORD_BYTES
        fields = EVENT.unpack_from(body, offset)
        ident, hz, count, first_boot, last_boot, first_ms, last_ms, order, span, edges, pinned, reason, peak = fields
        if not ident:
            continue
        if not 6 <= edges <= 4096 or not count:
            raise ValueError('Invalid event metadata')
        pulses = struct.unpack_from('<' + 'i' * edges, body, offset + EVENT.size)
        records.append(dict(id=ident, frequency_hz=hz, count=count, pinned=bool(pinned),
            first_boot=first_boot, last_boot=last_boot, first_uptime_ms=first_ms,
            last_uptime_ms=last_ms, span_us=span, edges=edges, end_reason=reason,
            peak_magnitude=peak, pulses=pulses))
    return records

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--id', type=int, help='Export this one pattern ID')
    parser.add_argument('--out', type=Path, help='New .sub file; existing files are never overwritten')
    args = parser.parse_args()
    records = load(args.directory)
    if args.id is None:
        if args.out:
            parser.error('--out requires --id')
        print(json.dumps([{k:v for k,v in r.items() if k != 'pulses'} for r in records], indent=2))
        return
    if not args.out:
        parser.error('--id requires --out')
    record = next((r for r in records if r['id'] == args.id), None)
    if record is None:
        parser.error('Pattern ID is not in the archive')
    with args.out.open('x', encoding='utf-8') as f:
        f.write('Filetype: Flipper SubGhz RAW File\nVersion: 1\n')
        f.write(f"Frequency: {record['frequency_hz']}\n")
        f.write('Preset: FuriHalSubGhzPresetOok650Async\nProtocol: RAW\n')
        for i in range(0, len(record['pulses']), 512):
            f.write('RAW_Data: ' + ' '.join(map(str, record['pulses'][i:i+512])) + '\n')

if __name__ == '__main__':
    main()
