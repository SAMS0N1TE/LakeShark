"""API 2.6 new examples, headless and scripted; no RF, native dialogs or browser."""
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import unittest
SDK=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(SDK))
import lap
import lsdk
BUILD=SDK.parent/'bench/build'
BAD=re.compile(r'simulator contract:.*(?:EINVAL|permission denied|flush failed)',re.I)


class TxDesktop26(unittest.TestCase):
    def run_app(self, app, script, radio=None, replay=False):
        with tempfile.TemporaryDirectory(dir=BUILD) as directory:
            folder=Path(directory)
            env=dict(lsdk.host_env(), LAPSIM=app,LAPSIM_SCRIPT=script,LAPSIM_CONSENT='grant')
            for key in ('LAPSIM_DLL','LSSIM_FRAMES','LAPSIM_NFC','LAPSIM_RADIO','LAPSIM_HW'):env.pop(key,None)
            if radio:env['LAPSIM_RADIO']=radio
            if replay:
                data=folder/'sdcard/apps_data/hackrf_signal';data.mkdir(parents=True)
                header=struct.pack('<4sHHIIQ',b'LSR2',1,128,1,2000000,100000000).ljust(128,b'\0')
                # 48-byte framed block, 32 signed IQ16 pairs; no discontinuity marker.
                block=bytearray(48)
                struct.pack_into('<I',block,0,128)
                struct.pack_into('<I',block,4,1)
                struct.pack_into('<I',block,40,2000000)
                (data/'signal.lsr').write_bytes(header+block+struct.pack('<64h',*([8192,0,0,8192,-8192,0,0,-8192]*8)))
            exe=BUILD/('lssim.exe' if os.name=='nt' else 'lssim')
            run=subprocess.run([str(exe),'lap','-A','40','-f','65'],cwd=folder,env=env,capture_output=True,text=True,errors='replace',timeout=60,creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)
            text=run.stdout+run.stderr
            (BUILD/(app+'-scripted'+('-missing' if radio=='none' else '')+'.log')).write_text(text,encoding='utf-8')
            self.assertEqual(run.returncode,0,text)
            self.assertIsNone(BAD.search(text),text)
            return text

    def test_hackrf_generate_replay_cancel_limits_and_unplug(self):
        text=self.run_app('hackrf_signal','5:key=t;10:key=r;16:hackrflimits=0,0;20:key=+;22:key=t;28:key=c;34:radio=none',radio='hackrf',replay=True)
        for name in ('lap_tx_hackrf_config','lap_tx_iq','lap_tx_replay','lap_tx_status'):
            self.assertIn('simulator limit: '+name+':',text)

    def test_desktop_export_url_cancel_and_scale(self):
        text=self.run_app('desktop_export','1:desktopcaps=3,1500;7:key=s;12:desktopresult=1;18:key=u;22:key=c;30:desktop=0')
        for name in ('lap_desktop_save_file','lap_desktop_open_url','lap_desktop_request_status','lap_desktop_request_cancel'):
            self.assertIn('simulator limit: '+name+':',text)

    def test_missing_hardware_and_old_viewer(self):
        self.run_app('hackrf_signal','5:key=t;10:key=r',radio='none')
        text=self.run_app('desktop_export','1:desktop=1;8:key=s;14:key=u')
        self.assertNotIn('simulator limit: lap_desktop_save_file:',text)
        self.assertNotIn('simulator limit: lap_desktop_open_url:',text)

    def test_rv32_new_examples_keep_descriptor(self):
        for app in ('hackrf_signal','desktop_export'):
            file=lap.build(SDK/'apps'/app,BUILD/(app+'-api26.lap'),log=[])
            elf=lap.Elf(file.read_bytes())
            self.assertEqual((elf.manifest['api_major'],elf.manifest['api_minor']),(2,6))


if __name__=='__main__':unittest.main()
