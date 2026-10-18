"""P25 VFO journeys through the real screen and action registry in lssim."""
from pathlib import Path
import re
import subprocess
import sys
from PIL import Image

sim = Path(sys.argv[1]).resolve()
out = sim.parent / 'p25-tune-ui'
out.mkdir(exist_ok=True)

for tft in (False, True):
    for wide in (False, True):
        suffix = ('tft-' if tft else 'amoled-') + ('landscape' if wide else 'portrait')

        def render(name, *args):
            target = out / (name + '-' + suffix + '.bmp')
            run = subprocess.run([str(sim), 'p25', *(['--tft'] if tft else []),
                                  *(['-l'] if wide else []), '-f', '2', '-d',
                                  '-o', str(target), *args], capture_output=True,
                                 text=True, check=True)
            target.with_suffix('.txt').write_text(run.stdout, encoding='utf-8')
            im = Image.open(target)
            if wide:
                im = im.transpose(Image.Transpose.ROTATE_270)
            im.save(target.with_suffix('.png'))
            rows = {int(line[:4]): line[4:] for line in run.stdout.splitlines()
                    if line[:4].strip().isdigit()}
            return run.stdout, rows

        text, rows = render('idle')
        title = next(y for y, line in rows.items() if 'TUNE  ' in line)
        keys = list(re.finditer(r'={8,}', rows[title + 1]))
        assert len(keys) == 5, suffix
        taps = [f'{r.start()}:{title + 1}' for r in keys]
        for index, expected in ((2, '851.0000'), (3, '851.0250')):
            text, _ = render('step-' + str(index), '-x', taps[index] + ",+t")
            assert expected in text, (suffix, expected)
        text, _ = render('cycle-step', '-k', '/', '-x', '@right,+t')
        assert '851.0375' in text, suffix
        for index in (1, 4):
            text, _ = render('seek-' + str(index), '-x', taps[index])
            assert 'SCAN  ' in text and all(word in text for word in ('NEXT', 'SKIP', 'HOLD', 'STOP'))
        text, _ = render('held', '-k', '.H')
        assert 'SCAN  HELD' in text and 'RESUME' in text
        for shortcut in ('N', 'K'):
            text, _ = render('advance-' + shortcut, '-k', '.H' + shortcut)
            assert 'SCAN  ' in text and 'HELD' not in text
        text, _ = render('stopped', '-k', '.HX')
        assert 'SCAN  HELD' not in text and 'TUNE  ' in text
        for word in ('P25 / RECEIVER', 'DECODE / SIGNAL', 'ACTIVITY',
                     'RECEIVER HEALTH', 'AUDIO drops', 'DECODE 0.0', 'VOLUME', 'GAIN'):
            assert word in text, (suffix, word)
print('PASS: P25 actual frequency retunes, raster, bidirectional seek, scan actions and retained content on all four layouts')
