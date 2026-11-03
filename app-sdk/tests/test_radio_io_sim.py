"""Interactive API 2.4 examples over headless lssim (build lssim first)."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
SDK=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(SDK))
import lsdk
BUILD=SDK.parent/'bench/build'
BAD=re.compile(r'simulator contract:.*(?:EINVAL|permission denied|flush failed)',re.I)
class RadioIO(unittest.TestCase):
    def run_app(self,app,script,missing=False,nfc=False):
        with tempfile.TemporaryDirectory(dir=BUILD) as d:
            env=dict(lsdk.host_env(),LAPSIM=app,LAPSIM_SCRIPT=script,LAPSIM_CONSENT='grant')
            for k in ('LAPSIM_DLL','LSSIM_FRAMES','LAPSIM_NFC','LAPSIM_RADIO','LAPSIM_HW'):env.pop(k,None)
            if missing:env.update(LAPSIM_RADIO='none',LAPSIM_HW='none')
            if nfc:env['LAPSIM_NFC']='1'
            exe=BUILD/('lssim.exe' if os.name=='nt' else 'lssim')
            run=subprocess.run([str(exe),'lap','-A','40','-f','65'],cwd=d,env=env,capture_output=True,text=True,errors='replace',timeout=60,creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)
            text=run.stdout+run.stderr
            self.assertEqual(run.returncode,0,text)
            self.assertIsNone(BAD.search(text),text)
            (BUILD/('radio-io-'+app+('-missing' if missing else '')+'.log')).write_text(text,encoding='utf-8')
            return list(Path(d).glob('sdcard/apps_data/*/*.lsr')) and any(p.read_bytes().startswith(b'LSR2') for p in Path(d).glob('sdcard/apps_data/*/*.lsr'))
    def test_packet_record_local_replay_and_send(self):
        self.assertTrue(self.run_app('packet_io','2:packethex=0102aaff;4:key=r;6:packet=record payload;7:key=m;8:key=r;12:key=p;15:key=k;18:key=x;22:key=t;27:key=x'))
    def test_fsk_ook_and_iq_send(self):
        self.run_app('packet_io','3:key=i;8:key=f;12:key=t;18:key=f;21:edges=100,-200,300,-400;24:key=r;28:edges=100,-200,300,-400;31:key=r')
    def test_scanner_database_and_controls(self):
        self.run_app('scanner_db','3:key=w;8:iorssi=-650;12:key=h;15:key=n;18:key=s;22:key=h;28:key=x')
    def test_nfc_read_write_remove_and_lock(self):
        self.run_app('nfc_tags','2:nfc=sample;6:key=w;12:nfc=removed;16:nfc=sample2;22:nfclock=1;27:key=w',nfc=True)
    def test_missing_hardware_and_radio_arrival(self):
        for app in ('packet_io','scanner_db','nfc_tags'):
            with self.subTest(app=app):self.run_app(app,'10:radio=sim',missing=True)
if __name__=='__main__':unittest.main()
