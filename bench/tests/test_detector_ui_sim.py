"""Touch regression against production UI, picker and detector worker in lssim."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[2]
EXE=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else ROOT/'bench/build/lssim.exe'
env=os.environ.copy()
env['LSSIM_DETECTOR']='1'
checks=0
with tempfile.TemporaryDirectory() as tmp:
    for wide in (False,True):
        def run(script):
            cmd=[str(EXE),'detector','--tft','-A','40',
                 '-d','-x',script,'-o',str(Path(tmp)/'frame.bmp')]
            if wide:cmd+=['-l']
            return subprocess.run(cmd,cwd=ROOT,env=env,check=True,capture_output=True,text=True).stdout
        options='48:26' if wide else '43:60'
        rule='45:12' if wide else '10:39'
        inc='102:18' if wide else '46:30'
        value='92:18' if wide else '37:30'
        out=run(f'{options},{rule},{inc}')
        assert 'DETECTOR > Rules' in out and 'RTL-SDR' not in out
        assert '10' in next(line for line in out.splitlines() if '<' in line and '10' in line)
        checks+=1
        out=run(f'{options},{rule},{value}')
        assert 'seconds' in out and 'CANCEL' in out and 'OK' in out
        checks+=1
        test='105:26' if wide else '43:64'
        out=run(f'{test},~12,@esc')
        assert all(word in out for word in ('SINK TEST RESULT','BANNER','VIBRATE','SD LOG','ACCEPTED','FAILED'))
        checks+=1
        stop='8:26' if wide else '8:60'
        out=run(stop)
        assert 'START' in out and '.OFF.' in out
        assert 'RSSI -110..-30 / dots per match' in out
        checks+=1
print(f'PASS: {checks} production touch checks, portrait and landscape')
