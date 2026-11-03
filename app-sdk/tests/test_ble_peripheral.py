"""API 2.6 peripheral journeys, metadata and 228-byte legacy ABI. No RF/GUI."""
import json
import os
from pathlib import Path
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

class Peripheral(unittest.TestCase):
 def test_registry_and_example_use(self):
  block=(SDK/'exports.txt').read_text().split('# API 2.6: Bluetooth peripheral roles')[1].split('# API ',1)[0]
  names=[n for n in block.split() if n.startswith('lap_')]
  self.assertEqual(len(names),12)
  calls={e['name']:e for e in apidoc.entries() if e.get('exported')}
  examples='\n'.join((SDK/'apps'/app/'main.c').read_text() for app in ('ble_beacon','gatt_sensor','board_gamepad'))
  for name in names:
   self.assertEqual(calls[name]['since'],'2.6',name)
   self.assertEqual(calls[name]['simulator']['status'],'limited',name)
   self.assertIn(name+'(',examples,name)
 def run_app(self,name,script='',hardware='ble,gps,sd,desktop',features='hid'):
  self.assertTrue(EXE.exists(),'build lssim first')
  folder=BUILD/'peripheral-scenarios'/(name+'_'+str(len(script))+'_'+features+'_'+str(len(hardware)))
  folder.mkdir(parents=True,exist_ok=True)
  env=dict(lsdk.host_env(),LAPSIM=name,LAPSIM_SCRIPT=script,LAPSIM_HW=hardware,LAPSIM_PERIPHERAL=features,LAPSIM_CONSENT='grant')
  for key in ('LAPSIM_DLL','LSSIM_FRAMES'):env.pop(key,None)
  r=subprocess.run([str(EXE),'lap','-A','40','-f','70'],env=env,cwd=folder,capture_output=True,text=True,errors='replace',timeout=90,
   creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)
  output=r.stdout+r.stderr;(folder/'run.log').write_text(output,encoding='utf-8')
  self.assertEqual(r.returncode,0,output[-4000:]);self.assertNotIn('simulator contract:',output)
  return output
 def test_missing_and_trimmed_features(self):
  for app in ('ble_beacon','gatt_sensor','board_gamepad'):
   with self.subTest(app=app):
    self.run_app(app,hardware='none')
    self.run_app(app,features='none')
  self.run_app('gatt_sensor',features='advertiser')
  self.run_app('board_gamepad',features='gatt')
 def test_beacon_start_stop(self):
  text=self.run_app('ble_beacon','10:key=s;20:key=a;40:hide;50:stop')
  self.assertIn('peripheral simulator: advertising',text)
  self.assertIn('peripheral simulator: closed',text)
 def test_sensor_read_and_reconnect(self):
  text=self.run_app('gatt_sensor','8:periphconnect;9:periphmtu=185;10:periphbond=1;12:periphsubscribe=2,1;13:periphsubscribe=5,1;15:periphread=2;16:periphread=5;35:periphdisconnect;40:periphconnect;45:periphread=3;52:hide;60:stop')
  self.assertIn('peripheral fixture: read id=2 ATT=0',text)
  self.assertIn('peripheral fixture: read id=5 ATT=0',text)
  self.assertIn('peripheral simulator: sent id=2',text)
 def test_hid_reports_and_neutral_on_hide(self):
  text=self.run_app('board_gamepad','8:periphconnect;9:periphbond=1;10:periphsubscribe=6,1;12:keydown= ;13:keydown=@right;20:hide;40:show;50:periphdisconnect;60:stop')
  self.assertIn('peripheral simulator: sent id=6 indicate=0 data=010064000000',text)
  self.assertIn('peripheral simulator: sent id=6 indicate=0 data=000000000000',text)
 def test_lease_failure_and_controller_failure(self):
  self.run_app('ble_beacon','1:peripherror=-105;8:key=s;10:peripherror=0;15:key=a;40:key=s')
  self.run_app('board_gamepad','1:peripherror=-27;10:key=s;15:peripherror=0;20:key=a;40:stop')
 def test_rv32_descriptor_and_legacy25(self):
  folder=BUILD/'peripheral-scenarios/abi';folder.mkdir(parents=True,exist_ok=True)
  base='#include <lakeshark_app.h>\n_Static_assert(sizeof(lap_app_t)==228,"descriptor ABI");\n'
  (folder/'main.c').write_text('#define LAP_TARGET_API_25\n'+base+'#ifdef LAP_API_26\n#error new API leaked\n#endif\nLAP_APP(.id="legacy25",.name="LEGACY",.version="1.0");\n')
  output=lap.build(folder,folder/'legacy25.lap',log=[])
  self.assertEqual(lap.Elf(output.read_bytes()).manifest['api_minor'],5)
  sizes=[('lap_ble_advert_config_t',80),('lap_gatt_attribute_t',162),('lap_gatt_server_event_t',20),('lap_ble_peripheral_status_t',24)]
  (folder/'main.c').write_text(base+'\n'.join('_Static_assert(sizeof(%s)==%d,"wire ABI");'%pair for pair in sizes)+'\nLAP_APP(.id="periph26",.name="PERIPHERAL",.version="1.0",.flags=LAP_CAP_RADIO_TX|LAP_CAP_HID_DEVICE);\n')
  output=lap.build(folder,folder/'peripheral26.lap',log=[]);elf=lap.Elf(output.read_bytes())
  self.assertEqual(elf.manifest['api_minor'],6)
  self.assertEqual(elf.sections[elf.names.index('.lap_app')][5],228)

if __name__=='__main__':unittest.main()
