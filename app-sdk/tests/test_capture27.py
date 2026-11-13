"""API 2.7 raw capture and the TAP analyser over headless lssim (build lssim first).
The simulator replays bench/fixtures/capture_sample.pcapng, a synthetic file."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
SDK=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(SDK))
import lsdk
BUILD=SDK.parent/'bench/build'
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
class Capture27(unittest.TestCase):
    def run_tap(self,script,capture=SAMPLE,**more):
        with tempfile.TemporaryDirectory(dir=BUILD) as d:
            env=dict(lsdk.host_env(),LAPSIM='tap',LAPSIM_SCRIPT=script,LAPSIM_CONSENT='grant',LAPSIM_RADIO='none',**more)
            for k in ('LAPSIM_DLL','LSSIM_FRAMES','LAPSIM_NFC','LAPSIM_HW','LAPSIM_CAPTURE'):env.pop(k,None)
            if capture:env['LAPSIM_CAPTURE']=str(capture)
            exe=BUILD/('lssim.exe' if os.name=='nt' else 'lssim')
            run=subprocess.run([str(exe),'lap','-A','40','-f','80','-l','-d'],cwd=d,env=env,capture_output=True,text=True,errors='replace',timeout=60,creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)
            text=run.stdout+run.stderr
            self.assertEqual(run.returncode,0,text)
            return text,[p.read_bytes() for p in Path(d).glob('sdcard/captures/*.pcapng')]
    def test_api_is_exported_at_2_7(self):
        exports=(SDK/'exports.txt').read_text().split()
        table=(SDK.parent/'components/apps/tui/ls_lap_exports.inc').read_text()
        header=(SDK/'include/lakeshark_app.h').read_text(encoding='utf-8')
        for name in NAMES:
            self.assertIn(name,exports)
            self.assertIn('EXPORT("%s", %s)'%(name,name),table)
        self.assertIn('#define LAP_API_MINOR 7',header)
        self.assertIn('#define LS_LAP_API2_MINOR 7u',(SDK.parent/'components/apps/tui/ls_lap_manifest.h').read_text())
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
if __name__=='__main__':unittest.main()
