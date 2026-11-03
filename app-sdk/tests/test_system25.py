"""Headless API 2.5 system scenarios and RV32 compatibility checks."""
import os
from pathlib import Path
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
EXE=BUILD/('lssim.exe' if os.name=='nt' else 'lssim')

class System25(unittest.TestCase):
 def test_registry_and_examples(self):
  block=(SDK/'exports.txt').read_text().split('# API 2.5: system services')[1].split('# API ', 1)[0]
  names=block.split()
  calls={e['name']:e for e in apidoc.entries() if e.get('exported')}
  examples='\n'.join((SDK/'apps'/app/'main.c').read_text() for app in ('reminder','scoped_files','topic_reader','topic_writer'))
  self.assertEqual(len(names),22)
  for name in names:
   self.assertEqual(calls[name]['since'],'2.5',name)
   self.assertEqual(calls[name]['simulator']['status'],'limited',name)
   self.assertIn(name+'(',examples,name)
 def run_app(self,app,script='',hardware='sd,desktop'):
  folder=BUILD/'system-scenarios'/(app+'_'+str(len(script))+'_'+str(len(hardware)))
  folder.mkdir(parents=True,exist_ok=True)
  (folder/'sdcard/shared').mkdir(parents=True,exist_ok=True)
  (folder/'sdcard/shared/hello.txt').write_text('Hello selected file\n')
  env=dict(lsdk.host_env(),LAPSIM=app,LAPSIM_SCRIPT=script,LAPSIM_HW=hardware,LAPSIM_CONSENT='grant')
  for key in ('LAPSIM_DLL','LSSIM_FRAMES'):env.pop(key,None)
  r=subprocess.run([str(EXE),'lap','-A','40','-f','90'],env=env,cwd=folder,capture_output=True,text=True,errors='replace',timeout=90,
                   creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)
  output=r.stdout+r.stderr
  (folder/'run.log').write_text(output,encoding='utf-8')
  self.assertEqual(r.returncode,0,output[-4000:])
  self.assertNotIn('simulator contract:',output)
  return folder,output
 def test_notice_background_activation(self):
  folder,output=self.run_app('reminder','2:desktop=1;5:hide;35:noticeaction=1;45:show;50:focus=0;52:focus=1;55:resize=640,480;60:desktop=0;65:key=d;70:key=u;75:key=p')
  self.assertIn('lap_notify_post',output)
  self.assertIn('lap_launch_data',output)
  self.assertIn('lap_desktop_open_url',output)
 def test_picker_file_and_folder(self):
  folder,output=self.run_app('scoped_files','2:key=p;8:pick=4;14:pick=4;20:key=f;26:pick=4;32:pick=2;40:key=h;44:key=s;48:key=c;53:key=w;56:key=p;62:pickcancel;65:key=n')
  self.assertIn('lap_file_picker_poll',output)
  self.assertIn('lap_stream_read',output)
  self.assertIn('lap_stream_write',output)
 def test_missing_hardware(self):
  for app in ('reminder','scoped_files','topic_reader','topic_writer'):
   with self.subTest(app=app): self.run_app(app,hardware='none')
 def test_rv32_and_legacy24(self):
  folder=BUILD/'system-scenarios/abi';folder.mkdir(parents=True,exist_ok=True)
  base='#include <lakeshark_app.h>\n_Static_assert(sizeof(lap_app_t)==228,"descriptor ABI");\n'
  (folder/'main.c').write_text('#define LAP_TARGET_API_24\n'+base+'#ifdef LAP_API_25\n#error 2.5 leaked into 2.4\n#endif\nLAP_APP(.id="legacy24",.name="LEGACY",.version="1.0");\n')
  path=lap.build(folder,folder/'legacy24.lap',log=[])
  self.assertEqual(lap.Elf(path.read_bytes()).manifest['api_minor'],4)
  (folder/'main.c').write_text('#define LAP_TARGET_API_25\n'+base+'#ifdef LAP_API_26\n#error 2.6 leaked into 2.5\n#endif\nLAP_APP(.id="legacy25",.name="LEGACY",.version="1.0");\n')
  path=lap.build(folder,folder/'legacy25.lap',log=[])
  self.assertEqual(lap.Elf(path.read_bytes()).manifest['api_minor'],5)
  asserts='\n'.join('_Static_assert(sizeof(%s)==%d,"wire ABI");' % p for p in
                    [('lap_notification_t',316),('lap_launch_data_t',144),('lap_dir_entry_t',76),('lap_ipc_message_t',272),('lap_desktop_event_t',28)])
  (folder/'main.c').write_text(base+asserts+'\nLAP_APP(.id="system25",.name="SYSTEM",.version="1.0");\n')
  path=lap.build(folder,folder/'system25.lap',log=[])
  self.assertEqual(lap.Elf(path.read_bytes()).manifest['api_minor'],lap.sdk_api_version(SDK)[1])

if __name__=='__main__':unittest.main()
