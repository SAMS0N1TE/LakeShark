"""Phase 2 headless example scenarios; build lssim in bench/build first."""
import json
import os
from pathlib import Path
import subprocess
import sys
import unittest
import wave
SDK=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(SDK))
import apidoc
import lap
import lsdk
BUILD=SDK.parent/'bench/build'
EXE=BUILD/('lssim.exe' if os.name=='nt' else 'lssim')

class NetAudio(unittest.TestCase):
 def test_registry_and_example_use(self):
  block=(SDK/'exports.txt').read_text().split('# API 2.4: networking, Bluetooth and audio')[1].split('# API 2.5: system services')[0]
  names=block.split()
  calls={e['name']:e for e in apidoc.entries() if e.get('exported')}
  examples='\n'.join((SDK/'apps'/app/'main.c').read_text() for app in ('tcp_probe','ble_explorer','sound_board'))
  for name in names:
   self.assertEqual(calls[name]['since'],'2.4',name)
   self.assertNotEqual(calls[name]['simulator']['status'],'missing',name)
   self.assertIn(name+'(',examples,name)
  self.assertEqual(len(names),len(set(names)))
 def run_app(self,name,script,hardware):
  if not EXE.exists():self.skipTest('build lssim first')
  folder=BUILD/'net-audio-scenarios'/(name+'_'+str(len(script))+'_'+str(len(hardware)))
  folder.mkdir(parents=True,exist_ok=True)
  env=dict(lsdk.host_env(),LAPSIM=name,LAPSIM_SCRIPT=script,LAPSIM_HW=hardware,LAPSIM_CONSENT='grant')
  for key in ('LAPSIM_DLL','LSSIM_FRAMES'):env.pop(key,None)
  r=subprocess.run([str(EXE),'lap','-A','40','-f','60'],env=env,cwd=folder,capture_output=True,text=True,errors='replace',timeout=90,
                   creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)
  (folder/'run.log').write_text(r.stdout+r.stderr,encoding='utf-8')
  self.assertEqual(r.returncode,0,(r.stdout+r.stderr)[-4000:])
  self.assertNotIn('simulator contract:',r.stdout+r.stderr)
  return folder
 def test_missing_hardware(self):
  for name in ('tcp_probe','ble_explorer','sound_board'):
   with self.subTest(app=name):self.run_app(name,'','none')
 def test_network_failures_and_datagrams(self):
  self.run_app('tcp_probe','0:httperror=-109;3:key=u;5:key=s;8:key=m;10:key=n;12:mdnserror=-111;14:key=v;17:key=t;20:key=d;25:key=a;30:key=x;35:hide','wifi,sd,desktop')
 def test_gatt_and_advertisements(self):
  self.run_app('ble_explorer','1:blead=-63,02010603030f1804ff341201;5:key=c;10:key=d;15:key=g;20:key=e;25:key=r;30:key=w;35:key=n;40:blenotify=1234;45:key=u;48:key=x;52:hide','ble,sd,desktop')
 def test_recording_and_sample(self):
  folder=self.run_app('sound_board','2:key=1;3:key=2;4:key=3;5:key=m;8:key=r;28:key=s;32:key=p;48:hide','mic,speaker,sd,desktop')
  with wave.open(str(folder/'sdcard/apps_data/sound_board/recording.wav'),'rb') as f:
   self.assertEqual((f.getnchannels(),f.getsampwidth(),f.getframerate()),(1,2,16000))
   self.assertGreater(f.getnframes(),0)
 def test_rv32_descriptor_and_legacy_target(self):
  folder=BUILD/'net-audio-scenarios/legacy';folder.mkdir(parents=True,exist_ok=True)
  (folder/'main.c').write_text('#define LAP_TARGET_API_23\n#include <lakeshark_app.h>\nLAP_APP(.id="legacy23",.name="LEGACY",.version="1.0");\n')
  path=lap.build(folder,folder/'legacy.lap',log=[])
  self.assertEqual(lap.Elf(path.read_bytes()).manifest['api_minor'],3)
  path=lap.build(SDK/'apps/sound_board',folder/'sound.lap',log=[])
  self.assertEqual(lap.Elf(path.read_bytes()).manifest['api_minor'],lap.sdk_api_version(SDK)[1])

if __name__=='__main__':unittest.main()
