"""Syntax-check SDK service/device integration using an existing IDF compilation database.
No configure, link, image generation or changes to the database's build directory.
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import shlex
import subprocess

ROOT=Path(__file__).resolve().parents[2]
FILES={
 'components/apps/tui/ls_lap_net_audio.c':'ls_lap_sys.c',
 'components/apps/tui/ls_lap_wifi_http.c':'ls_lap_sys.c',
 'components/apps/tui/ls_lap_udp.c':'ls_lap_sys.c',
 'components/apps/tui/ls_lap_mdns.c':'ls_lap_sys.c',
 'components/apps/tui/ls_lap_ble_client.c':'ls_lap_sys.c',
 'components/apps/tui/ls_lap_sound.c':'ls_lap_sys.c',
 'components/apps/tui/ls_lap_api.c':'ls_lap_sys.c',
 'components/apps/tui/ls_lap_sys.c':'ls_lap_sys.c',
 'components/apps/tui/ls_lap_manifest.c':'ls_lap_sys.c',
 'components/apps/tui/ls_lap_radio.c':'ls_lap_sys.c',
 'components/apps/tui/ls_lap_ui.c':'ls_lap_sys.c',
 'components/apps/tui/ls_tui_screen.c':'ls_lap_sys.c',
 'components/apps/tui/desk/ls_desk.c':'ls_lap_sys.c',
 'components/lakeshark/audio/audio_out.c':'audio_out.c',
 'components/lakeshark/core/ls_time.c':'ls_time.c',
 'main/ble_lap_client.c':'ble_hid.c',
 'main/ble_hid.c':'ble_hid.c',
 'main/ble_link.c':'ble_link.c',
 'main/ble_link_stub.c':'ble_link.c',
}

def split(command):
 if os.name!='nt':return shlex.split(command)
 count=ctypes.c_int()
 fn=ctypes.windll.shell32.CommandLineToArgvW
 fn.restype=ctypes.POINTER(ctypes.c_wchar_p)
 fn.argtypes=[ctypes.c_wchar_p,ctypes.POINTER(ctypes.c_int)]
 ptr=fn(command,ctypes.byref(count))
 try:return [ptr[i] for i in range(count.value)]
 finally:ctypes.windll.kernel32.LocalFree(ptr)

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('database',type=Path);args=p.parse_args()
 db=json.loads(args.database.read_text());oldroot=Path(next(e['file'] for e in db if Path(e['file']).name=='ble_hid.c')).parents[1].as_posix()
 out=ROOT/'bench/build';out.mkdir(exist_ok=True)
 failed=0
 for path,base in FILES.items():
  entry=next(e for e in db if Path(e['file']).name==base)
  original=entry.get('arguments') or split(entry['command']);flags=[];skip=False
  for arg in original[1:]:
   if skip:skip=False;continue
   if arg in ('-o','-MF','-MT','-MQ'):skip=True;continue
   if arg in ('-c','-MD','-MMD','-MP','-fstack-usage') or arg==entry['file']:continue
   # Keep generated configuration/text assets in their original read-only build.
   norm=arg.replace('\\','/')
   if oldroot in norm and oldroot+'/build' not in norm:norm=norm.replace(oldroot,ROOT.as_posix())
   if 'C:/esp/v5.5.4/esp-idf' in norm and not Path('C:/esp/v5.5.4/esp-idf').exists():norm=norm.replace('C:/esp/v5.5.4/esp-idf','C:/Espressif/frameworks/esp-idf-v5.5.4')
   flags.append(norm)
  flags.extend(['-I'+str(ROOT/'managed_components/espressif__mdns/include'),'-fsyntax-only',str(ROOT/path)])
  r=subprocess.run([original[0],*flags],cwd=out,capture_output=True,text=True,errors='replace')
  (out/(Path(path).stem+'-device-syntax.log')).write_text(r.stdout+r.stderr,encoding='utf-8')
  print(('PASS' if not r.returncode else 'FAIL'),path,flush=True)
  if r.returncode:failed+=1;print((r.stdout+r.stderr)[-4500:])
 return bool(failed)
if __name__=='__main__':raise SystemExit(main())
