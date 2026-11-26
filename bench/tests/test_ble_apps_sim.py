"""BUGHUNT's tag watch and BLE EXPLORER in lssim, on the handheld and a wide screen.

BUGHUNT (which BLE WATCH was folded into) replays a generated advertising
capture (bughunt_tags_fixture.py, every frame made up) and must name the
trackers on TAGS, follow the AirTag's address change, raise the following
alert on any tab, HUNT a tag with BLE WATCH's meter and tone, write its CSV
log and say where BLE WATCH's old logs are.
BLE EXPLORER runs against the simulator's synthetic peripheral: connect,
walk the GATT table, decode the battery level, log notifications and save
the table as JSON.

    python bench/tests/test_ble_apps_sim.py path/to/lssim
"""
import json
import re
import os
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import bughunt_tags_fixture  # noqa: E402

EXE = Path(sys.argv[1]).resolve()
failures = []


# BUGHUNT 1.2's HUNT gauge scale: -100 on the left, the closeness word, -30 on the right
GAUGE = re.compile(r'-100 +[A-Z]+ +-30')

def check(what, ok, detail=''):
    print(('PASS ' if ok else 'FAIL ') + what)
    if not ok:
        failures.append(what)
        if detail:
            print(detail[-3000:] if 'lssim: lap' not in detail else detail[:detail.find('lssim: lap')][-3000:])


def run(app, folder, *args, script='', capture=None, hw='wifi,ble,imu,sd,desktop'):
    env = dict(os.environ, LAPSIM=app, LAPSIM_SCRIPT=script, LAPSIM_CONSENT='grant', LAPSIM_HW=hw)
    for k in ('LAPSIM_DLL', 'LSSIM_FRAMES', 'LAPSIM_CAPTURE', 'LAPSIM_CAPTURE_PACE', 'LAPSIM_RADIO'):
        env.pop(k, None)
    if capture:
        env['LAPSIM_CAPTURE'] = str(capture)
        env['LAPSIM_CAPTURE_PACE'] = '0'
    r = subprocess.run([str(EXE), 'lap', '-A', '40', '-d', '-o', str(folder / 'out.bmp'), *args], cwd=folder, env=env,
                       capture_output=True, text=True, errors='replace', timeout=120)
    text = r.stdout + r.stderr
    check('%s %s exits cleanly' % (app, ' '.join(args)), r.returncode == 0 and 'simulator contract:' not in text, text)
    return text


with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    cap = tmp / 'watch.pcapng'
    cap.write_bytes(bughunt_tags_fixture.build())
    TAGS = '5:key=@right;6:key=@right'      # SWEEP > FINDINGS > TAGS

    # ---- BUGHUNT's TAGS tab, handheld portrait --------------------------------
    d = tmp / 'tags_portrait'
    old = d / 'sdcard' / 'ble_watch'
    old.mkdir(parents=True)
    old_log = old / 'watch-20261101-090000.csv'
    old_log.write_text('uptime_s,event,address,addr_type,class,company,rssi,sightings_5m,span_s,moved_m,rotations,name\n')
    t = run('bughunt', d, '-f', '120', script=TAGS, capture=cap)
    for word in ('TAGS', 'FINDMY', 'AirTag?', 'TILE', 'GFMDN', 'SMTAG', 'FPAIR', 'AirPods Pro', 'rot 1 AWAY'):
        check('tags lists ' + word, word in t, t)
    check('tags raises the following alert', 'FOLLOWING' in t and 'with you for' in t and 'tap: HUNT' in t, t)
    check('the owner-near Find My device does not alert', '!FINDMY Apple device?' not in t, t)
    check('the pick\'s detail is under the list', 'through rotations' in t and 'RSSI, last 120 s' in t, t)
    logs = list(d.glob('sdcard/bughunt/tags-*.csv'))
    check('tags writes one CSV log', len(logs) == 1, str(logs))
    if logs:
        rows = logs[0].read_text().splitlines()
        check('the log starts with its header', rows[0].startswith('uptime_s,event,address'), rows[0])
        alerts = [r for r in rows if ',alert,' in r]
        check('the log has the alerts', len(alerts) >= 2, '\n'.join(rows[:40]))
        check('the AirTag alert carries its rotation', any('AirTag?' in r and r.split(',')[10] == '1' for r in alerts),
              '\n'.join(alerts))
        check('every log line has the header\'s columns', all(r.count(',') == rows[0].count(',') for r in rows), '')
    check('BLE WATCH\'s old log is left where it was', old_log.exists(), '')

    # the alert comes on any tab: SWEEP, nothing pressed
    d = tmp / 'tags_on_sweep'
    d.mkdir()
    t = run('bughunt', d, '-f', '120', capture=cap)
    check('the following alert shows on SWEEP', 'with you for' in t and 'tap: HUNT' in t and 'CHOOSE A SWEEP' in t, t)
    # and BLE WATCH's L key hunts the alert's tag
    t = run('bughunt', d, '-f', '260', script='100:key=l', capture=cap)
    check('L hunts the alert\'s tag', 'HUNTING' in t and 'closer = louder' in t and 'GFMDN' in t and GAUGE.search(t), t)
    check('hunting it clears the banner', 'tap: HUNT' not in t and 'FOLLOWING' in t, t)

    # ALL: every device, with advert types
    d = tmp / 'tags_all'
    d.mkdir()
    t = run('bughunt', d, '-f', '120', script=TAGS + ';8:key=v', capture=cap)
    for word in ('Pixel 8', 'Forerunner 265', 'ESP32-C6 sensor', 'iBeacon', 'Eddystone URL', 'Samsung', 'Microsoft', 'NONC'):
        check('tags ALL shows ' + word, word in t, t)
    check('tags ALL counts 15 devices (one rotated)', 'ALL DEVICES 15' in t, t)

    # HUNT on the pick (BLE WATCH's LOCATE), and INFO with BLE WATCH's old logs
    d = tmp / 'tags_hunt'
    old = d / 'sdcard' / 'ble_watch'
    old.mkdir(parents=True)
    (old / 'watch-20261101-090000.csv').write_text('x\n')
    (old / 'watch-20261102-100000.csv').write_text('x\n')
    t = run('bughunt', d, '-f', '40', script=TAGS + ';20:key=@enter', capture=cap)
    check('HUNT a tag shows BLE WATCH\'s meter and tone', 'dBm' in t and 'close' in t and GAUGE.search(t)
          and 'TONE' in t and 'LAST' in t, t)
    t = run('bughunt', d, '-f', '40', script=TAGS + ';8:key=v;9:key=v', capture=cap)
    check('tags INFO names the capture source', 'advert capture (API 2.7)' in t and 'alerts go by time' in t, t)
    check('tags INFO finds BLE WATCH\'s old logs', 'BLE WATCH' in t and '2 in ble_watch, kept' in t
          and 'watch-20261102-100000.csv' in t, t)

    # settings changed in a hurry: each one is saved, and the saves (card jobs
    # an app has four of) must not starve the log, which shares them
    d = tmp / 'tags_saves'
    d.mkdir()
    t = run('bughunt', d, '-f', '200', capture=cap,
            script=TAGS + ';8:key=-;9:key=-;10:key=p;11:key=+;12:key=+;13:key=p;14:key=-;15:key=v;16:key=v')
    check('quick settings changes leave the log on', 'card      on' in t and 'after     5 min present' in t, t)
    logs = list(d.glob('sdcard/bughunt/tags-*.csv'))
    rows = logs[0].read_text().splitlines() if logs else []
    check('and it is still written to the end', len(logs) == 1 and sum(',alert,' in r for r in rows) >= 2
          and sum(',seen,' in r for r in rows) >= 20, str(len(rows)))
    saves = list(d.glob('sdcard/apps_data/bughunt/*'))
    check('the last setting reached the card', len(saves) >= 1, str(saves))
    t = run('bughunt', d, '-f', '20', script=TAGS + ';8:key=v;9:key=v')
    check('and comes back on the next open', 'after     5 min present' in t, t)

    # wide screen: the list right, the pick's detail left
    d = tmp / 'tags_wide'
    d.mkdir()
    t = run('bughunt', d, '-f', '120', '-l', script=TAGS, capture=cap)
    check('tags wide shows list columns and the detail', '5MIN' in t and 'HERE' in t and 'random static' in t and 'THE PICK' in t, t)

    # no capture file: the scan fallback, and no Bluetooth at all
    d = tmp / 'tags_scan'
    d.mkdir()
    t = run('bughunt', d, '-f', '60', script=TAGS + ';8:key=v;9:key=v')
    check('tags falls back to the scan without a capture', 'BLE scan' in t, t)
    t = run('bughunt', d, '-f', '20', script=TAGS, hw='sd')
    check('tags says when there is no Bluetooth', 'no Bluetooth' in t, t)

    # ---- BLE EXPLORER against the simulator's synthetic peripheral -------------
    d = tmp / 'explorer_portrait'
    d.mkdir()
    t = run('ble_explorer', d, '-f', '200', script='10:key=c;40:key=@down;42:key=n;150:key=s')
    for word in ('Battery 180f', 'Battery Level', 'RWN', 'Client Char Config', 'notifications on', 'ready', 'NOTIFY'):
        check('explorer shows ' + word, word in t, t)
    check('explorer decodes the battery level', '87 %' in t or '86 %' in t, t)
    saved = list(d.glob('sdcard/ble_explorer/gatt-*.json'))
    check('explorer saves one GATT table', len(saved) == 1, str(saved))
    if saved:
        try:
            table = json.loads(saved[0].read_text())
        except ValueError as e:
            table = {}
            check('the saved table is JSON', False, str(e))
        svc = table.get('services', [{}])[0]
        chars = svc.get('characteristics', [{}])
        check('the JSON names the service and characteristic',
              svc.get('name') == 'Battery' and chars[0].get('name') == 'Battery Level', json.dumps(table)[:800])
        check('the JSON has the descriptor and its value',
              chars[0].get('descriptors', [{}])[0].get('decoded') == 'notifications on', json.dumps(table)[:800])
        check('the JSON carries the notification log', len(table.get('notifications', [])) >= 3, json.dumps(table)[:800])
        check('the walk is marked complete', table.get('complete') is True, json.dumps(table)[:300])

    # a second connection where the board grants one: refused politely, the first kept
    d = tmp / 'explorer_limit'
    d.mkdir()
    t = run('ble_explorer', d, '-f', '120',
            script='1:blead=-95,0b09466f726572756e6e6572;40:key=c;70:key=@left;72:key=@down;74:key=c')
    check('explorer learns the board allows one connection', 'Only 1 app connection on this board' in t, t)
    # the note sits over the title for 8 s; once it has gone the link count shows again
    t = run('ble_explorer', d, '-f', '340',
            script='1:blead=-95,0b09466f726572756e6e6572;40:key=c;70:key=@left;72:key=@down;74:key=c')
    check('the first connection stays', '1/1 links' in t, t)

    # wide screen: devices, table and notifications side by side
    d = tmp / 'explorer_wide'
    d.mkdir()
    t = run('ble_explorer', d, '-f', '160', '-l', script='10:key=c;40:key=@down;42:key=n')
    check('explorer wide shows devices, table and log', 'DEVICES' in t and 'Battery Level' in t and 'DISCONNECT' in t
          and 'TIME  # HANDLE' in t, t)

    d = tmp / 'explorer_none'
    d.mkdir()
    t = run('ble_explorer', d, '-f', '20', hw='sd')
    check('explorer says when there is no Bluetooth', 'Bluetooth not fitted' in t, t)

print('%d failed' % len(failures))
sys.exit(1 if failures else 0)
