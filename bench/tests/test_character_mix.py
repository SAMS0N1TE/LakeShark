"""Reproducible character-mix stills using production lssim, no hardware."""
from pathlib import Path
import os
import subprocess
import sys
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'bench/build-blocks/renders'
OUT.mkdir(parents=True, exist_ok=True)
EXE = Path(sys.argv[2]).resolve() if len(sys.argv)>2 else ROOT / 'bench/build-blocks/lssim.exe'
phase = sys.argv[1]
iq = OUT / 'character-mix.sub'
iq.write_text('Filetype: LakeShark SubGhz IQ File\nVersion: 1\nFrequency: 433920000\n'
              'Protocol: LR2021_IQ\nSampleRate: 125000\nSamples: 128\n' +
              ('IQ_Data: ' + '00010000'*16 + '\n')*8)
cases = [
    ('detector', 'detector', '', {'LSSIM_DETECTOR': '1'}),
    ('detector-detail', 'detector', 'd', {'LSSIM_DETECTOR': '1'}),
    ('survey', 'link', 'v', {'LSSIM_SURVEY': 'active'}),
    ('survey-detail', 'link', 'vd', {'LSSIM_SURVEY': 'active'}),
    ('survey-summary', 'link', 'v', {'LSSIM_SURVEY': 'summary'}),
    ('compass', 'compass', '', {'LSSIM_COMPASS_QUALITY': 'DISTURBED'}),
    ('map', 'map', '', {}),
    ('c6', 'update', 'c', {'LSSIM_C6': '1', 'LSSIM_C6_STAGE': '3', 'LSSIM_C6_PCT': '48'}),
    ('c6-ready', 'update', 'c', {'LSSIM_C6': '1'}),
    ('rec', 'rec', '', {}),
    ('rec-iq', 'rec', '', {}),
    ('sweep', 'sweep', '', {}),
    ('example', 'example', 'ccccrcccccc', {}),
]
for name, screen, keys, fixture in cases:
    for wide in (False, True):
        stem = OUT / f'{phase}-{name}-{"landscape" if wide else "portrait"}'
        args = [str(EXE), screen, '--tft', '-d', '-A', '100', '-f', '3', '-o', str(stem.with_suffix('.bmp'))]
        if keys: args += ['-k', keys]
        if name == 'rec-iq': args += ['-r', str(iq)]
        if wide: args += ['-l']
        env = {k:v for k,v in os.environ.items() if not k.startswith('LSSIM_')}
        env.update(fixture)
        result = subprocess.run(args, cwd=ROOT, env=env, capture_output=True, check=True)
        text = result.stdout.decode('utf-8', errors='replace')
        stem.with_suffix('.txt').write_text(text, encoding='utf-8')
        im = Image.open(stem.with_suffix('.bmp'))
        if wide: im = im.transpose(Image.Transpose.ROTATE_270)
        im.save(stem.with_suffix('.png'))
        assert im.width > 0 and im.height > 0
        if phase == 'after' and name in ('map','rec','rec-iq','sweep'):
            before = OUT / stem.with_suffix('.png').name.replace('after-', 'before-', 1)
            assert im.tobytes() == Image.open(before).tobytes(), f'{name}: unrelated pixels changed'
        if name == 'example': assert 'Count: 6' in text
        if name.startswith('survey'): assert 'SURVEY' in text
        if name.startswith('detector'): assert 'DETECTOR' in text
        if name.startswith('c6'): assert 'C6 FIRMWARE' in text
        if name == 'rec-iq':
            assert 'REPLAY / LR2021 I/Q' in text and '128 I/Q pairs' in text
            assert 'PLAY ONCE' in text and 'POWER-' in text and 'POWER+' in text
            if phase == 'after':
                power = subprocess.run(args + ['-k','+'],cwd=ROOT,env=env,capture_output=True,check=True)
                changed = power.stdout.decode('utf-8',errors='replace')
                assert changed != text and 'REPLAY / LR2021 I/Q' in changed
                # Restore the review still after exercising the real key handler.
                subprocess.run(args,cwd=ROOT,env=env,capture_output=True,check=True)
        print(stem.name)

if phase == 'after':
    rows = []
    for name, *_ in cases:
        cells = []
        for orientation in ('portrait','landscape'):
            images = ''.join(f'<a href="{p}-{name}-{orientation}.png"><img loading="lazy" '
                             f'src="{p}-{name}-{orientation}.png" alt="{p} {name} {orientation}"></a>'
                             for p in ('before','after'))
            cells.append(f'<td>{images}</td>')
        rows.append(f'<tr><th>{name}</th>{"".join(cells)}</tr>')
    (OUT/'index.html').write_text('<!doctype html><meta charset="utf-8"><title>Character mix evidence</title>'
        '<style>body{background:#151515;color:#eee;font:16px system-ui}table{border-collapse:collapse}'
        'td,th{border:1px solid #555;padding:8px;vertical-align:top}img{max-width:290px;max-height:620px}'
        'a{display:inline-block;margin:4px}</style><h1>Character mix: before / after</h1>'
        '<p>Each pair is before (left), after (right). Open an image for native resolution.</p>'
        '<table><tr><th>Screen</th><th>Portrait</th><th>Landscape</th></tr>' + ''.join(rows) + '</table>',encoding='utf-8')
