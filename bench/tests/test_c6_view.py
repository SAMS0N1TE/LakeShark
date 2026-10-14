"""C6 confirmation/touch/render regressions through the real TUI, no OTA linked."""
import os
from pathlib import Path
import re
import subprocess
import unittest

ROOT=Path(__file__).resolve().parents[2]
OUT=Path(os.environ.get('LSSIM_TEST_OUT',ROOT/'bench/build-blocks/c6-ui-test'))
OUT.mkdir(parents=True,exist_ok=True)
EXE=Path(os.environ.get('LSSIM_TEST_EXE',ROOT/'bench/build-blocks/lssim.exe'))
MARK='C6 FIXTURE: confirmed action (no transport)'

class C6View(unittest.TestCase):
    def run_view(self, keys='c', script='', wide=False, busy=0, stage=-1, frames=3, advance=0, ota=0, percent=0):
        env=os.environ.copy()
        env.update(LSSIM_C6='1',LSSIM_C6_AUDIT='1',LSSIM_OTA=str(ota),LSSIM_C6_BUSY=str(busy),LSSIM_C6_STAGE=str(stage),LSSIM_C6_PCT=str(percent))
        args=[str(EXE),'update','--tft','-k',keys,'-d','-f',str(frames),'-A',str(advance),'-o',str(OUT/'frame.bmp')]
        if wide: args+=['-l']
        if script: args+=['-x',script]
        p=subprocess.run(args,env=env,capture_output=True,text=True,check=True)
        return p.stdout,(OUT/'frame.bmp').read_bytes()
    def rects(self,text):
        return [tuple(map(int,m)) for m in re.findall(r'C6 KEY (\d+) (\d+) (\d+) (\d+) (\d+)',text)]
    def test_install_only_reviews(self):
        for wide in (False,True):
            text,_=self.run_view('ci',wide=wide)
            self.assertIn('CONFIRM C6 INSTALL',text)
            self.assertNotIn(MARK,text)
            for phrase in ('INSTALL PLAN','Wi-Fi/BLE stay OFF','USB-UART recovery'):
                self.assertIn(phrase,text)
    def test_enter_cannot_accept_confirmation(self):
        text,_=self.run_view('c',script='@enter,@enter,@enter')
        self.assertIn('CONFIRM C6 INSTALL',text); self.assertNotIn(MARK,text)
    def test_confirm_is_one_shot(self):
        text,_=self.run_view('ciyy')
        self.assertEqual(text.count(MARK),1)
    def test_unarmed_y_does_nothing(self):
        text,_=self.run_view('cyy'); self.assertNotIn(MARK,text)
    def test_cancel_stays_on_c6(self):
        text,_=self.run_view('ci',script='@esc,+y')
        self.assertIn('C6 FIRMWARE / SD UPDATE',text); self.assertNotIn('CONFIRM C6 INSTALL',text)
        self.assertNotIn(MARK,text)
    def test_busy_never_confirms(self):
        for stage in range(9):
            text,_=self.run_view('ciy',busy=1,stage=stage)
            self.assertNotIn(MARK,text); self.assertNotIn('CONFIRM C6 INSTALL',text)
    def test_touch_buttons_and_confirmation(self):
        for wide in (False,True):
            text,_=self.run_view(wide=wide)
            rects=self.rects(text)
            self.assertEqual(len(rects),4)
            for _,_,_,w,h in rects:
                self.assertGreaterEqual(w,8); self.assertGreaterEqual(h,3)
            _,x,y,w,h=rects[1]; tap=f'{x+w//2}:{y+h//2}'
            text,_=self.run_view(script=tap,wide=wide)
            self.assertIn('CONFIRM C6 INSTALL',text); self.assertNotIn(MARK,text)
            text,_=self.run_view(script=tap+','+tap,wide=wide)
            self.assertEqual(text.count(MARK),1)
            _,x,y,w,h=rects[0]
            text,_=self.run_view(script=tap+f',{x+w//2}:{y+h//2},+y',wide=wide)
            self.assertNotIn(MARK,text)
    def test_options_review_also_requires_confirmation(self):
        text,_=self.run_view('co',script='@down,@down,@enter')
        self.assertIn('CONFIRM C6 INSTALL',text); self.assertNotIn(MARK,text)
    def test_force_permission_is_one_request(self):
        text,_=self.run_view('co',script='@down,@enter,@enter,@esc,@esc,+iyiy')
        self.assertEqual(text.count(MARK),2)
        self.assertEqual(text.count(MARK+' force=1'),1)
        self.assertEqual(text.count(MARK+' force=0'),1)
    def test_fixed_data_stays_still(self):
        for stage in (-1,0,3,6,7,8):
            a,first=self.run_view(stage=stage,frames=30,advance=100)
            b,later=self.run_view(stage=stage,frames=60,advance=100)
            self.assertEqual(first,later,f'timer-driven C6 pixels at {stage}')
    def test_acknowledged_bytes_change_the_meter(self):
        a,first=self.run_view(stage=3,percent=10)
        b,later=self.run_view(stage=3,percent=70)
        self.assertIn('120000 / 1200000 bytes acknowledged',a)
        self.assertIn('840000 / 1200000 bytes acknowledged',b)
        self.assertNotEqual(first,later)
    def test_p4_fixed_data_stays_still(self):
        for stage in range(10):
            _,first=self.run_view(keys='',ota=stage,frames=30,advance=100)
            _,later=self.run_view(keys='',ota=stage,frames=60,advance=100)
            self.assertEqual(first,later,f'timer-driven P4 pixels at {stage}')
    def test_read_only_worker_boundary(self):
        source=(ROOT/'main/ls_c6_update.c').read_text()
        probe=source.split('if (s_probe) {',2)[2].split('int e = esp_event_handler_instance_register')[0]
        self.assertIn('ls_c6_validate(',probe)
        self.assertIn('power(NULL)',probe)
        self.assertIn('version(NULL',probe)
        self.assertIn('goto done;',probe)
        for call in ('ls_c6_run(', 'stop(', 'begin(', 'write_chunk(', 'end(', 'activate(', 'wait_boot('):
            self.assertNotIn(call,probe)
        view=(ROOT/'components/apps/tui/ls_c6_view.c').read_text().split('void ls_c6_view_draw',1)[1].split('bool ls_c6_view_key',1)[0]
        for call in ('malloc(', 'calloc(', 'fopen(', 'ls_action_call(', 'esp_timer_get_time('):
            self.assertNotIn(call,view)

if __name__=='__main__': unittest.main()
