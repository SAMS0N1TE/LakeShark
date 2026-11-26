"""WIFI WATCH and WIFI SURVEY over headless lssim (build lssim first; set
LS_BENCH_BUILD to its build folder). Every capture is generated here by
pcapng_gen: synthetic 802.11 with radiotap, nothing from the air."""
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import unittest
SDK = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SDK))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import lsdk
from pcapng_gen import Pcapng, mac, BCAST, beacon, probe_req, deauth, assoc_req, eapol_key, qos_data
BUILD = Path(os.environ.get('LS_BENCH_BUILD') or SDK.parent / 'bench/build')

AP = mac('02:aa:00:00:00:01')       # TEST-LakeNet, WPA2
TWIN = mac('02:ee:00:00:00:66')     # TEST-LakeNet again, open
HIDDEN = mac('02:aa:00:00:00:03')
CAFE = mac('02:aa:00:00:00:02')     # TEST-OpenCafe, open, channel 1
STA = mac('02:bb:00:00:00:10')
PHONE = mac('da:a1:19:00:00:01')    # locally administered: randomised
LAPTOP = mac('00:1b:63:00:00:02')   # global


def watch_capture():
    """Ten seconds of a channel with every kind of trouble WIFI WATCH looks for."""
    p = Pcapng()
    t = 1_000_000
    for i in range(10):                               # quiet start: two networks and a hidden one
        p.wifi(t + i * 102_400, beacon(AP, 'TEST-LakeNet', 6, 'wpa2', seq=i), ch=6, rssi=-48)
        p.wifi(t + i * 102_400 + 300, beacon(HIDDEN, 'TEST-Secret', 6, 'wpa2', seq=i, hidden=True), ch=6, rssi=-60)
        p.wifi(t + i * 102_400 + 600, beacon(CAFE, 'TEST-OpenCafe', 1, 'open', seq=i), ch=1, rssi=-70)
    t = 2_200_000
    p.wifi(t, probe_req(PHONE, '', seq=1), ch=6, rssi=-55)
    p.wifi(t + 1000, probe_req(PHONE, 'TEST-HomeNet', seq=2), ch=6, rssi=-55)
    p.wifi(t + 2000, probe_req(LAPTOP, 'TEST-Office', seq=3), ch=6, rssi=-62)
    p.wifi(t + 3000, probe_req(LAPTOP, 'TEST-Office', seq=4), ch=6, rssi=-62)
    # the hidden network answers a probe by name
    p.wifi(t + 10_000, beacon(HIDDEN, 'TEST-Secret', 6, 'wpa2', seq=20, probe_resp_to=LAPTOP), ch=6, rssi=-60)
    # a handshake, message 1 carrying a PMKID (test pattern)
    t = 2_500_000
    for m in (1, 2, 3, 4):
        p.wifi(t + m * 5000, eapol_key(m, AP, STA, pmkid=(m == 1), seq=40 + m), ch=6, rssi=-50)
    # an open twin of TEST-LakeNet
    t = 3_000_000
    for i in range(3):
        p.wifi(t + i * 102_400, beacon(TWIN, 'TEST-LakeNet', 6, 'open', seq=i), ch=6, rssi=-40)
    # a deauthentication burst: 25 frames in half a second, to everyone
    t = 3_500_000
    for i in range(25):
        p.wifi(t + i * 20_000, deauth(AP, BCAST, AP, reason=7, seq=100 + i), ch=6, rssi=-47)
    # a disassociation burst to one station
    t = 4_200_000
    for i in range(12):
        p.wifi(t + i * 30_000, deauth(AP, STA, AP, reason=8, seq=200 + i, disassoc=True), ch=6, rssi=-47)
    # a beacon flood: 60 made-up networks in two seconds
    t = 5_000_000
    for i in range(60):
        b = bytes([0x02, 0xf1, 0x00, 0x00, i // 256, i % 256])
        p.wifi(t + i * 33_000, beacon(b, 'TEST-Flood%02d' % i, 6, 'open', seq=i), ch=6, rssi=-65)
    # ordinary data afterwards
    for i in range(5):
        p.wifi(7_500_000 + i * 10_000, qos_data(STA, AP, AP, seq=300 + i), ch=6, rssi=-50)
    return p


def survey_capture():
    """Beacons on channels 1, 6 and 11 with traffic on 6, as hopping hears them."""
    p = Pcapng()
    t = 1_000_000
    for i in range(30):
        p.wifi(t + i * 100_000, beacon(AP, 'TEST-LakeNet', 6, 'wpa2', seq=i), ch=6, rssi=-48 - (i % 3))
        p.wifi(t + i * 100_000 + 200, beacon(CAFE, 'TEST-OpenCafe', 1, 'open', seq=i), ch=1, rssi=-71)
        p.wifi(t + i * 100_000 + 400, beacon(mac('02:aa:00:00:00:0b'), 'TEST-Library', 11, 'wpa3', seq=i), ch=11, rssi=-63)
        for k in range(4):
            p.wifi(t + i * 100_000 + 1000 + k * 300, qos_data(STA, AP, AP, n=600, seq=i * 4 + k), ch=6, rssi=-52, rate=108)
    return p


# A crowded block of flats: 30 access points over channels 1-13, most on
# 1, 6 and 11, a few on the channels between, from -38 to -90 dBm.
CROWD = [
    ('TEST-Attic', 1, -52), ('TEST-Bakery', 1, -64), ('TEST-Cellar', 1, -77), ('TEST-Dojo', 1, -86),
    ('TEST-Elm', 2, -81), ('TEST-Fern', 3, -70), ('TEST-Garage', 3, -88), ('TEST-Harbor', 4, -74),
    ('TEST-Igloo', 5, -83), ('TEST-Jasmine', 6, -38), ('TEST-Kiosk', 6, -55), ('TEST-Lantern', 6, -61),
    ('TEST-Maple', 6, -69), ('TEST-Nook', 6, -79), ('TEST-Oasis', 6, -90), ('TEST-Pier', 7, -66),
    ('TEST-Quarry', 8, -72), ('TEST-Raven', 8, -85), ('TEST-Studio', 9, -58), ('TEST-Tower', 10, -76),
    ('TEST-Umbra', 11, -44), ('TEST-Vault', 11, -57), ('TEST-Willow', 11, -63), ('TEST-Xylem', 11, -73),
    ('TEST-Yard', 11, -84), ('TEST-Zinc', 12, -68), ('TEST-Alcove', 12, -80), ('TEST-Brook', 13, -60),
    ('TEST-Cabin', 13, -75), ('TEST-Dune', 13, -89),
]


def crowd_capture():
    """CROWD's beacons, each AP a BSSID of its own, as hopping 1-13 hears them."""
    p = Pcapng()
    t = 1_000_000
    for i in range(12):
        for k, (name, ch, rssi) in enumerate(CROWD):
            b = bytes([0x02, 0xcc, 0x00, 0x00, 0x00, k + 1])
            p.wifi(t + i * 100_000 + k * 300, beacon(b, name, ch, ('wpa2', 'wpa3', 'open')[k % 3], seq=i), ch=ch, rssi=rssi - (i % 2))
    return p


class WifiApps(unittest.TestCase):
    def run_app(self, app, script, capture=None, frames=120, grid=None, landscape=False, taps=None, **more):
        with tempfile.TemporaryDirectory(dir=BUILD) as d:
            env = dict(lsdk.host_env(), LAPSIM=app, LAPSIM_SCRIPT=script, LAPSIM_CONSENT='grant', LAPSIM_RADIO='none', **more)
            for k in ('LAPSIM_DLL', 'LSSIM_FRAMES', 'LAPSIM_NFC', 'LAPSIM_HW', 'LAPSIM_CAPTURE', 'LSSIM_GRID'):
                if k not in more:
                    env.pop(k, None)
            if grid:
                env['LSSIM_GRID'] = grid
            if capture:
                path = Path(d) / 'capture.pcapng'
                capture.save(path)
                env['LAPSIM_CAPTURE'] = str(path)
            exe = BUILD / ('lssim.exe' if os.name == 'nt' else 'lssim')
            args = [str(exe), 'lap', '-A', '40', '-f', str(frames), '-d'] + (['-l'] if landscape else []) + (['-x', taps] if taps else [])
            run = subprocess.run(args, cwd=d, env=env, capture_output=True, text=True, errors='replace', timeout=120,
                                 creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
            text = run.stdout + run.stderr
            self.assertEqual(run.returncode, 0, text)
            logs = {p.name: p.read_bytes().decode('utf-8', 'replace') for p in Path(d).glob('sdcard/**/*.csv')}
            return text, logs

    # ---- WIFI WATCH ----------------------------------------------------
    def test_watch_raises_each_alert_and_logs_them(self):
        text, logs = self.run_app('wifi_watch', '', watch_capture(), frames=260)
        self.assertRegex(text, r' ALERTS .*\n.* 1/6 ')           # the handheld's tab strip: name, then i/n
        self.assertNotIn('NOT LISTENING', text)
        self.assertEqual(len(logs), 1, text)
        log = next(iter(logs.values()))
        self.assertTrue(log.startswith('time,severity,kind,channel,source,target,detail\r\n'), log[:80])
        for kind in ('DEAUTH', 'DISASSOC', 'FLOOD', 'TWIN', 'HIDDEN', 'HANDSHAKE', 'PMKID'):
            self.assertIn(',' + kind + ',', log, kind)
        self.assertIn('Deauth burst to broadcast', log)
        self.assertIn('reason 7', log)
        self.assertIn('Evil twin? ""TEST-LakeNet"" open here, WPA2 on 02:aa:00:00:00:01', log)
        self.assertIn('Hidden network is ""TEST-Secret""', log)
        self.assertIn('02:aa:00:00:00:01,02:bb:00:00:00:10', log)   # handshake AP and station
        self.assertNotIn('0102030405060708', log.replace(':', ''))  # the PMKID pattern is never written

    def test_watch_counts_and_probe_table(self):
        text, _ = self.run_app('wifi_watch', '200:key=2', watch_capture(), frames=260)
        self.assertRegex(text, r' COUNTERS .*\n.* 2/6 ')
        self.assertRegex(text, r'Deauthentication +25 ')
        self.assertRegex(text, r'Disassociation +12 ')
        text, _ = self.run_app('wifi_watch', '200:key=5', watch_capture(), frames=260)
        self.assertIn('da:a1:19:00:00:01 R', text)               # randomised address flagged
        self.assertIn('00:1b:63:00:00:02  ', text)
        self.assertIn('"TEST-HomeNet" (any)', text)

    def test_watch_handshake_is_only_marked(self):
        text, _ = self.run_app('wifi_watch', '200:key=6', watch_capture(), frames=260)
        self.assertIn('M1234 PMKID', text)
        self.assertIn('Seen, not kept', text)

    def test_watch_without_transport_says_how(self):
        text, logs = self.run_app('wifi_watch', '', watch_capture(), frames=40, LAPSIM_CAPTURE_NO_TRANSPORT='1')
        self.assertIn('NOT LISTENING', text)
        self.assertIn('C6 monitor firmware', text)
        self.assertIn('SYSTEM > UPDATE > C6 RADIO FW', text)
        self.assertEqual(logs, {})

    def test_watch_on_a_network_falls_back_to_its_channel(self):
        text, _ = self.run_app('wifi_watch', '', watch_capture(), frames=60, LAPSIM_CAPTURE_JOINED='1')
        self.assertIn('LINK ch', text)
        self.assertIn('network: watching its', text)

    def test_watch_desktop(self):
        text, _ = self.run_app('wifi_watch', '', watch_capture(), frames=260, grid='160x45')
        self.assertIn(' ALERTS ', text)
        self.assertIn(' HANDSHAKES 1 ', text)

    # ---- WIFI WATCH: alert delivery (LoRa mesh, sound, vibrate) ----------
    def run_watch(self, folder, script, capture, frames=260, taps=None, prefs=None):
        """WIFI WATCH in `folder`, which keeps sdcard/apps_data between runs.
        Returns the cell grid and console, and the mesh notices it handed over."""
        env = dict(lsdk.host_env(), LAPSIM='wifi_watch', LAPSIM_SCRIPT=script, LAPSIM_CONSENT='grant', LAPSIM_RADIO='none')
        for k in ('LAPSIM_DLL', 'LSSIM_FRAMES', 'LAPSIM_NFC', 'LAPSIM_HW', 'LSSIM_GRID'):
            env.pop(k, None)
        path = Path(folder) / 'capture.pcapng'
        capture.save(path)
        env['LAPSIM_CAPTURE'] = str(path)
        if prefs is not None:
            data = Path(folder) / 'sdcard' / 'apps_data' / 'wifi_watch'
            data.mkdir(parents=True, exist_ok=True)
            (data / 'alerts.cfg').write_bytes(prefs)
        exe = BUILD / ('lssim.exe' if os.name == 'nt' else 'lssim')
        args = [str(exe), 'lap', '-A', '40', '-f', str(frames), '-d'] + (['-x', taps] if taps else [])
        run = subprocess.run(args, cwd=folder, env=env, capture_output=True, text=True, errors='replace', timeout=120,
                             creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        text = run.stdout + run.stderr
        self.assertEqual(run.returncode, 0, text)
        return text, re.findall(r'\[app wifi_watch\] mesh ([^:]*): (.*)', text)

    @staticmethod
    def watch_prefs(mesh_to=0, min_sev=2, gap_min=10, per_hour=6, out=(0, 0, 3), speak=0, banner=0):
        """watch_prefs_t as main.c writes it (watch.h): 60 bytes, little endian."""
        b = struct.pack('<IHHBBHB3BBBBBBB17s20s', 0x31505757, 60, 1, mesh_to, min_sev, gap_min, per_hour, *out,
                        speak, banner, 0, 22, 7, 1, b'', b'')
        return b + b'\0' * (60 - len(b))

    def flood_capture(self):
        """Deauthentication bursts from three sources in three seconds, and the
        first source again after its ten seconds: four DEAUTH alerts."""
        p = Pcapng()
        for i in range(10):
            p.wifi(1_000_000 + i * 102_400, beacon(AP, 'TEST-LakeNet', 6, 'wpa2', seq=i), ch=6, rssi=-48)
        for n, (t, src) in enumerate(((2_000_000, AP), (3_000_000, CAFE), (4_000_000, HIDDEN), (15_000_000, AP))):
            for i in range(15):
                p.wifi(t + i * 20_000, deauth(src, BCAST, src, reason=7, seq=n * 100 + i), ch=6, rssi=-47)
        return p

    def test_watch_mesh_off_sends_nothing(self):
        with tempfile.TemporaryDirectory(dir=BUILD) as d:
            text, sent = self.run_watch(d, '0:mesh', watch_capture())        # a mesh, armed: still nothing
            self.assertEqual(sent, [], text)
            self.assertIn('ALERTS', text)
            text, sent = self.run_watch(d, '0:mesh;200:key=t', watch_capture())
            self.assertEqual(sent, [], text)
            self.assertIn('LoRa off', text)                            # TEST says so and sends nothing

    def test_watch_mesh_rate_limit_and_coalesce(self):
        with tempfile.TemporaryDirectory(dir=BUILD) as d:
            # channel 1, alerts only, one per kind a minute: 70 s of a deauthentication flood
            text, sent = self.run_watch(d, '0:mesh', self.flood_capture(), frames=1750, prefs=self.watch_prefs(mesh_to=2, gap_min=1, per_hour=30))
            deauth_sent = [m for dest, m in sent if ' DEAUTH' in m]
            self.assertEqual(len(deauth_sent), 2, sent)                # four alerts, two notices
            self.assertRegex(deauth_sent[0], r'^WIFI WATCH DEAUTH 02:aa:00:00:00:01>bcast ch6 12:00$')
            self.assertRegex(deauth_sent[1], r'^WIFI WATCH DEAUTH x3 02:aa:00:00:00:01>bcast ch6 12:00$')   # the newest stands for three
            self.assertTrue(all(dest == 'CH1 (open)' for dest, _ in sent), sent)
            self.assertTrue(all(len(m) <= 150 for _, m in sent))
            # the hourly cap: one notice an hour in all, whatever the kinds
            text, sent = self.run_watch(d, '0:mesh', watch_capture(), prefs=self.watch_prefs(mesh_to=1, gap_min=1, per_hour=1))
            self.assertEqual(len(sent), 1, sent)
            self.assertEqual(sent[0][0], 'CH0 public')

    def test_watch_mesh_says_why_it_cannot_send(self):
        with tempfile.TemporaryDirectory(dir=BUILD) as d:
            down = ';'.join('%d:key=@down' % f for f in range(201, 251))
            text, sent = self.run_watch(d, '200:key=2;' + down, watch_capture(), frames=270, prefs=self.watch_prefs(mesh_to=2))
            self.assertEqual(sent, [])                                  # no mesh in this simulator run
            self.assertIn('no LoRa mesh', text)
            self.assertIn('held', text)                                # the alerts wait for it

    def test_watch_options_persist(self):
        with tempfile.TemporaryDirectory(dir=BUILD) as d:
            # MORE > ALERT OPTIONS (o), LORA MESH, then SEND TO steps OFF -> CH0 PUBLIC
            text, _ = self.run_watch(d, '0:mesh;200:key=o', watch_capture(), taps='~3,20:11,~3,20:11,~3')
            self.assertIn('WIFI WATCH > LORA MESH', text)
            self.assertRegex(text, r'SEND TO .*CH0 PUBLIC')
            cfg = Path(d) / 'sdcard' / 'apps_data' / 'wifi_watch' / 'alerts.cfg'
            self.assertTrue(cfg.exists())
            self.assertEqual(cfg.read_bytes()[8], 1)                   # mesh_to: MESH_CH0
            # a new session reads it back, and sends there
            text, sent = self.run_watch(d, '0:mesh;200:key=o', watch_capture(), taps='~3,20:11,~3')
            self.assertRegex(text, r'SEND TO .*CH0 PUBLIC')
            self.assertTrue(sent and all(dest == 'CH0 public' for dest, _ in sent), sent)

    def test_watch_test_alert_runs_each_output(self):
        with tempfile.TemporaryDirectory(dir=BUILD) as d:
            text, sent = self.run_watch(d, '0:mesh;200:key=t', watch_capture(),
                                        prefs=self.watch_prefs(mesh_to=3, min_sev=2, out=(0, 0, 3), speak=1))
            self.assertIn(('CH2 (open)', 'WIFI WATCH TEST ch6 12:00'), sent)
            self.assertIn('lap_say: Wi-Fi watch test alert', text)
            self.assertRegex(text, r'test: sound, no motor, spoken, LoRa: sent to CH2')   # the simulator has no motor
            self.assertIn('{x}', text)                                 # and the header says so

    def test_watch_content_taps_never_open_the_menus(self):
        # "Touching in probes opens the settings": a tap in a tab's list must not
        # fall through to the board, which turns an unused tap into ENTER (MORE).
        # -x taps run from the first frame: pick the tab at frame 1, tap once
        # into its list (rows 27-48 are the band the board would make ENTER)
        # after the alerts are in, and look at the very next frames.
        for tab in range(1, 7):
            for at in ('20:40', '8:30', '45:46'):
                with tempfile.TemporaryDirectory(dir=BUILD) as d:
                    text, _ = self.run_watch(d, '1:key=%d' % tab, watch_capture(), frames=160, taps='~150,%s,~3' % at)
                    self.assertNotIn('CHOOSE', text, 'tab %d tap %s' % (tab, at))
                    self.assertNotIn('OPTIONS-', text, 'tab %d tap %s' % (tab, at))
                    self.assertRegex(text, r' %d/6 ' % tab)            # still on its tab

    # ---- WIFI SURVEY ---------------------------------------------------
    def test_survey_lists_scan_and_beacons(self):
        text, _ = self.run_app('wifi_survey', '5:wifi=8', survey_capture(), frames=150)
        self.assertIn('TEST-LakeNet', text)
        self.assertIn('02:aa:00:00:00:01', text)                 # BSSID from its beacons
        self.assertIn('WPA3', text)
        self.assertIn('TEST-Printer', text)                      # from the scan only

    def test_survey_channels_and_traffic(self):
        text, _ = self.run_app('wifi_survey', '5:wifi=8;140:key=2', survey_capture(), frames=150)
        self.assertIn('2/5', text)                               # the < CHANNELS > strip
        self.assertIn('+- CHANNELS -', text)                      # its panel, the name on the rule
        # the plot fills the first page; DOWN pages to the tables under it
        text, _ = self.run_app('wifi_survey', '5:wifi=8;140:key=2', survey_capture(), frames=150, taps='22:66')
        self.assertIn('5 GHz', text)                             # TEST-Home5G on 36
        text, _ = self.run_app('wifi_survey', '5:wifi=8;140:key=3', survey_capture(), frames=150)
        self.assertIn('3/5', text)
        self.assertIn('+- TRAFFIC ', text)
        self.assertRegex(text, r' 6 +1\d\d ')                    # 120 data + beacons heard on channel 6

    def test_survey_history_and_csv(self):
        text, logs = self.run_app('wifi_survey', '5:wifi=8;30:wifiwalk=3;60:wifiwalk=3;90:key=4;120:key=w', survey_capture(), frames=150)
        self.assertIn('4/5', text)
        self.assertIn('+- HISTORY -', text)
        names = sorted(logs)
        self.assertEqual(len(names), 2, names)
        aps = logs[[n for n in names if n.endswith('-aps.csv')][0]]
        chans = logs[[n for n in names if n.endswith('-channels.csv')][0]]
        self.assertTrue(aps.startswith('ssid,bssid,channel,band,rssi,rssi_min,rssi_max,security,heard_by,first_s,last_s\r\n'), aps[:100])
        self.assertIn('TEST-LakeNet,02:aa:00:00:00:01,6,2.4,', aps)
        self.assertIn('TEST-Home5G,,36,5,', aps)
        self.assertTrue(chans.startswith('channel,mhz,aps,strongest_dbm,frames,airtime_ms,listened_ms,busy_percent\r\n'), chans[:100])

    def test_survey_without_capture_still_scans(self):
        text, _ = self.run_app('wifi_survey', '5:wifi=8;100:key=3', survey_capture(), frames=110, LAPSIM_CAPTURE_NO_TRANSPORT='1')
        self.assertIn('SYSTEM > UPDATE > C6 RADIO FW', text)
        text, _ = self.run_app('wifi_survey', '5:wifi=8', survey_capture(), frames=60, LAPSIM_CAPTURE_NO_TRANSPORT='1')
        self.assertIn('TEST-LakeNet', text)

    def test_survey_desktop(self):
        text, _ = self.run_app('wifi_survey', '5:wifi=8', survey_capture(), frames=150, grid='160x45')
        self.assertIn('BSSID', text)
        self.assertIn('Security', text)

    # ---- WIFI SURVEY: RECORD, the survey recorder that was LINK's ---------
    WIGLE_HEAD = ('MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,'
                  'AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type\n')

    def sessions(self, logs):
        return {n: v for n, v in logs.items() if re.fullmatch(r'\d{8}_\d{6}(_\d{3})?\.csv', n)}

    def test_record_writes_wigle_rows_and_summary(self):
        script = '2:gpsfix=-34.6,-58.4,25;3:key=5;4:key=r;150:key=r'
        text, logs = self.run_app('wifi_survey', script, crowd_capture(), frames=260, LAPSIM_HW='wifi,ble,gps,sd')
        self.assertIn('5/5', text)
        self.assertIn('Session saved', text)
        found = self.sessions(logs)
        self.assertEqual(len(found), 1, sorted(logs))
        name, rows = next(iter(found.items()))
        self.assertTrue(rows.startswith('WigleWifi-1.6,appRelease=wifi_survey-'), rows[:80])
        self.assertIn('\n' + self.WIGLE_HEAD, rows)
        self.assertRegex(rows, r'\n02:cc:00:00:00:0a,"TEST-Jasmine",\[WPA2-PSK-CCMP\]\[ESS\],2026-10-23 12:00:\d\d,6,2437,-3\d,'
                               r'-34\.6000000,-58\.4000000,25\.0,5\.0,,,WIFI\n')
        self.assertRegex(rows, r'\n02:cc:00:00:00:0b,"TEST-Kiosk",\[WPA3-SAE-CCMP\]\[ESS\],')
        self.assertRegex(rows, r'\n06:05:04:03:02:01,"",\[BLE\],2026-10-23 12:00:\d\d,0,0,-\d+,-34\.6000000,')
        summary = logs[name[:-4] + '_summary.csv']
        self.assertTrue(summary.startswith('MAC,Type,AddressType,SSID,Samples,EstimatedLatitude,EstimatedLongitude,Weight\n'))
        self.assertRegex(summary, r'\n02:cc:00:00:00:0a,WIFI,0,"TEST-Jasmine",\d+,-34\.6000000,-58\.4000000,')
        self.assertIn('\n06:05:04:03:02:01,BLE,0,"",', summary)

    def test_record_needs_a_fix_or_says_why(self):
        text, logs = self.run_app('wifi_survey', '3:key=5;4:key=r', survey_capture(), frames=40)
        self.assertIn('no GPS here: turn ONLY GPS FIX off', text)
        self.assertFalse(self.sessions(logs))
        # ONLY GPS FIX off (MORE on RECORD, its fourth row): rows without a position, a scan alone is not one
        script = '3:key=5;4:key=m;6:dialogpick=3;8:dialogcancel;10:key=r;120:key=r'
        text, logs = self.run_app('wifi_survey', '1:wifi=8;' + script, survey_capture(), frames=200)
        found = self.sessions(logs)
        self.assertEqual(len(found), 1, sorted(logs))
        rows = next(iter(found.values()))
        self.assertRegex(rows, r'\n02:aa:00:00:00:01,"TEST-LakeNet",\[WPA2-PSK-CCMP\]\[ESS\],2026-10-23 12:00:\d\d,6,2437,-\d+,,,,,,,WIFI\n')
        self.assertRegex(rows, r'\n02:aa:00:00:00:0b,"TEST-Library",\[WPA3-SAE-CCMP\]\[ESS\],')
        self.assertNotIn('TEST-Printer', rows)                   # heard by scan only: no BSSID, no row

    def test_record_keeps_going_behind_home(self):
        script = '2:gpsfix=-34.6,-58.4,25;3:key=5;4:key=r;40:hide;100:services;120:show'
        text, _ = self.run_app('wifi_survey', script, crowd_capture(), frames=160, LAPSIM_HW='wifi,ble,gps,sd')
        m = re.search(r'service id=wifi_survey shown=0 ticks=(\d+)', text)
        self.assertTrue(m and int(m.group(1)) > 0, text[-2000:])  # the tick ran with the screen closed
        self.assertIn('+- RECORD LIVE', text)                    # reopened on RECORD, still recording
        self.assertIn('Keeps recording behind HOME', text)

    def test_record_landscape_and_desk(self):
        script = '2:gpsfix=-34.6,-58.4,25;3:key=5;4:key=r'
        text, _ = self.run_app('wifi_survey', script, crowd_capture(), frames=60, landscape=True, LAPSIM_HW='wifi,ble,gps,sd')
        self.assertIn('WI-FI APs', text)
        self.assertRegex(text, r'REC 00:00:0\d')
        text, _ = self.run_app('wifi_survey', script, crowd_capture(), frames=60, grid='160x45', LAPSIM_HW='wifi,ble,gps,sd')
        self.assertRegex(text, r'NAME +ADDRESS +SECURITY +SIGNAL')
        self.assertRegex(text, r'W TEST-Jasmine +02:cc:00:00:00:0a WPA2 ch6 +-3\d dBm GPS')

    # ---- WIFI SURVEY: the CHANNELS plot with a crowd ---------------------
    # Screen rows in the -d dump: the board's own bar takes the top rows, the
    # app's below; in portrait the plot runs from row 22 to its axis.
    def plot_labels(self, text):
        """The names on the plot: (row, first column, text) for each, from
        the row under the '2.4 GHz' line down to the axis."""
        rows = text.splitlines()
        start = next(i for i, r in enumerate(rows) if '2.4 GHz' in r)
        end = next(i for i in range(start, len(rows)) if re.search(r'-{20,}', rows[i]))
        out = []
        for i in range(start + 1, end):
            body = rows[i][4:] if len(rows[i]) > 4 else ''
            if '>' in body[:4] and ' ch ' in body:
                continue                                          # the picked AP's line
            for m in re.finditer(r'(TEST-[A-Za-z]*)( -\d+)?', body):
                out.append((i, m.start(), m.group(0)))
        return out

    def assert_labels_readable(self, labels, least, most):
        names = [n for n, _, _ in CROWD]
        self.assertGreaterEqual(len(labels), least, labels)
        self.assertLessEqual(len(labels), most, labels)
        for _, _, t in labels:                                   # never one name over another
            self.assertTrue(any(n.startswith(t.split(' ')[0]) for n in names), t)
        for a in labels:
            for b in labels:
                if a is b or abs(a[0] - b[0]) > 1:
                    continue
                apart = a[1] + len(a[2]) < b[1] or b[1] + len(b[2]) < a[1]
                self.assertTrue(apart, (a, b))                   # a column between, a row above or below

    def test_survey_channels_crowded_portrait(self):
        text, _ = self.run_app('wifi_survey', '140:key=2', crowd_capture(), frames=160)
        self.assertIn('2/5', text)
        self.assertIn('+- CHANNELS -', text)
        self.assertRegex(text, r'2\.4 GHz +\.ALL\. ch 1-13')
        self.assertIn('> TEST-Jasmine  ch 6  -', text)            # the AP picked on ACCESS POINTS
        labels = self.plot_labels(text)
        self.assert_labels_readable(labels, 3, 8)
        self.assertTrue(any(t.startswith('TEST-Jasm') for _, _, t in labels), labels)
        rows = text.splitlines()
        top = next(i for i, r in enumerate(rows) if '2.4 GHz' in r)
        axis = next(i for i in range(top, len(rows)) if re.search(r'-{20,}', rows[i]))
        self.assertGreaterEqual(axis - top - 2, 30)              # a tall plot: 30+ rows
        text, _ = self.run_app('wifi_survey', '140:key=2', crowd_capture(), frames=160, taps='22:66')
        self.assertRegex(text, r'\| +6 +6 +\d+ +-[34]\d')       # a page down: the per-channel table is still there

    def test_survey_channels_crowded_landscape(self):
        text, _ = self.run_app('wifi_survey', '140:key=2', crowd_capture(), frames=160, landscape=True)
        self.assertIn('+- CHANNELS -', text)
        labels = self.plot_labels(text)
        self.assert_labels_readable(labels, 5, 12)
        self.assertTrue(any(re.fullmatch(r'TEST-Jasmine -\d+', t) for _, _, t in labels), labels)

    def test_survey_channels_zoom_and_pan(self):
        script = '140:key=2;150:key=z'
        text, _ = self.run_app('wifi_survey', script, crowd_capture(), frames=160)
        self.assertRegex(text, r'\*ZOOM\* x2 ch 3-11')           # centred on the middle of 1-13
        self.assertRegex(text, r'\[=+\]')                       # where the view sits
        self.assert_labels_readable(self.plot_labels(text), 3, 8)
        # a drag right on the plot brings the lower channels in; its swipe is not a tab change
        text, _ = self.run_app('wifi_survey', script, crowd_capture(), frames=160, taps='S15:40>40:40')
        self.assertIn('2/5', text)
        self.assertRegex(text, r'x2 ch 1-[67] ')
        self.assertRegex(text, r'\n +\d+ \| +1 +2 +3 +4 +5 +6 ')     # the numbers follow
        # a drag left, the higher ones
        text, _ = self.run_app('wifi_survey', script, crowd_capture(), frames=160, taps='S40:40>10:40')
        self.assertIn('2/5', text)
        self.assertRegex(text, r'x2 ch 7-13 ')
        # a sideways swipe off the plot still changes tabs, and a vertical drag on it scrolls
        text, _ = self.run_app('wifi_survey', script, crowd_capture(), frames=160, taps='S40:61>10:61')
        self.assertIn('3/5', text)
        text, _ = self.run_app('wifi_survey', script, crowd_capture(), frames=160, taps='S25:50>25:30')
        self.assertIn('2/5', text)
        self.assertNotIn('*ZOOM* x2', text)                      # scrolled past the top of the tab
        # at ALL there is nothing to pan: the swipe changes tabs as anywhere else
        text, _ = self.run_app('wifi_survey', '140:key=2', crowd_capture(), frames=160, taps='S40:40>10:40')
        self.assertIn('3/5', text)

    def test_survey_channels_tap_lists_a_channel(self):
        text, _ = self.run_app('wifi_survey', '140:key=2', crowd_capture(), frames=160, taps='25:40')
        self.assertIn('CH 6  2437 MHz  6 APs on it, 5 over it', text)
        for name in ('TEST-Kiosk', 'TEST-Lantern', 'TEST-Maple', 'TEST-Nook', 'TEST-Oasis'):
            self.assertRegex(text, r'# ' + name + r' +-\d+ ')
        # zoomed and panned, a tap after the drag is a tap again
        text, _ = self.run_app('wifi_survey', '140:key=2;150:key=z', crowd_capture(), frames=160, taps='S40:40>10:40,=400,27:40')
        self.assertRegex(text, r'x2 ch 7-13 ')
        self.assertIn('CH 11  2462 MHz  5 APs on it', text)

    def test_survey_channels_desktop_zoom_keys(self):
        text, _ = self.run_app('wifi_survey', '140:key=2;150:key=z;152:key=z;154:key=.;156:key=.', crowd_capture(), frames=160, grid='160x45')
        self.assertRegex(text, r'\*ZOOM\* x3 ch \d+-\d+')
        self.assertRegex(text, r'\d 24\d\d ')                     # wide channels carry their MHz
        self.assert_labels_readable(self.plot_labels(text), 3, 16)


if __name__ == '__main__':
    unittest.main()
