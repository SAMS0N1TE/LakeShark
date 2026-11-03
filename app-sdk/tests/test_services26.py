"""API 2.6 resident lifecycle and real worker/channel headless integration."""
from pathlib import Path
import json
import os
import re
import subprocess
import sys
import unittest
SDK=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(SDK))
import apidoc
import lap
import lsdk
BUILD=SDK.parent/'bench/build'

class Services26(unittest.TestCase):
    def run_sim(self,app,script='',services='',radio='sim',frames=100):
        env=lsdk.host_env()
        env.update(LAPSIM=app,LAPSIM_SCRIPT=script,LAPSIM_SERVICES=services,LAPSIM_RADIO=radio,LAPSIM_CONSENT='grant')
        for key in ('LAPSIM_DLL','LSSIM_FRAMES'): env.pop(key,None)
        folder=BUILD/'services26'/app; folder.mkdir(parents=True,exist_ok=True)
        r=subprocess.run([str(BUILD/'lssim.exe'),'lap','-A','40','-f',str(frames)],cwd=folder,env=env,
            capture_output=True,text=True,errors='replace',timeout=30,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)
        output=r.stdout+r.stderr
        (folder/('run-'+radio+'.log')).write_text(output)
        self.assertEqual(r.returncode,0,output[-3000:])
        self.assertNotIn('simulator contract:',output)
        return output

    def test_two_hidden_services_and_stop(self):
        text=self.run_sim('dice','50:services;60:service_stop=service_logger;80:services',
            'service_notifier,service_logger')
        self.assertIn('logger record 4',text)
        self.assertIn('notifier received 4',text)
        self.assertRegex(text,r'services count=2')
        self.assertRegex(text,r'services count=1\s+service id=service_notifier shown=0 ticks=\d+')
        snapshots=re.findall(r'service id=service_notifier shown=0 ticks=(\d+)',text)
        self.assertEqual(len(snapshots),2); self.assertGreater(int(snapshots[1]),int(snapshots[0]))

    def test_custom_protocol(self):
        text=self.run_sim('custom_decoder','0:dspook=90')
        self.assertIn('custom decoded 90',text)
        self.assertNotIn('state=3',text)

    def test_overrun_and_absent_radio(self):
        text=self.run_sim('custom_decoder','0:dspcost=20000')
        self.assertIn('custom worker state=3 overruns=1',text)
        self.assertNotIn('custom decoded',text)
        self.assertNotIn('custom worker',self.run_sim('custom_decoder',radio='none',frames=20))

    def test_registry_and_abi(self):
        entries={e['name']:e for e in apidoc.entries() if e.get('exported')}
        names=['lap_service_list','lap_service_query','lap_service_limit','lap_rx_dsp_start','lap_rx_dsp_status','lap_rx_dsp_next','lap_rx_dsp_close']
        for name in names:
            self.assertEqual(entries[name]['since'],'2.6')
            self.assertEqual(entries[name]['simulator']['status'],'limited')
        folder=BUILD/'services26/legacy25';folder.mkdir(parents=True,exist_ok=True)
        (folder/'main.c').write_text('#define LAP_TARGET_API_25\n#include "lakeshark_app.h"\n#ifdef LAP_API_26\n#error API 2.6 leaked\n#endif\nLAP_APP(.id="legacy25",.name="LEGACY 25",.version="1");\n')
        elf=lap.Elf(lap.build(folder,folder/'legacy25.lap').read_bytes())
        self.assertEqual(elf.manifest['api_minor'],5)
        self.assertEqual(elf.sections[elf.names.index('.lap_app')][5],228)

if __name__=='__main__': unittest.main()
