"""Focused simulator fidelity scenarios; no GUI, network peers or hardware.
Run after configuring bench/build: python app-sdk/tests/test_sim_runtime.py
"""
import json
import os
from pathlib import Path
import re
import subprocess
import struct
import sys
import tempfile
import unittest
SDK=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(SDK))
import lsdk
import lap


class SimulatorContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build=Path(os.environ.get('LSDK_SIM_BUILD',SDK.parent/'bench/build')).resolve()
        fixture=SDK.parent/'bench/fixtures/sdk_contract'
        env=lsdk.host_env()
        cache=(cls.build/'CMakeCache.txt').read_text()
        m=re.search(r'^LAPSIM_EXTRA_APPS:STRING=(.*)$',cache,re.M)
        apps=[p for p in (m.group(1).split(';') if m else []) if p and Path(p).is_dir()]
        if fixture.as_posix() not in apps: apps.append(fixture.as_posix())
        subprocess.run(['cmake','-S',str(SDK.parent/'bench'),'-B',str(cls.build),'-DLAPSIM_EXTRA_APPS='+';'.join(apps),
                        '-DLSSIM_DYNAMIC_APPS='+('ON' if os.environ.get('LSDK_TEST_DYNAMIC') else 'OFF')],env=env,check=True,stdout=subprocess.DEVNULL)
        subprocess.run(['cmake','--build',str(cls.build),'--target','lssim','-j','8'],env=env,check=True,stdout=subprocess.DEVNULL)
        lap.build(fixture,cls.build/'sdk-contract-fixture.lap',log=[])
        cls.dll=None
        if os.environ.get('LSDK_TEST_DYNAMIC'):
            cls.dll=cls.build/'sdk-contract-fixture.dll'
            subprocess.run(['gcc','-std=gnu11','-shared','-static-libgcc','-Wl,--export-all-symbols',
                            '-include',str(SDK/'tools/sim_alloc.h'),'-I'+str(SDK/'include'),'-I'+str(SDK/'lib'),
                            str(fixture/'main.c'),*map(str,(SDK/'lib').glob('*.c')),str(cls.build/'liblssim.dll.a'),
                            '-o',str(cls.dll)],env=env,check=True)


    def run_app(self, directory, script='', consent='grant', hardware=None, radio=None, events=None):
        env=dict(lsdk.host_env(),LAPSIM='sdk_contract',LAPSIM_SCRIPT=script,LAPSIM_CONSENT=consent)
        for k in ('LAPSIM_DLL','LSSIM_FRAMES','LAPSIM_RADIO'): env.pop(k,None)
        env.pop('LAPSIM_HW',None)
        if hardware is not None: env['LAPSIM_HW']=hardware
        if radio is not None: env['LAPSIM_RADIO']=radio
        if self.dll: env['LAPSIM_DLL']=str(self.dll)
        exe=self.build/('lssim.exe' if os.name=='nt' else 'lssim')
        r=subprocess.run([str(exe),'lap','-A','40','-f','40']+(['--ev',events] if events else []),cwd=directory,env=env,capture_output=True,
                         text=True,errors='replace',timeout=60,creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        return r.stdout+r.stderr

    def test_stack_sync_accepted_retained_and_invalid_rejected(self):
        with tempfile.TemporaryDirectory(dir=self.build) as d:
            text=self.run_app(d,'4:key=p;5:key=r;6:key=z')
            self.assertIn('fixture stack sync 1',text)
            self.assertRegex(text,r'simulator contract: lap_sd_read EINVAL:.*stack storage')
            self.assertRegex(text,r'simulator contract: lap_net_status EINVAL: synchronous pointer')
            self.assertIn('fixture tracked heap 1',text)

    def test_caps_present_and_absent(self):
        with tempfile.TemporaryDirectory(dir=self.build) as d:
            text=self.run_app(d,'4:key=h',hardware='wifi,ble,gps,sd,desktop')
            self.assertIn('fixture caps 151 radio 1',text)
            text=self.run_app(d,'4:key=h',hardware='sd,desktop',radio='none')
            self.assertIn('fixture caps 144 radio 0',text)
            self.assertNotIn('simulator contract:',text)

    def test_held_keys_pointer_focus_resize(self):
        with tempfile.TemporaryDirectory(dir=self.build) as d:
            text=self.run_app(d,'2:key=i;3:keydown=W;4:keydown=@left;5:buttondown=30,40,2;6:wheel=-3;7:keyup=W;8:buttonup=30,40,2;9:resize=800,600;10:focus=0;11:focus=1')
            for expected in ('fixture held 1 1 buttons 4 xy 30 40','fixture held 0 1','fixture held 0 0','wheel -3','size 800 600'):
                self.assertIn(expected,text)
            self.assertNotIn('simulator contract:',text)

    def test_native_key_mouse_queue(self):
        def event(kind,key=0,ch=0,button=0,buttons=0,wheel=0):
            return struct.pack('<7B5hHBI',1,kind,0,0,button,buttons,1,15,8,150,150,wheel,key,ch,1).hex()
        wire=(event(0,1,ord('i'))+event(1,1,ord('i'))+event(0,1,ord('W'))+
              event(3,button=2,buttons=4)+event(5,buttons=4,wheel=-2))
        with tempfile.TemporaryDirectory(dir=self.build) as d:
            text=self.run_app(d,events=wire)
            self.assertIn('fixture held 1 0 buttons 4',text)
            self.assertIn('wheel -2',text)
            self.assertNotIn('simulator contract:',text)

    def test_scripted_dialogs(self):
        with tempfile.TemporaryDirectory(dir=self.build) as d:
            text=self.run_app(d,'3:key=t;5:dialogtext=Grace;7:key=c;9:dialogpick=1;11:key=o;13:dialogpick=0;15:key=t;17:dialogcancel')
            for expected in ('fixture modal 1','fixture dialog 1 value -1 text Grace','fixture dialog 1 value 1','fixture dialog 1 value 0','fixture dialog 2'):
                self.assertIn(expected,text)
            self.assertNotIn('simulator contract:',text)

    def test_saves_settings_persist_on_sd(self):
        with tempfile.TemporaryDirectory(dir=self.build) as d:
            text=self.run_app(d,'3:key=s;5:key=v;7:key=g')
            for expected in ('fixture save 1','fixture setting 1','fixture load 1 schema 7','fixture preference 1'):
                self.assertIn(expected,text)
            target=Path(d)/'sdcard/apps_data/sdk_contract'
            self.assertEqual((target/'save_progress').read_bytes()[:4],b'LSAV')
            self.assertEqual((target/'pref_volume').read_bytes()[:4],b'LSAV')
            text=self.run_app(d,'3:key=g')
            self.assertIn('fixture load 1 schema 7',text)
            self.assertIn('fixture preference 1',text)
            self.assertNotIn('simulator contract:',text)

    def test_manifest_prevents_tx(self):
        with tempfile.TemporaryDirectory(dir=self.build) as d:
            text=self.run_app(d,'4:mesh;5:key=m')
            self.assertIn('lap_mesh_advert permission denied (LAP_CAP_RADIO_TX)',text)

    def test_denied_consent_never_enters(self):
        with tempfile.TemporaryDirectory(dir=self.build) as d:
            text=self.run_app(d,consent='deny')
            self.assertIn('simulator consent: launch denied',text)
            self.assertNotIn('fixture persisted',text)

    def test_workers_hidden_service_reopen_stop_and_flush(self):
        with tempfile.TemporaryDirectory(dir=self.build) as d:
            text=self.run_app(d,'8:hide;18:show;24:hide;30:stop')
            self.assertNotIn('simulator contract:',text)
            for expected in ('fixture network allowed 1','fixture tracked heap 1','fixture SDK helpers 1','fixture sd roundtrip 1',
                             'fixture hidden tick','fixture reopened','fixture stopped'):
                self.assertIn(expected,text)
            self.assertEqual((Path(d)/'sdcard/contract.bin').read_bytes(),b'hello')
            text=self.run_app(d)
            self.assertIn('fixture persisted 2',text)
            self.assertNotIn('simulator contract:',text)


if __name__=='__main__': unittest.main()
