"""Reproducible lssim evidence: real touch controls over host scenario setups."""
from pathlib import Path
import os
import subprocess
import sys
import struct
import zlib

ROOT = Path(__file__).resolve().parents[2]


def save_png(bmp, png):
    """lssim's uncompressed RGB24 BMP to PNG; no optional Python packages."""
    data = bmp.read_bytes()
    offset, = struct.unpack_from('<I', data, 10)
    w, h = struct.unpack_from('<ii', data, 18)
    bits, = struct.unpack_from('<H', data, 28)
    compression, = struct.unpack_from('<I', data, 30)
    assert data[:2] == b'BM' and bits == 24 and compression == 0 and w > 0 and h > 0
    stride = (w * 3 + 3) & ~3
    raw = bytearray()
    for y in range(h - 1, -1, -1):
        row = data[offset + y * stride:offset + y * stride + w * 3]
        rgb = bytearray(len(row))
        rgb[0::3], rgb[1::3], rgb[2::3] = row[2::3], row[1::3], row[0::3]
        raw += b'\0' + rgb
    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))
    png.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                    + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def run(exe, out, landscape=False, tft=False):
    out.mkdir(parents=True, exist_ok=True)
    h = 28 if landscape else 64
    if tft:
        h = 27 if landscape else 61
    fire_y = (h - 3) * 17          # the flight keys fill the dash's last five rows
    action_y = (h - 3) * 17
    action_x = 420 if landscape else 500
    script = [
        '2:carrier=inspect', f'5:down={action_x},{action_y}', '6:up', '10:carrier=inspect',
        '15:down=500,170', '18:up', '25:carrier=cruise',
        '35:down=100,200', '38:down=130,225', '41:up',
        '60:carrier=target', '85:carrier=combat',
        f'86:down=100,{fire_y}', '90:carrier=inspect', '107:up', '110:carrier=inspect',
        '130:carrier=dock', '190:carrier=inspect',
        '240:carrier=jump', '320:carrier=inspect', '380:carrier=inspect',
        '390:carrier=rings', '400:carrier=inspect',
    ]
    env = dict(os.environ, LAPSIM='carrier', LAPSIM_SCRIPT=';'.join(script),
               LAPSIM_PROFILE='1', LSSIM_FRAMES=str(out))
    cmd = [str(exe), 'lap', '-A', '40', '-f', '410', '-o', str(out / 'final.bmp')]
    if landscape:
        cmd.append('-l')
    if tft:
        cmd.append('--tft')
    result = subprocess.run(cmd, cwd=ROOT, env=env, capture_output=True, text=True)
    (out / 'run.log').write_text(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    shots = {'launch': 12, 'cruising': 32, 'target': 65, 'lasers': 90,
             'explosion': 104, 'docking': 190, 'hyperspace': 320, 'arrival': 380, 'rings': 400}
    frames = sorted(out.glob('[0-9][0-9][0-9][0-9].bmp'))
    assert len(frames) >= 410, len(frames)
    # Opening HOME is also presented: locate the first app frame by the caption
    # state trace; this lssim presents one HOME frame before frame_no starts at 0.
    for name, n in shots.items():
        save_png(frames[n + 1], out / (name + '.png'))
    # Raw captures are transient. Keep reviewable PNGs and the exact input script.
    (out / 'input.txt').write_text(';'.join(script) + '\n')
    for frame in frames:
        frame.unlink()
    (out / 'final.bmp').unlink()
    assert 'jump 2' in result.stdout, result.stdout
    assert 'kills 1' in result.stdout, result.stdout
    assert 'debris 64' in result.stdout, result.stdout
    print(result.stdout)


if __name__ == '__main__':
    exe = Path(sys.argv[1]).resolve()
    out = Path(sys.argv[2]).resolve()
    run(exe, out / 'portrait')
    run(exe, out / 'landscape', landscape=True)
    run(exe, out / 'tft-portrait', tft=True)
    run(exe, out / 'tft-landscape', landscape=True, tft=True)
