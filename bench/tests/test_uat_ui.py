"""Real UAT experiment, touch OPTIONS and rate/mode choices in both TFT postures."""
import os, re, subprocess, sys
from pathlib import Path
from PIL import Image
exe = Path(sys.argv[1]).resolve()
out = Path(sys.argv[2]).resolve()
out.mkdir(parents=True, exist_ok=True)

def run(name, wide, mode='stream', taps=''):
    env = dict(os.environ, LSSIM_UAT=mode, LSSIM_LORA='lr2021')
    dest = out / (name + ('-landscape' if wide else '-portrait'))
    cmd = [str(exe), 'experiments', '--tft', '-d', '-f', '3', '-o', str(dest.with_suffix('.bmp'))]
    if wide: cmd += ['-l']
    if taps: cmd += ['-x', taps]
    p = subprocess.run(cmd, env=env, text=True, capture_output=True, check=True)
    text = p.stdout + p.stderr
    dest.with_suffix('.txt').write_text(text, encoding='utf-8')
    picture = Image.open(dest.with_suffix('.bmp'))
    # Simulator writes physical panel coordinates, so rotate for review.
    if wide: picture = picture.rotate(-90, expand=True)
    picture.save(dest.with_suffix('.png'))
    return text

def hit(text, label, offset=2):
    for line in text.splitlines():
        m = re.match(r'^\s*(\d+) (.*)$', line)
        if m and label in m[2]:
            return f'{m[2].index(label)+offset}:{m[1]}'
    raise AssertionError(f'missing touch label: {label}')

for wide in (False, True):
    text = run('uat-stream-tft', wide)
    for label in ('MODE STREAM', '2000000 bps', '250000 expected', 'RESTARTS', 'loss seams',
                  'DN SYNC', 'UP SYNC', 'RS basic', 'Corrected', 'ADDRESS', 'POSITION', 'RX ONLY'):
        assert label in text, label
    options_hit = hit(text, 'OPTIONS')
    opts = run('uat-options-tft', wide, taps=options_hit)
    for label in ('OPTIONS / LR2021', '__|__', 'STREAM SAMPLE RATE', '2000k', '< BACK'):
        assert label in opts, label
    # All changes use filled keys in the real shared picker, including BACK.
    rate_row = next(line for line in opts.splitlines() if '2000k' in line)
    m = re.match(r'^\s*(\d+) (.*)$', rate_row)
    arrow = f'{m[2].rindex(">")-2}:{m[1]}'
    back = hit(opts, '< BACK')
    text = run('uat-rate-tft', wide, taps=','.join((options_hit, arrow, back)))
    assert 'MODE STREAM / 1500000 bps' in text
    # Switch MODE with its own right arrow, then return to PACKET readout.
    mode_row = next(line for line in opts.splitlines() if 'STREAM' in line and '<' in line)
    m = re.match(r'^\s*(\d+) (.*)$', mode_row)
    mode_arrow = f'{m[2].rindex(">")-2}:{m[1]}'
    text = run('uat-packet-touch-tft', wide, taps=','.join((options_hit, mode_arrow, back)))
    assert 'MODE PACKET' in text
    text = run('uat-packet-tft', wide, mode='packet')
    assert 'MODE PACKET' in text and '32-bit sync capture' in text
print('PASS: UAT STREAM, PACKET, OPTIONS and touch mode/rate changes in portrait + landscape TFT')
