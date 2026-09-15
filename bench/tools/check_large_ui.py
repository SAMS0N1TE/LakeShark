"""Render normal and large TUI layouts and check that primary controls remain visible.

Run after building lssim. Images use fixture data, not live radio reception.
"""
import argparse
import json
from pathlib import Path
import subprocess

APPS = ('home', 'p25', 'fm', 'adsb', 'set', 'radios', 'gps', 'map',
        'falls', 'mesh', 'diag', 'journal', 'rec', 'subghz', 'mixrf', 'splash')
REQUIRED = {
    'p25': ('SCAN', 'TUNE', 'SIGNAL', 'LISTS', 'SETTINGS', 'MORE'),
    'fm': ('A / B', 'TUNE', 'SCAN', 'LISTS', 'SQUELCH', 'MORE'),
    'set': ('Screen lock', 'Brightness', 'Auto dim', 'Dim after', 'Volume',
            'Theme', 'Daylight', 'Font', 'Boot sound', 'Alert sound',
            'Vibrate', 'USB autoreboot'),
    'radios': ('SDR', 'LORA', 'GPS', 'ANTENNA', 'CC1101', 'NRF24', 'NFC'),
    'subghz': ('WATCH', 'TUNE', 'PIN', 'ALERTS', 'EXPORT', 'JOURNAL',
               'BANDS', 'SETUP', 'SOURCE', 'TOOLS'),
    'mixrf': ('PROBE', 'MONITOR', 'BAND', 'MARK', '2.4 SCAN', 'VIEW', 'NFC', 'STOP'),
}
REQUIRED['rec'] = REQUIRED['subghz']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--sim', type=Path)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    sim = (args.sim or repo / 'bench/build/lssim.exe').resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    results = []
    for font in (0, 2):
        for landscape in (False, True):
            for app in APPS:
                name = f'{app}-font{font}-{"landscape" if landscape else "portrait"}'
                command = [str(sim), app, '-F', str(font), '-d', '-o', str(output / (name + '.bmp'))]
                if landscape:
                    command.append('-l')
                run = subprocess.run(command, cwd=repo, capture_output=True, text=True, check=True)
                (output / (name + '.txt')).write_text(run.stdout)
                missing = [label for label in REQUIRED.get(app, ()) if label not in run.stdout]
                fallback = any(text in run.stdout for text in ('Enlarge the pane', 'Open a larger radio view'))
                results.append(dict(view=name, missing=missing, fallback=fallback))
    (output / 'summary.json').write_text(json.dumps(results, indent=2))
    failures = [r for r in results if r['missing'] or r['fallback']]
    for result in failures:
        print(result)
    print(f'{len(results)} rendered layouts; {len(failures)} failed control checks. Fixture data only.')
    raise SystemExit(bool(failures))


if __name__ == '__main__':
    main()
