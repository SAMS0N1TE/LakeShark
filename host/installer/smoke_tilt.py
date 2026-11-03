"""Headless motion API + real app frames, checkout or frozen DLL simulator.

python host/installer/smoke_tilt.py --checkout
python host/installer/smoke_tilt.py --exe build_app/dist/LakeShark.exe
Reuses the installed compiler; no downloads or desktop windows.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

from smoke_sim import REPO, Screen, grid
from smoke_test import own_process_tree, stop

PROBE = r'''
#include "lakeshark_app.h"
#include <stdio.h>
static void draw(void) {
    lap_motion_t m; char b[160];
    lap_clear(7);
    lap_put_str(1,1,"MOTION PROBE: real lap_motion sample",7);
    if(lap_motion(&m)) {
        snprintf(b,sizeof(b),"ACC=%.4f,%.4f,%.4f",m.ax,m.ay,m.az); lap_put_str(1,3,b,7);
        snprintf(b,sizeof(b),"ANGLE=%.2f,%.2f GYRO=%.0f",m.roll,m.pitch,m.gz); lap_put_str(1,5,b,7);
    }
}
LAP_APP(.id="motion-probe",.name="Motion probe",.sub="Sensor regression",.draw=draw);
'''


def app_key(raw):
    """Visual cell data inside the app; exclude clocks/board chrome."""
    cols, rows = struct.unpack_from('<HH', raw, 1)
    cells, at = [b''] * (cols * rows), 8
    while at < len(raw):
        t, at = raw[at], at + 1
        if t == ord('P'):
            at += 32
            continue
        first, n = struct.unpack_from('<HH', raw, at)
        at += 4
        size = 3 if t == ord('C') else 8
        for i in range(n):
            cells[first+i] = raw[at:at+size]
            at += size
    return b''.join(cells[cols*3:cols*(rows-3)])


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    source = ap.add_mutually_exclusive_group(required=True)
    source.add_argument('--checkout', action='store_true')
    source.add_argument('--exe', type=Path)
    ap.add_argument('--port', type=int, default=8815)
    ap.add_argument('--only', nargs='+', choices=('probe', 'dots', 'dice', 'level'), default=('probe', 'dots', 'dice', 'level'))
    args = ap.parse_args()
    mode = 'checkout' if args.checkout else 'frozen'
    out = REPO / ('build_installer/tilt-' + mode)
    out.mkdir(parents=True, exist_ok=True)
    (out / 'profile').mkdir(exist_ok=True)
    env = dict(os.environ, USERPROFILE=str(out / 'profile'),
               LOCALAPPDATA=str(REPO / 'build_installer/sim-smoke-verified/local'),
               LSDK_STUDIO_SETTINGS=str(out / 'settings.json'))
    for k in ('PYTHONPATH', 'PYTHONHOME', 'LAPSIM_DLL', 'LAPSIM_SCRIPT', 'LAPSIM_RADIO'):
        env.pop(k, None)
    if args.checkout:
        cmd = [sys.executable, str(REPO / 'app-sdk/lsdk.py'), 'studio', '--no-browser',
               '--workspace', str(out / 'apps'), '--port', str(args.port)]
    else:
        env['PATH'] = str(Path(os.environ.get('SystemRoot', 'C:/Windows')) / 'System32')
        cmd = [str(args.exe.resolve()), '--studio-only', '--port', str(args.port)]
    base = 'http://127.0.0.1:%d' % args.port

    def api(name, body=None):
        req = urllib.request.Request(base + '/api/' + name,
            data=body if isinstance(body, bytes) else json.dumps(body).encode() if body is not None else None,
            headers={'X-Studio': '1', 'Content-Type': 'application/json'})
        with urllib.request.urlopen(req, timeout=180) as r:
            return json.load(r)

    def tilt(phase='move', x=0, y=0):
        assert api('sim/tilt', dict(phase=phase, x=x, y=y))['ok']

    results = []
    with (out / 'studio.log').open('w', encoding='utf-8') as log:
        proc = subprocess.Popen(cmd, cwd=REPO, env=env, stdout=log, stderr=log,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        own_process_tree(proc)
        try:
            end = time.monotonic() + 60
            while True:
                try:
                    state = api('state'); break
                except OSError:
                    assert time.monotonic() < end and proc.poll() is None
                    time.sleep(.2)
            assert state['frozen'] == bool(args.exe)
            with urllib.request.urlopen(base + '/screen/screen.html') as r:
                page = r.read()
                assert page.index(b'/static/sim-tilt.js') < page.index(b'/static/sim-touch.js')
            for bad in ({'x': 46, 'y': 0}, {'x': float('nan'), 'y': 0}, {'x': True, 'y': 0}, {'phase': 'bad'}):
                try:
                    api('sim/tilt', bad)
                    raise AssertionError(bad)
                except urllib.error.HTTPError as e:
                    assert e.code == 400

            for name in args.only:
                project = api('new', {'name': 'tilt-' + name + '-' + str(time.time_ns()),
                                      'template': 'dots' if name == 'dots' else 'blank'})
                query = '?p=' + urllib.parse.quote(project['path'])
                def put(file, data):
                    api('file' + query + '&path=' + file, data.encode())
                if name == 'probe': put('main.c', PROBE)
                if name == 'dice': put('main.c', (REPO / 'app-sdk/apps/dice/dice.c').read_text())
                if name == 'level':
                    for file in ('main.c', 'meter.c', 'meter.h'):
                        source = (REPO / 'app-sdk/apps/level' / file).read_text()
                        # Checkout links examples and user apps together: isolate
                        # this copy's three engine symbols from built-in LEVEL.
                        for symbol in ('meter_start', 'meter_feed', 'meter_lines'):
                            source = source.replace(symbol, Path(project['path']).name.replace('-', '_') + '_' + symbol)
                        put(file, source.replace('../burst/trial_page.h', 'trial_page.h')
                            .replace('../burst/burst_det.h', 'burst_det.h'))
                    put('trial_page.h', (REPO / 'app-sdk/apps/burst/trial_page.h').read_text())
                    put('burst_det.h', (REPO / 'app-sdk/apps/burst/burst_det.h').read_text())
                for landscape in ((False, True) if name in ('probe', 'dots') else (False,)):
                    label = name + ('-landscape' if landscape else '-portrait')
                    run = api('run' + query, {'landscape': landscape})
                    assert run['ok'] and run['started'], (label, run, api('logs'))
                    screen = Screen(args.port)
                    def frame(tag, predicate=lambda raw, text: True, seconds=40):
                        deadline = time.monotonic() + seconds
                        while time.monotonic() < deadline:
                            raw = screen.frame(); _, text = grid(raw)
                            if predicate(raw, text):
                                (out / (label + '-' + tag + '.cells')).write_bytes(raw)
                                (out / (label + '-' + tag + '.txt')).write_text(text, encoding='utf-8')
                                return raw, text
                        raise AssertionError((label, tag, text))
                    def stable(tag):
                        last, count = None, 0
                        def same(raw, text):
                            nonlocal last, count
                            key = app_key(raw)
                            count = count + 1 if key == last else 0
                            last = key
                            return count >= 2
                        return frame(tag, same, 45)[0]
                    try:
                        frame('start')
                        if name == 'probe':
                            tilt(x=45, y=45)
                            tilted, text = frame('clamped', lambda r,t: 'ANGLE=-30.00,30.00' in t)
                            acc = [float(v) for v in re.search(r'ACC=([\d.,-]+)', text)[1].split(',')]
                            assert abs(sum(v*v for v in acc) - 1) < .001 and acc[0] < 0 and acc[1] > 0
                            tilt('release')
                            _, text = frame('easing', lambda r,t: 'ANGLE=-30.00,30.00' not in t and 'ANGLE=-0.00,0.00' not in t)
                            frame('level', lambda r,t: 'ACC=-0.0000,0.0000,1.0000' in t)
                            screen.send('term tilt {"phase":"move","x":-30,"y":0}')
                            frame('socket', lambda r,t: 'ANGLE=30.00,0.00' in t)
                            replacement = Screen(args.port); screen.close(); screen = replacement
                            frame('reconnect', lambda r,t: 'ACC=0.0000,0.0000,1.0000' in t)
                            tilt('shake')
                            frame('shake', lambda r,t: 'GYRO=320' in t or 'GYRO=-320' in t)
                            frame('shake-end', lambda r,t: 'GYRO=0' in t)
                        elif name in ('dots', 'dice'):
                            before = stable('rest')
                            tilt(x=40, y=0)
                            changed, _ = frame('tilted', lambda r,t: app_key(r) != app_key(before))
                            # Hold long enough to move beyond the original resting spot.
                            time.sleep(.8)
                            tilt('release')
                            settled = stable('settled')
                            if name == 'dice':
                                time.sleep(.6); tilt('shake')
                                frame('shaken', lambda r,t: app_key(r) != app_key(settled))
                                stable('shake-settled')
                            assert app_key(changed) != app_key(before)
                            results.append({'case': label, 'rest': hashlib.sha256(app_key(before)).hexdigest(),
                                'tilted': hashlib.sha256(app_key(changed)).hexdigest(),
                                'settled': hashlib.sha256(app_key(settled)).hexdigest()})
                        else:
                            tilt(x=30, y=10); frame('tilt-no-motion-consumer'); tilt('release')
                        print('PASS', mode, label, flush=True)
                    finally:
                        screen.close(); api('stop', {})
            suffix = '' if len(args.only) == 4 else '-' + '-'.join(args.only)
            (out / ('results' + suffix + '.json')).write_text(json.dumps(results, indent=2))
            print('PASS tilt; evidence:', out, flush=True)
        finally:
            stop(proc)


if __name__ == '__main__': main()
