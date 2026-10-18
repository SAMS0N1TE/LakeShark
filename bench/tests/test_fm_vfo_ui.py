"""FM VFO touch/keyboard journeys on AMOLED and cutout TFT in both orientations."""
from pathlib import Path
import re
import subprocess
import sys
from PIL import Image

sim = Path(sys.argv[1]).resolve()
out = sim.parent / 'fm-ui'
out.mkdir(exist_ok=True)

tft = True

def render(name, wide, *args):
    result = subprocess.run([str(sim), 'fm', *(['--tft'] if tft else []), *(['-l'] if wide else []),
        '-f', '2', '-d', '-o', str(out / (name + '.bmp')), *args],
        capture_output=True, text=True, check=True)
    (out / (name + '.txt')).write_text(result.stdout, encoding='utf-8')
    Image.open(out / (name + '.bmp')).save(out / (name + '.png'))
    cells = {}
    for line in result.stdout.splitlines():
        if line[:4].strip().isdigit():
            cells[int(line[:4])] = line[4:]
    return result.stdout, cells

for tft in (False, True):
    for wide in (False, True):
        suffix = ('tft-' if tft else 'amoled-') + ('landscape' if wide else 'portrait')
        text, cells = render('vfo-' + suffix, wide)
        title = next(y for y, line in cells.items() if ('TUNE  / STEP' if wide else 'TUNE  STEP') in line)
        runs = list(re.finditer(r'={8,}', cells[title + 1]))
        assert len(runs) == 5
        keys = []
        for run in runs:
            assert run.end() - run.start() >= 8
            assert cells[title + (3 if wide else 5)][run.start():run.end()] == run.group()
            # Portrait has three painted interior rows, all of them tappable.
            if not wide:
                for dy in (2,3,4):
                    assert '.' in cells[title + dy][run.start():run.end()]
            keys.append(f'{(run.start() + run.end()) // 2}:{title + (2 if wide else 3)}')
        assert '12.5k' in cells[title + (2 if wide else 3)]
        if not wide:
            assert 'STEP' in cells[title + 2]
            # Lower controls retain filled tap keys too, even on the shorter TFT.
            for label in ('VOLUME','GAIN dB','SQUELCH'):
                y = next(y for y,line in cells.items() if label in line)
                runs_lower = list(re.finditer(r'={8,}',cells[y+1]))
                assert len(runs_lower) == 2
                assert all(any(cells[yy][r.start():r.end()] == r.group()
                               for yy in range(y+5,y+7) if yy in cells)
                           for r in runs_lower)
        assert '100%' in text  # the meter's value must clear the camera hole
        # Derive targets from HEAD paint: bracketed title value and bar left of -/+.
        for label in ('VOLUME', 'GAIN dB', 'SQUELCH'):
            y = next(y for y, line in cells.items() if label in line)
            x = cells[y].index(label) - 2
            value = re.search(r'\[[^]]+\]', cells[y][x + 12:])
            vx = x + 12 + value.start()
            down = next(r for r in re.finditer(r'={8,}', cells[y + 1]) if r.start() >= x)
            bottom = next(yy for yy in range(y + 3, y + 8)
                          if cells[yy][down.start():down.end()] == down.group())
            # All title characters; corners and center of the unchanged bar.
            taps = [(cx, y) for cx in range(vx, vx + len(value.group()))]
            taps += [(cx, cy) for cx in (x + 2, down.start() - 3)
                     for cy in (y + 1, bottom)]
            for cx, cy in taps:
                exact, _ = render('exact-' + label.split()[0] + f'-{cx}-{cy}-' + suffix,
                                  wide, '-x', f'{cx}:{cy}')
                assert 'CANCEL' in exact and 'OK' in exact, (suffix, label, cx, cy)
            key_runs = [r for r in re.finditer(r'={8,}', cells[y + 1]) if r.start() >= x]
            up = key_runs[1]
            expected = {'VOLUME': ('70%', '80%'), 'GAIN dB': ('26.0', '30.0'),
                        'SQUELCH': ('11%', '13%')}[label]
            for direction, key in enumerate((down, up)):
                stepped, stepped_cells = render('step-' + str(direction) + '-' + label.split()[0] + '-' + suffix,
                                    wide, '-x', f'{key.start()}:{y + 1}')
                assert 'CANCEL' not in stepped
                assert '[' + expected[direction] + ']' in stepped_cells[y][vx:vx + 8]
        entry, _ = render('entry-ten-' + suffix, wide, '-k', 't154.785000')
        assert '154.785000' in entry
        for presses, label in enumerate(('25k','100k','1M','5k','6.25k','10k','12.5k'), 1):
            cycle, _ = render('cycle-' + str(presses) + '-' + suffix, wide, '-k', '/' * presses)
            assert 'STEP ' + label in cycle
        changed, _ = render('step-size-' + suffix, wide, '-x', keys[0])
        assert 'STEP 25k' in changed
        stepped, _ = render('manual-up-' + suffix, wide, '-x', keys[3])
        assert '162.5625' in stepped
        for idx, direction in ((1, '<'), (4, '>')):
            seek, _ = render('seek-' + str(idx) + '-' + suffix, wide, '-x', keys[idx])
            assert 'SCAN  ' in seek and 'NEXT' in seek and 'SKIP' in seek
            stop, _ = render('cancel-' + str(idx) + '-' + suffix, wide,
                             '-x', keys[idx] + ',' + keys[4])
            assert 'SCAN  ' not in stop and ('TUNE  / STEP' if wide else 'TUNE  STEP') in stop
        keyboard, _ = render('keyboard-step-' + suffix, wide, '-k', '/', '-x', '@right')
        assert 'STEP 25k' in keyboard and '162.5750' in keyboard
        seek, _ = render('keyboard-seek-' + suffix, wide, '-k', '.')
        assert 'SCAN  ' in seek and 'NEXT' in seek
        # All active controls use the idle card's same five filled hit rectangles.
        for tap, shortcut in ((keys[1], 'N'), (keys[2], 'K')):
            moved, _ = render('scan-' + shortcut + '-' + suffix, wide,
                              '-k', '.', '-x', tap)
            assert 'SCAN  ' in moved and 'HELD' not in moved
        for how, args in (('tap', ('-x', keys[3])), ('key', ('-k', '.H'))):
            held, held_cells = render('held-' + how + '-' + suffix, wide,
                                      *(['-k', '.'] if how == 'tap' else []), *args)
            assert 'SCAN  HELD' in held and 'RESUME' in held
            assert all(word in held for word in ('NEXT','SKIP','STOP'))
        resumed, _ = render('resume-' + suffix, wide, '-k', '.HH')
        assert 'SCAN  ' in resumed and 'HELD' not in resumed
        for shortcut in ('N', 'K'):
            moved, _ = render('held-' + shortcut + '-' + suffix, wide, '-k', '.H' + shortcut)
            assert 'SCAN  ' in moved and 'HELD' not in moved
        stopped, _ = render('stop-' + suffix, wide, '-k', '.HX')
        assert 'SCAN  HELD' not in stopped and ('TUNE  / STEP' if wide else 'TUNE  STEP') in stopped
        band, _ = render('band-scan-' + suffix, wide, '-k', '0SBSM1')
        assert 'SCAN  ' in band and 'NEXT' in band and 'SKIP' in band
        band_held, _ = render('band-held-' + suffix, wide, '-k', '0SBSM1H')
        assert 'SCAN  HELD' in band_held and 'RESUME' in band_held
        # The existing spectrum/scanner tabs remain reachable by tap.
        for label in ('SPECTRUM', 'SCANNER'):
            row = max(y for y, line in cells.items() if label in line)
            col = cells[row].index(label) + len(label) // 2
            page, _ = render(label.lower() + '-' + suffix, wide, '-x', f'{col}:{row}')
            assert 'TUNE  / STEP' not in page and 'TUNE  STEP' not in page
            assert (label in page) if label == "SPECTRUM" else ("CHANNELS" in page)
print('PASS: AMOLED/TFT portrait/landscape, filled TUNE keys, step/manual tune, seek, NEXT/SKIP/HOLD/RESUME/STOP touch and shortcuts, tabs')
