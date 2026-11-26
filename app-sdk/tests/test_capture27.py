"""API 2.7 raw capture and the TAP analyser over headless lssim (build lssim first).
The simulator replays bench/fixtures/capture_sample.pcapng, a synthetic file."""
import os
import re
import shutil
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
SDK=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(SDK))
sys.path.insert(0,str(Path(__file__).resolve().parent))
import lsdk
from pcapng_gen import Pcapng,beacon,qos_data,deauth,BCAST,mixed
BUILD=Path(os.environ.get('LS_BENCH_BUILD') or SDK.parent/'bench/build')
SAMPLE=SDK.parent/'bench/fixtures/capture_sample.pcapng'
NAMES=['lap_wifi_capture_start','lap_ble_capture_start','lap_capture_read','lap_capture_status','lap_capture_stop']
def blocks(data):
    at=0
    while at<len(data):
        kind,total=struct.unpack_from('<II',data,at)
        assert total>=12 and total%4==0 and at+total<=len(data),(at,total)
        assert struct.unpack_from('<I',data,at+total-4)[0]==total,at
        yield kind,data[at+8:at+total-4]
        at+=total
def tshark_command():
    """tshark from PATH or the Wireshark Flatpak; None when there is neither."""
    if shutil.which('tshark'):return ['tshark']
    if shutil.which('flatpak'):
        try:
            if subprocess.run(['flatpak','info','org.wireshark.Wireshark'],capture_output=True,timeout=30).returncode==0:
                return ['flatpak','run','--filesystem=%s'%BUILD,'--command=tshark','org.wireshark.Wireshark']
        except (OSError,subprocess.SubprocessError):pass
    return None
TSHARK=tshark_command()
MENU=re.compile(r'\+-+ ?(OPTIONS|PACKET|FILE|CAPTURE|VIEW|ANALYZE|STATISTICS|EDIT AND GO|OPEN CAPTURE|RECENT FILTERS)\b')
def frames_of(data):
    """(interface, time in microseconds, captured, original, packet bytes) of every enhanced packet block."""
    out=[]
    for kind,body in blocks(data):
        if kind==6:
            ifc,hi,lo,cap,orig=struct.unpack_from('<IIIII',body)
            out.append((ifc,hi<<32|lo,cap,orig,body[20:20+cap]))
    return out
class Capture27(unittest.TestCase):
    def tshark(self,data,*args):
        """Rows of tab-separated fields from tshark over a capture."""
        with tempfile.TemporaryDirectory(dir=BUILD) as d:
            path=Path(d)/'t.pcapng';path.write_bytes(data)
            run=subprocess.run(TSHARK+['-r',str(path)]+list(args),capture_output=True,text=True,errors='replace',timeout=120)
            self.assertEqual(run.returncode,0,run.stderr)
            return [line.split('\t') for line in run.stdout.splitlines()]
    def run_tap(self,script,capture=SAMPLE,landscape=True,args=(),frames=80,card=None,radio=False,**more):
        with tempfile.TemporaryDirectory(dir=BUILD) as d:
            env=dict(lsdk.host_env(),LAPSIM='tap',LAPSIM_SCRIPT=script,LAPSIM_CONSENT='grant',LAPSIM_RADIO='none',**more)
            for k in ('LAPSIM_DLL','LSSIM_FRAMES','LAPSIM_NFC','LAPSIM_HW','LAPSIM_CAPTURE'):env.pop(k,None)
            if radio:env.pop('LAPSIM_RADIO')        # the simulator's LoRa radio, fed by packethex=
            if capture:env['LAPSIM_CAPTURE']=str(capture)
            for name,data in (card or {}).items():  # captures already on the card
                (Path(d)/'sdcard/captures').mkdir(parents=True,exist_ok=True)
                (Path(d)/'sdcard/captures'/name).write_bytes(data)
            exe=BUILD/('lssim.exe' if os.name=='nt' else 'lssim')
            run=subprocess.run([str(exe),'lap','-A','40','-f',str(frames)]+(['-l'] if landscape else [])+['-d']+list(args),cwd=d,env=env,capture_output=True,text=True,errors='replace',timeout=60,creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)
            text=run.stdout+run.stderr
            self.assertEqual(run.returncode,0,text)
            self.saved={p.name:p.read_bytes() for p in sorted(Path(d).glob('sdcard/captures/*.pcapng')) if p.name not in (card or {})}
            return text,list(self.saved.values())
    def test_api_is_exported_at_2_7(self):
        exports=(SDK/'exports.txt').read_text().split()
        table=(SDK.parent/'components/apps/tui/ls_lap_exports.inc').read_text()
        header=(SDK/'include/lakeshark_app.h').read_text(encoding='utf-8')
        for name in NAMES:
            self.assertIn(name,exports)
            self.assertIn('EXPORT("%s", %s)'%(name,name),table)
        self.assertIn('#define LAP_API_MINOR 7',header)
        self.assertIn('#define LAP_API_27 1',header)
        minor=int(re.search(r'#define LS_LAP_API2_MINOR (\d+)u',(SDK.parent/'components/apps/tui/ls_lap_manifest.h').read_text()).group(1))
        self.assertGreaterEqual(minor,7)
    def test_replay_dissects_and_saves_pcapng(self):
        text,files=self.run_tap('60:key=w')
        self.assertIn('WIFI 30',text)
        self.assertIn('BLE 5',text)
        self.assertIn('Beacon, SSID=TEST-LakeNet, WPA2',text)
        self.assertEqual(len(files),1,text)
        links=[];packets={}
        for kind,body in blocks(files[0]):
            if kind==1:links.append(struct.unpack_from('<H',body)[0])
            if kind==6:
                ifc,_,_,cap,orig=struct.unpack_from('<IIIII',body)
                self.assertLessEqual(cap,orig)
                packets[links[ifc]]=packets.get(links[ifc],0)+1
        self.assertEqual(links,[127,256,270])
        self.assertEqual(packets,{127:30,256:5})
    def test_saved_file_replays_the_same(self):
        _,files=self.run_tap('60:key=w')
        with tempfile.TemporaryDirectory(dir=BUILD) as d:
            again=Path(d)/'again.pcapng'
            again.write_bytes(files[0])
            text,_=self.run_tap('70:key=s',capture=again)
        self.assertIn('35 packets, 2197 bytes',text)
    def test_no_file_is_unavailable_never_invented(self):
        text,_=self.run_tap('79:key=a',capture=None)
        self.assertIn('WIFI n/a 0',text)
        self.assertIn('BLE n/a 0',text)
        self.assertIn(' 0 of 0',text)
    def test_hopping_is_refused_while_joined(self):
        text,_=self.run_tap('50:key=o;52:key=@enter;54:key=@down;55:key=@down;56:key=@enter',LAPSIM_CAPTURE_JOINED='1')
        self.assertIn('WI-FI         ON (unavailable)',text)
        self.assertIn('hopping would drop it',text)
    def test_display_filter(self):
        keys=';'.join('%d:key=%s'%(50+i,c) for i,c in enumerate('/eapol||deauth'))
        text,_=self.run_tap(keys+';70:key=@enter')
        self.assertIn('5 of 35',text)
        self.assertIn('Key (message 4 of 4)',text)
    # ---- the handheld: T-Display portrait, one pane at a time -----------
    def test_handheld_list_and_soft_keys(self):
        text,_=self.run_tap('',landscape=False)
        self.assertIn('TAP  35 pkts',text)
        self.assertIn('WIFI 30',text)
        self.assertIn('BLE 5',text)
        self.assertIn('SOURCES',text)                    # the source panel's title on its rule
        # the board's finger-sized keys: one row of four, labels in their middle
        self.assertRegex(text,r'\| PAUSE \|.*\|FILTER \|.*\| STATS \|.*\| MENU')
    def test_handheld_taps_open_detail_and_step(self):
        # tap packet 11 twice for its tree, then NEXT on the key bar
        text,_=self.run_tap('',landscape=False,frames=120,args=('-x','20:25,20:25,30:66'))
        self.assertIn('Packet 12: EAPOL',text)
        self.assertRegex(text,r'\| BACK .\| +\| PREV .\| +\| NEXT .\|')
        self.assertIn('Key information: 0x010a',text)
    def test_handheld_menu_soft_key(self):
        text,_=self.run_tap('',landscape=False,frames=120,args=('-x','40:66'))
        self.assertIn('SOURCES AND DROPS',text)
    def test_handheld_filter_through_the_keyboard(self):
        text,_=self.run_tap('',landscape=False,frames=120,args=('-x','10:13,~3,+eapol_or_deauth,@enter,~3'))
        self.assertIn('Filter: eapol or deauth',text)
        self.assertIn('5 of 35',text)
    def test_handheld_statistics_fit(self):
        text,_=self.run_tap('60:key=s;61:key=@right',landscape=False)
        self.assertRegex(text,r' ENDPOINTS .*\n.* 2/7 ')   # the strip's middle key: name over n/7
        self.assertIn('02:aa:00:00:00:01 WIFI',text)
        self.assertIn('TEST-LakeNet',text)
    def test_handheld_without_transport_says_how(self):
        text,_=self.run_tap('',landscape=False,LAPSIM_CAPTURE_NO_TRANSPORT='1')
        self.assertIn('WIFI n/a 0',text)
        self.assertIn('C6 monitor firmware',text)
        self.assertIn('SYSTEM > UPDATE > C6 RADIO FW',text)
        text,_=self.run_tap('60:key=i',landscape=False,LAPSIM_CAPTURE_NO_TRANSPORT='1')
        self.assertIn('WI-FI 802.11   NOT AVAILABLE',text)
        self.assertIn('BLUETOOTH LE   RUNNING',text)
        self.assertIn('advertising channels 37, 38 and 39',text)
        self.assertIn('STORE (PSRAM)',text)
    def test_handheld_link_in_use_says_how(self):
        text,_=self.run_tap('50:key=o;52:key=@enter;54:key=@down;55:key=@down;56:key=@enter',landscape=False,LAPSIM_CAPTURE_JOINED='1')
        self.assertIn('WIFI n/a 30',text)
        self.assertIn('802.11: the board is on a Wi-Fi network; hopping',text)
        self.assertIn('Allow that under CAPTURE > ALLOW',text)
    def test_heavy_traffic_is_bounded_and_counted(self):
        """Ten thousand frames a second for three seconds: the store keeps its
        4096, ages the rest out, and the radio's drops show on the source row."""
        p=Pcapng()
        for i in range(30000):
            ap=bytes([2,0xaa,0,0,(i//256)%64,i%256])
            f=beacon(ap,'TEST-Flood%d'%(i%300),6,seq=i) if i%3==0 else deauth(ap,BCAST,ap,seq=i) if i%3==1 else qos_data(bytes([2,0xbb,0,0,0,i%50]),ap,ap,n=300,seq=i)
            p.wifi(1_000_000+i*100,f,ch=6,rssi=-40-i%50)
        with tempfile.TemporaryDirectory(dir=BUILD) as d:
            path=Path(d)/'heavy.pcapng';p.save(path)
            text,_=self.run_tap('',capture=path,landscape=False,frames=200,LAPSIM_CAPTURE_DROP='1')
            self.assertIn('TAP  4096 pkts',text)
            self.assertRegex(text,r'4096 pkts +\d+ aged out')
            self.assertRegex(text,r'WIFI \d+k -\d+k')
            text,_=self.run_tap('190:key=i',capture=path,landscape=False,frames=200,LAPSIM_CAPTURE_DROP='1')
            self.assertRegex(text,r'\d+ frames heard, \d+ dropped by the radio')
            self.assertRegex(text,r'4096 of 4096 packets')
            # without drops the file waits: every frame arrives, the store stays at 4096
            text,_=self.run_tap('',capture=path,landscape=False,frames=400,LAPSIM_CAPTURE_PACE='0')
            self.assertIn('WIFI 30k',text)
            self.assertIn('25904 aged out',text)
    # ---- TAP 1.2: files Wireshark opens, long captures, saved files, filters ----
    def shown(self,text):
        m=re.search(r'(\d+) of (\d+)',text)
        self.assertIsNotNone(m,text)
        return int(m.group(1)),int(m.group(2))
    @unittest.skipUnless(TSHARK,'tshark is not installed (PATH or the org.wireshark.Wireshark Flatpak)')
    def test_tshark_dissects_a_live_save_cleanly(self):
        """Wi-Fi, Bluetooth and LoRa heard in one session, saved, and read by the
        real Wireshark: every interface, no malformed packet, no error or warning."""
        lora='ffffffff78563412efcdab8963080000deadbeef0102030405060708'
        text,files=self.run_tap('20:packethex=%s;70:key=w'%lora,radio=True,frames=100)
        self.assertEqual(len(files),1,text)
        rows=self.tshark(files[0],'-T','fields','-e','frame.interface_name','-e','frame.len','-e','frame.cap_len','-e','frame.protocols','-e','_ws.malformed','-e','_ws.expert.severity','-e','btle.crc.incorrect')
        mine=frames_of(files[0])
        self.assertEqual(len(rows),len(mine))
        self.assertEqual({r[0] for r in rows},{'wifi0','ble0','lora0'})
        for r,(ifc,_,cap,orig,_) in zip(rows,mine):
            self.assertEqual((int(r[1]),int(r[2])),(orig,cap),r)
            self.assertEqual(r[4],'',r)                                    # [Malformed Packet]
            self.assertEqual(r[6],'',r)
            self.assertFalse([v for v in r[5].split(',') if v and int(v)>=0x600000],r)     # warnings and errors
        self.assertEqual(sum(r[0]=='wifi0' for r in rows),30)
        self.assertEqual(sum(r[0]=='ble0' for r in rows),5)
        self.assertTrue(all('radiotap:wlan_radio:wlan' in r[3] for r in rows if r[0]=='wifi0'),rows)
        self.assertTrue(all(r[3].startswith('bluetooth:btle_rf:btle') for r in rows if r[0]=='ble0'),rows)
        got=self.tshark(files[0],'-Y','loratap.payload contains de:ad:be:ef','-T','fields','-e','loratap.channel.frequency','-e','loratap.channel.sf','-e','loratap.syncword')
        self.assertEqual(got,[['910525000','7','0x12']])
        # the wall clock the board had (the simulator's: 2026-10-23 12:00:00 UTC), one second of capture
        times=[float(r[0]) for r in self.tshark(files[0],'-T','fields','-e','frame.time_epoch')]
        self.assertTrue(all(1792756800<=t<1792756805 for t in times),times)
    @unittest.skipUnless(TSHARK,'tshark is not installed (PATH or the org.wireshark.Wireshark Flatpak)')
    def test_tshark_reads_what_a_reopened_file_saves(self):
        """A capture opened from the card and saved again is the same capture to
        Wireshark: every frame, its time to the microsecond, its lengths, the
        radio's figures."""
        src=mixed().bytes()
        text,files=self.run_tap('',capture=None,frames=120,card={'tap-mixed.pcapng':src},args=('-x','+l,~4,@enter,~30,+w,~30'))
        self.assertEqual(len(files),1,text)
        want=frames_of(src);got=frames_of(files[0])
        self.assertEqual(len(got),len(want))
        for (wi,wt,wc,wo,wd),(gi,gt,gc,go,gd) in zip(want,got):
            self.assertEqual((gi,gt,gc,go),(wi,wt,wc,wo))
            if gi!=1:self.assertEqual(gd,wd)             # Bluetooth: the CRC is worked out; the fixture's was zero
        rows=self.tshark(files[0],'-T','fields','-e','frame.number','-e','frame.time_epoch','-e','_ws.malformed','-e','_ws.expert.severity')
        self.assertEqual(len(rows),len(want))
        for r,(_,t,_,_,_) in zip(rows,want):
            self.assertEqual(round(float(r[1])*1e6),t,r)
            self.assertEqual(r[2],'',r)
            self.assertFalse([v for v in r[3].split(',') if v and int(v)>=0x600000],r)
        got=self.tshark(files[0],'-Y','loratap.syncword == 0x2b','-T','fields','-e','loratap.channel.frequency','-e','loratap.channel.sf','-e','loratap.channel.bandwidth','-e','loratap.rssi.packet','-e','loratap.rssi.snr')
        self.assertEqual(len(got),10)
        self.assertEqual(got[0][:3],['906875000','11','2'])
        self.assertEqual((int(got[0][3])-139,(int(got[0][4])-256)/4),(-101,-4.5))    # as LoRaTap packs them: dBm + 139, quarter dB
        radio=self.tshark(files[0],'-Y','wlan.ssid == "TEST-OpenCafe"','-T','fields','-e','wlan_radio.channel','-e','radiotap.dbm_antsignal','-e','radiotap.channel.freq')
        self.assertEqual(len(radio),40)
        self.assertEqual(radio[0],['11','-70','2462'])
    @unittest.skipUnless(TSHARK,'tshark is not installed (PATH or the org.wireshark.Wireshark Flatpak)')
    def test_tshark_accepts_every_advertising_pdu_tap_writes(self):
        """The awkward ones: a directed advertisement as the controller reports it
        (no target), one with its target, an empty ADV_IND, a full 31 bytes."""
        from pcapng_gen import mac
        p=Pcapng(ble=True);a=mac('c6:05:04:03:02:01')[::-1]
        p.ble_adv(1_100_000,a,b'',pdu=1)
        p.ble_adv(1_200_000,a,mac('02:11:22:33:44:55')[::-1],pdu=1)
        p.ble_adv(1_300_000,a,b'',pdu=0)
        p.ble_adv(1_400_000,a,bytes([30,9])+b'A'*29,pdu=6)
        p.ble_adv(1_500_000,a,bytes([2,1,6]),pdu=2)
        with tempfile.TemporaryDirectory(dir=BUILD) as d:
            path=Path(d)/'ble.pcapng';p.save(path)
            text,files=self.run_tap('40:key=w',capture=path,frames=60,LAPSIM_CAPTURE_PACE='0')
        self.assertIn('ADV_DIRECT_IND to a target',text)
        rows=self.tshark(files[0],'-T','fields','-e','_ws.col.Info','-e','_ws.malformed','-e','_ws.expert.severity','-e','btle.crc.incorrect')
        self.assertEqual([r[0] for r in rows],['ADV_DIRECT_IND','ADV_DIRECT_IND','ADV_IND','ADV_SCAN_IND','ADV_NONCONN_IND'])
        for r in rows:
            self.assertEqual(r[1:]+['']*(4-len(r)),['','',''],r)
    FILTERS=['wlan','btle','eapol','wlan.fc.type == 0','wlan.fc.type == 2','wlan.fc.type_subtype == 0x08','wlan.fc.type_subtype == 8',
        'wlan.fc.type_subtype == 12','wlan.fc.type_subtype in {10, 12}','wlan.ssid == "TEST-LakeNet"','wlan.ssid contains "Lake"',
        'wlan.ssid matches "^test-.*net$"','wlan.addr == 02:bb:00:00:00:10','wlan.sa == 02:bb:00:00:00:10','wlan.da == ff:ff:ff:ff:ff:ff',
        'wlan.ta == 02:aa:00:00:00:01','wlan.ra == 02:aa:00:00:00:01','wlan.bssid == 02:aa:00:00:00:02','wlan.bssid != 02:aa:00:00:00:02',
        'wlan_radio.channel == 11','radiotap.dbm_antsignal > -50','radiotap.dbm_antsignal <= -70','frame.number >= 100 && frame.number < 120',
        'frame.len > 120','frame.time_relative > 20.5','wlan.fc.protected == 1','wlan.fc.retry == 1','wlan.fc.protected',
        'btle.advertising_address == c6:05:04:03:02:01','btcommon.eir_ad.entry.device_name == "TEST-Tag-01"',
        'btcommon.eir_ad.entry.device_name contains "Tag"','btcommon.eir_ad.entry.company_id == 0x004c','wlan.fc.type_subtype == 0x0c || eapol',
        '!wlan && frame.len < 60','not (wlan.fc.type == 0) and wlan','frame contains "LakeNet"','wlan.fixed.reason_code == 7',
        'btle.advertising_header.pdu_type == 0','wlan.ssid','!(wlan.addr == 02:aa:00:00:00:01) && wlan','frame.len <= 40 or frame.number == 1']
    @unittest.skipUnless(TSHARK,'tshark is not installed (PATH or the org.wireshark.Wireshark Flatpak)')
    def test_display_filters_agree_with_wireshark(self):
        """The same filter text shows the same number of packets in TAP and in
        tshark, on the file TAP itself saved from that session."""
        with tempfile.TemporaryDirectory(dir=BUILD) as d:
            path=Path(d)/'mixed.pcapng';mixed().save(path)
            _,files=self.run_tap('60:key=w',capture=path,landscape=False,frames=100,LAPSIM_CAPTURE_PACE='0')
            total=len(frames_of(files[0]))
            self.assertEqual(total,sum(f[0]<2 for f in frames_of(mixed().bytes())))      # the simulator replays Wi-Fi and Bluetooth, not LoRa
            for f in self.FILTERS:
                text,_=self.run_tap('60:key=/;62:dialogtext=%s'%f,capture=path,landscape=False,frames=80,LAPSIM_CAPTURE_PACE='0')
                self.assertNotIn('filter: ',text,f)     # refused
                self.assertEqual(self.shown(text),(len(self.tshark(files[0],'-Y',f,'-T','fields','-e','frame.number')),total),f)
    def test_wireshark_filter_names_and_short_forms(self):
        for f,n in (('wlan.fc.type_subtype == 0x08',14),('wlan.fc.type_subtype == 8',14),('beacon',14),('eapol || wlan.fc.type_subtype == 12',5),
                    ('wlan.ssid contains "Open"',7),('ssid == test-opencafe',7),('btle',5),('btcommon.eir_ad.entry.company_id == 0x004c',2),
                    ('frame.number <= 10 and not btle',7),('wlan.fc.type_subtype in {0x08, 0x05}',15),('wlan.addr == 02:bb:00:00:00:10 && !eapol',10)):
            text,_=self.run_tap('60:key=/;62:dialogtext=%s'%f,landscape=False)
            self.assertEqual(self.shown(text),(n,35),f)
    def test_a_wrong_filter_turns_red_and_says_why(self):
        text,_=self.run_tap('40:key=/;42:dialogtext=eapol;50:key=/;52:dialogtext=wlan.fc.typ == 0',landscape=False)
        self.assertIn('filter: unknown field wlan.fc.typ',text)
        self.assertRegex(text,r'Filter: wlan\.fc\.typ == 0|! unknown field wlan\.fc\.typ')
        self.assertEqual(self.shown(text),(4,35))       # the list keeps the last good filter
        for bad,why in (('wlan.fc.type == abc','is not a number: abc'),('(eapol','missing )'),('wlan.addr contains 02','is an address'),
                        ('eapol deauth','expected and/or before deauth'),('wlan.ssid matches "a(b"','matches: ( ) | { } are not supported')):
            text,_=self.run_tap('40:key=/;42:dialogtext=%s'%bad,landscape=False)
            self.assertIn(why,text,bad)
    def test_recent_filters_are_kept(self):
        text,_=self.run_tap('40:key=/;42:dialogtext=eapol;46:key=/;48:dialogtext=wlan.fc.retry == 1;52:key=x;60:key=r',landscape=False)
        self.assertIn('RECENT FILTERS',text)
        self.assertRegex(text,r'wlan\.fc\.retry == 1[^\n]*\n(.*\n){0,8}.* eapol ')
    def test_capture_to_card_streams_rotates_and_keeps_the_newest(self):
        """A long capture: every kept frame reaches the card in files of the size
        chosen, though the store holds only the last 4096; the ring removes the
        oldest file."""
        p=Pcapng()
        for i in range(30000):
            ap=bytes([2,0xaa,0,0,(i//256)%64,i%256])
            p.wifi(1_000_000+i*100,beacon(ap,'TEST-Long%d'%(i%300),6,seq=i) if i%2 else qos_data(bytes([2,0xbb,0,0,0,i%50]),ap,ap,n=300,seq=i),ch=6,rssi=-40-i%50)
        keys=['o','@enter']+['@down']*13+['@left','@left','@down']            # CAPTURE > CARD FILE SIZE: 256 KB; then FILES KEPT
        end=['@down','@enter','d']                                            # SOURCES AND DROPS, then TO CARD on
        with tempfile.TemporaryDirectory(dir=BUILD) as d:
            path=Path(d)/'long.pcapng';p.save(path)
            for keep,want in ((['@right','@right'],None),([],8)):             # keep ALL, then the default 8
                script=';'.join('%d:key=%s'%(3+i,k) for i,k in enumerate(keys+keep+end))
                text,_=self.run_tap(script,capture=path,landscape=False,frames=520,LAPSIM_CAPTURE_PACE='0')
                self.assertIn('TAP  SOURCES AND DROPS',text)
                self.assertRegex(text,r'CAPTURE TO CARD +ON')
                self.assertIn('0 dropped: the card was slower than the air',text)
                self.assertRegex(text,r'4096 of 4096 packets')
                names=sorted(self.saved)
                numbers=[int(n[-12:-7]) for n in names]
                self.assertEqual(numbers,list(range(numbers[0],numbers[0]+len(numbers))),names)
                seqs=[]
                for n in names:
                    links=[struct.unpack_from('<H',b)[0] for k,b in blocks(self.saved[n]) if k==1]
                    self.assertEqual(links,[127,256,270],n)                   # every file stands alone
                    if n!=names[-1]:self.assertTrue(256*1024<=len(self.saved[n])<256*1024+2600,(n,len(self.saved[n])))
                    seqs+=[struct.unpack_from('<H',f[4][15+22:])[0]>>4 for f in frames_of(self.saved[n])]
                written=int(re.search(r'(\d+) packets, \d+ KB written in (\d+) files',text).group(1))
                made=int(re.search(r'(\d+) packets, \d+ KB written in (\d+) files',text).group(2))
                self.assertEqual(numbers[-1],made)
                self.assertGreater(written,20000)                             # far more than the store's 4096
                self.assertEqual(seqs[-1],29999&0xFFF)                        # up to the last frame heard
                self.assertTrue(all((b-a)&0xFFF==1 for a,b in zip(seqs,seqs[1:])),'a frame is missing between files')
                if want is None:
                    self.assertEqual(numbers[0],1)
                    self.assertEqual(len(seqs),written)                       # the count on screen is what is on the card
                    self.assertIn('all are kept',text)
                else:
                    self.assertEqual(len(names),want)
                    self.assertRegex(text,r'the newest 8\s+(\d+ +)?are kept, %d removed so far'%(made-8))
    def test_open_a_saved_capture_and_browse_it_offline(self):
        src=mixed(short=True).bytes()
        card={'tap-mixed.pcapng':src,'notes.txt':b'not a capture'}
        text,_=self.run_tap('',capture=None,landscape=False,frames=60,card=card,args=('-x','+l,~4'))
        self.assertIn('OPEN CAPTURE',text)
        self.assertIn('tap-mixed.pcapng',text)
        self.assertNotIn('notes.txt',text)
        text,_=self.run_tap('',capture=None,landscape=False,frames=100,card=card,args=('-x','+l,~4,@enter,~30'))
        n=[sum(f[0]==i for f in frames_of(src)) for i in range(3)]
        self.assertIn('FILE tap-mixed  %d pkts'%sum(n),text)                       # the title says what it is
        self.assertIn('FILE  NOT LIVE',text)
        self.assertIn('WIFI %d  BLE %d  LORA %d'%tuple(n),text)                      # all three link types read back
        self.assertRegex(text,r'\| LIVE ')                                   # the key that goes back to the radios
        self.assertNotIn('PAUSE',text)
        self.assertIn('Beacon, SSID=TEST-La',text)
        # the same filters, tree and statistics over the file
        text,_=self.run_tap('70:key=/;72:dialogtext=meshtastic || btcommon.eir_ad.entry.company_id == 0x004c',capture=None,landscape=False,frames=100,card=card,args=('-x','+l,~4,@enter,~30'))
        self.assertEqual(self.shown(text),(10+7,sum(n)))
        text,_=self.run_tap('',capture=None,landscape=False,frames=110,card=card,args=('-x','+l,~4,@enter,~30,+s,~2,+e,~2'))
        self.assertIn('TAP  STATISTICS  (FILE)',text)
        self.assertIn('Malformed or short frame',text)
        self.assertIn('Deauth/disassoc flood',text)
        self.assertIn('EAPOL 4-way handshake complete',text)
        self.assertIn('Same SSID, different security',text)
        self.assertIn('Beacon: BSSID changed its SSID',text)
        self.assertIn('Retransmission (retry bit)',text)
        # LIVE closes the file and the radios listen again
        text,_=self.run_tap('',landscape=False,frames=140,card=card,args=('-x','+l,~4,@enter,~30,+l,~40'))
        self.assertIn('TAP  35 pkts',text)
        self.assertRegex(text,r'o WIFI \d+')
        self.assertIn('LIVE',text)
        self.assertNotIn('NOT LIVE',text)
        self.assertRegex(text,r'\| PAUSE \|')
    def test_a_file_larger_than_the_store_is_browsed_in_parts(self):
        p=Pcapng()
        for i in range(6000):p.wifi(1_000_000+i*1000,beacon(bytes([2,0xaa,0,0,i//256,i%256]),'TEST-Part%d'%i,6,seq=i),ch=6)
        card={'big.pcap':b'\xd4\xc3\xb2\xa1\x02\x00\x04\x00'+bytes(8)+struct.pack('<II',65535,127),'tap-big.pcapng':p.bytes()}
        text,_=self.run_tap('',capture=None,frames=120,card=card,args=('-x','+l,~4,@enter,~60'))
        self.assertIn('part 1-4096 of 6000',text)
        self.assertIn('SSID=TEST-Part0,',text)
        text,_=self.run_tap('',capture=None,frames=220,card=card,args=('-x','+l,~4,@enter,~60,+g,~3,+5000,@enter,~60'))
        self.assertIn('part 4800-6000 of 6000',text)
        self.assertIn('SSID=TEST-Part4999,',text)
        text,_=self.run_tap('',capture=None,landscape=False,frames=60,card=card,args=('-x','+l,~4,@enter,~60,+s,~3'))
        self.assertIn('6000 packets, ',text)                                  # statistics cover the whole file
        self.assertIn('in the whole file',text)
        # a classic pcap with no packets opens and says so
        text,_=self.run_tap('',capture=None,landscape=False,frames=100,card=card,args=('-x','+l,~4,@down,@enter,~30'))
        self.assertIn('FILE big  0 pkts',text)
        self.assertIn('0 frames, 24 B',text)
        self.assertIn('no frames TAP reads in this file',text)
    def test_mark_go_to_find_and_save_marked_or_displayed(self):
        # mark packet 35 (selected), go to 12 and mark it, find a deauth, save the marked
        steps='+k,+g,~3,+12,@enter,~3,+k,+e,+deauth,@enter,~3,+o,@down,@down,@down,@down,@enter,@down,@down,@down,@enter,~30'
        text,files=self.run_tap('',frames=200,args=('-x',steps))
        self.assertIn('Packet 23: 802.11',text)                               # find went to the deauth
        self.assertEqual(list(self.saved),['tap-20261023-120000-marked.pcapng'])
        got=frames_of(files[0])
        self.assertEqual(len(got),2)
        self.assertIn(bytes.fromhex('888e'),got[0][4])                         # packet 12: EAPOL message 2
        text,_=self.run_tap('',frames=120,args=('-x','+k,+g,~3,+12,@enter,~3,+k'))
        self.assertRegex(text,r'\* +12 ')                                     # the mark shows in the list
        self.assertIn('marked',text)
        text,_=self.run_tap('',frames=120,args=('-x','+k,+g,~3,+12,@enter,~3,+k,+/,+marked,@enter,~3'))     # -x runs after the frames
        self.assertEqual(self.shown(text),(2,35))
        # save only what the filter displays
        text,files=self.run_tap('40:key=/;42:dialogtext=eapol',landscape=False,frames=140,args=('-x','+o,@down,@down,@down,@down,@enter,@down,@down,@enter,~30'))
        self.assertEqual(list(self.saved),['tap-20261023-120000-shown.pcapng'])
        self.assertEqual(len(frames_of(files[0])),4)
        # find by text, backwards, and a packet number that is not there
        text,_=self.run_tap('',frames=120,args=('-x','+e,+"opencafe",@enter,~3,+b,~3'))
        self.assertIn('Packet 34: 802.11',text)                               # forward wrapped to 3, back wrapped to 34
        text,_=self.run_tap('',frames=120,args=('-x','+g,~3,+500,@enter,~3'))
        self.assertIn('Packet 35: 802.11',text)                               # no such packet: the selection stays
    def test_time_display_and_colouring_rule(self):
        text,_=self.run_tap('60:key=t;61:key=t')
        self.assertIn('Time of day',text)
        self.assertRegex(text,r' 12:00:0\d\.\d\d\d ')
        text,_=self.run_tap('60:key=t')
        self.assertIn('Delta',text)
        text,_=self.run_tap('',landscape=False,frames=120,args=('-x','20:25,20:25'))
        self.assertIn('Colouring rule: EAPOL (eapol)',text)
        self.assertIn('Arrival: 2026-10-23 12:00:0',text)
        self.assertIn('frame.len 146',text)
        # the handheld's third column: a tap on the heading
        text,_=self.run_tap('',landscape=False,frames=120,args=('-x','20:14,~3'))
        self.assertRegex(text,r'No\. Proto +Time +dBm Info')
    def test_statistics_like_wireshark(self):
        with tempfile.TemporaryDirectory(dir=BUILD) as d:
            path=Path(d)/'mixed.pcapng';mixed().save(path)
            more=dict(capture=path,landscape=False,frames=100,LAPSIM_CAPTURE_PACE='0')
            text,_=self.run_tap('80:key=s;81:key=@right;82:key=@right;83:key=@right',**more)
            self.assertRegex(text,r' I/O GRAPH .*\n.* 4/7 ')
            self.assertIn('Packets a second, one dot 1 s',text)
            self.assertRegex(text,r'WIFI +262 packets +peak +68/s')
            self.assertRegex(text,r'BLE +47 packets')
            self.assertIn('0.0 s',text)
            text,_=self.run_tap('80:key=s;81:key=@right;82:key=@right',**more)
            self.assertRegex(text,r'02:bb:00:00:00:10 <> 02:aa:00:00:00:01')
            self.assertRegex(text,r'A>B \d+ +B>A \d+ +-?\d+ dBm +\d+\.\d-\d+\.\d s')
            self.assertRegex(text,r'SORT *\.*\|')
            text,_=self.run_tap('80:key=s;81:key=@right;82:key=z;83:key=z',**more)   # ENDPOINTS by signal
            self.assertRegex(text,r'SIGNAL')
            first=re.search(r'Address +Via +Name *\n *\d+ (\S+)',text).group(1)
            self.assertEqual(first,'02:ee:00:00:00:66')                       # the strongest: the twin at -30 dBm
            # an expert row filters the list to what it counted
            text,_=self.run_tap('80:key=s;81:key=e',args=('-x','20:22,~3'),**dict(more,frames=120))
            self.assertIn('Filter: deauth',text)
            self.assertEqual(self.shown(text),(61,309))
    def test_a_tap_on_empty_ground_opens_nothing(self):
        """A tap TAP does not use must not fall through to the board, which would
        turn it into UP, ENTER or DOWN: no menu, no list, no detail may open."""
        for landscape in (False,True):
            spot='20:56' if not landscape else '60:22'
            for name,before,stay in (('list','','Filter: (none)'),('detail','20:25,20:25,~3' if not landscape else '+m,@enter,~3','Packet '),
                                     ('statistics','+s,~3','Protocol'),('expert','+s,~3,+e,~3','Errors 0'),('sources','+i,~3','frames heard'),('help','+h,~3','TAP listens')):
                if landscape and name=='detail':continue
                steps=','.join(x for x in ('~30',before,spot,'~2',spot,'~2','3:40' if not landscape else '100:20','~3') if x)
                text,_=self.run_tap('',landscape=landscape,frames=120,args=('-x',steps))
                self.assertIsNone(MENU.search(text),(name,landscape,text))
                self.assertIn(stay,text,(name,landscape))
                if name=='list':self.assertNotIn('Packet 35:' if not landscape else '+- OPTIONS',text)
        # The rule over the tree is nobody's: a tap there once fell through and the
        # board made it UP, which moved the tree's pick (seen here as the long
        # line above the picked one unfolding onto a second row).
        text,_=self.run_tap('',landscape=False,frames=100,args=('-x','20:25,20:25,~3,20:25,~3,20:14,~3'))
        self.assertIn('source 02:aa:00:>',text)
        self.assertIn('Packet 11: EAPOL',text)
        # an empty list too: nothing heard, nothing to open
        text,_=self.run_tap('',capture=None,landscape=False,frames=100,args=('-x','~30,20:40,~2,20:56,~3'))
        self.assertIsNone(MENU.search(text),text)
        self.assertIn(' 0 of 0',text)
if __name__=='__main__':unittest.main()
