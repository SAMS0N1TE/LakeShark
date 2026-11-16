"""API 2.8 network diagnostics: the LAN SCAN, NET DIAG and THROUGHPUT apps over
headless lssim (build lssim first; LS_BENCH_BUILD names the build folder).

lssim answers lap_ping/lap_dns/lap_arp/lap_net_info from its fixed LAN
(192.0.2.0/24). lap_tcp and lap_udp stay real host sockets, so the WoL and
iperf2 checks talk to listeners this test opens on 127.0.0.1."""
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
SDK=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(SDK))
import lsdk
BUILD=Path(os.environ.get('LS_BENCH_BUILD') or SDK.parent/'bench/build')
LSSIM=BUILD/('lssim.exe' if os.name=='nt' else 'lssim')
NAMES=['lap_net_info','lap_ping_start','lap_ping_next','lap_ping_stop','lap_dns_resolve','lap_dns_result',
       'lap_arp_probe','lap_arp_table']


def run(app,script,frames,grid=None,folder=None,hw=None):
    env=lsdk.host_env()
    env.update(LAPSIM=app,LAPSIM_SCRIPT=script,LAPSIM_CONSENT='grant',LAPSIM_RADIO='none')
    for k in ('LAPSIM_DLL','LSSIM_FRAMES','LAPSIM_HW','LSSIM_GRID'): env.pop(k,None)
    if grid: env['LSSIM_GRID']=grid
    if hw: env['LAPSIM_HW']=hw
    r=subprocess.run([str(LSSIM),'lap','-A','40','-d','-f',str(frames)],cwd=folder,env=env,capture_output=True,
                     text=True,errors='replace',timeout=120,
                     creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)
    text=r.stdout+r.stderr
    assert r.returncode==0,text[-3000:]
    assert 'simulator contract:' not in text,text[-3000:]
    return text


class NetDiag28(unittest.TestCase):
    def setUp(self):
        BUILD.mkdir(parents=True,exist_ok=True)
        self.tmp=tempfile.TemporaryDirectory(dir=BUILD)
        self.dir=self.tmp.name

    def tearDown(self):
        self.tmp.cleanup()

    def test_api_is_exported_at_2_8(self):
        exports=(SDK/'exports.txt').read_text().split()
        table=(SDK.parent/'components/apps/tui/ls_lap_exports.inc').read_text()
        header=(SDK/'include/lakeshark_app.h').read_text(encoding='utf-8')
        for name in NAMES:
            self.assertIn(name,exports)
            self.assertIn('EXPORT("%s", %s)'%(name,name),table)
        self.assertIn('#define LAP_API_MINOR 8',header)
        self.assertIn('LAP_NETIF_ETH = 2',header)
        self.assertIn('LAP_CAPTURE_ETH = 3',header)

    def test_lan_scan_finds_the_fixture_lan_and_saves_csv(self):
        # udpdrop keeps the SSDP search off the real network.
        text=run('lan_scan','1:udpdrop=1;900:key=w',905,folder=self.dir)
        for ip,vendor in [('192.0.2.1','TP-Link'),('192.0.2.3','Espressif'),('192.0.2.10','Raspberry Pi'),
                          ('192.0.2.23','Philips Hue'),('192.0.2.42','Synology')]:
            self.assertRegex(text,r'%s\s+\d\.\d ms\s+%s'%(ip.replace('.',r'\.'),vendor))
        self.assertIn('6 hosts',text)
        self.assertIn('ARP 254/254  PING 253/253',text)
        self.assertIn('24:0a:c4:4c:53:03  sim',text)          # the mDNS fixture names .3
        files=list(Path(self.dir).glob('sdcard/apps_data/lan_scan/scan_*.csv'))
        self.assertEqual(len(files),1,text[-2000:])
        rows=files[0].read_text().splitlines()
        self.assertEqual(rows[0],'ip,mac,vendor,rtt_ms,name,ssdp_server,found_by,open_ports')
        self.assertEqual(len(rows),7)
        self.assertIn('192.0.2.10,dc:a6:32:4c:53:0a,Raspberry Pi,1.',rows[4])
        self.assertIn('192.0.2.2,02:4c:53:00:00:02,this board,',rows[2])

    def test_lan_scan_wide_has_one_row_per_host(self):
        text=run('lan_scan','1:udpdrop=1',900,grid='160x45',folder=self.dir)
        self.assertRegex(text,r'192\.0\.2\.10\s+dc:a6:32:4c:53:0a\s+Raspberry Pi\s+1\.\d ms')

    def test_lan_scan_without_network(self):
        text=run('lan_scan','1:udpdrop=1',120,folder=self.dir,hw='sd')     # no Wi-Fi fitted
        self.assertIn('No network: join Wi-Fi first',text)
        text=run('net_diag','1:netdown=1;5:key=5',40,folder=self.dir)
        self.assertIn('no network',text)
        self.assertIn('Link      down',text)

    def test_traceroute_dns_and_info(self):
        text=run('net_diag','10:key=2;20:key=s',200,folder=self.dir)
        self.assertRegex(text,r'1\s+192\.0\.2\.1\s+5\.30')
        self.assertRegex(text,r'3\s+203\.0\.113\.9\s')
        self.assertRegex(text,r'4\s+198\.51\.100\.\d+\s')
        self.assertRegex(text,r'@\s+4\s+198\.51\.100\.')           # the chain ends at the destination node
        self.assertNotRegex(text,r'[O@*o.]\s+5\s+\S')               # and stops there
        self.assertIn('TRACEROUTE  4 HOPS',text)
        text=run('net_diag','10:key=3;20:key=s;40:key=e;42:dialogtext=nowhere.invalid;50:key=s',100,folder=self.dir)
        self.assertIn('name not found',text)
        self.assertRegex(text,r'198\.51\.100\.\d+')
        text=run('net_diag','5:key=1;8:key=s;100:key=5',150,folder=self.dir)
        self.assertIn('Ethernet  not available',text)
        self.assertIn('Gateway   192.0.2.1',text)
        self.assertRegex(text,r'192\.0\.2\.1\s+50:c7:bf:4c:53:01')

    def test_ping_counts_and_timeouts(self):
        text=run('net_diag','20:key=e;22:dialogtext=192.0.2.99;24:key=s',200,folder=self.dir)
        self.assertRegex(text,r'sent \d+  received 0  loss 100%')
        self.assertIn('timeout',text)
        text=run('net_diag','20:key=e;22:dialogtext=192.0.2.10;24:key=s',300,folder=self.dir)
        self.assertRegex(text,r'sent (\d+)  received \1  loss 0%')
        self.assertRegex(text,r'seq=\d+\s+192\.0\.2\.10\s+ttl=64\s+1\.\d\d ms')

    def test_wake_on_lan_sends_the_magic_packet(self):
        s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.bind(('127.0.0.1',0)); s.settimeout(10)
        port=s.getsockname()[1]
        try:
            text=run('net_diag','10:key=4;20:key=e;22:dialogtext=dc-a6-32-4c-53-0a;30:key=c;32:dialogtext=127.0.0.1:%d;40:key=s'%port,
                     80,folder=self.dir)
            data,_=s.recvfrom(2048)
        finally:
            s.close()
        self.assertEqual(data,b'\xff'*6+bytes.fromhex('dca6324c530a')*16)
        self.assertIn('dc:a6:32:4c:53:0a -> 127.0.0.1:%d'%port,text)

    def test_board_keys_replace_the_softkey_line(self):
        # The board's finger-sized keys (lap_ui_keys), not a row of [KEY] text.
        text=run('net_diag','',20,folder=self.dir)
        self.assertNotIn('[START]',text)
        self.assertRegex(text,r'\|\W*START\W*\|')
        self.assertRegex(text,r'\|\W*PING\W*\|')              # the < PING > tab strip
        self.assertIn('1/5',text)
        text=run('throughput','',20,folder=self.dir)
        self.assertNotIn('[START]',text)
        self.assertRegex(text,r'\|\W*RATE\W*\|')
        text=run('lan_scan','1:udpdrop=1',20,folder=self.dir)
        self.assertNotIn('[SCAN]',text)
        self.assertRegex(text,r'\|\W*STOP\W*\|')              # scanning: SCAN reads STOP

    def test_art_panels(self):
        # Each app draws its picture inside the board's bezel, not bare text.
        text=run('net_diag','20:key=s',100,folder=self.dir)
        self.assertIn('===PING',text)                              # the tab's bezel, title on the rule
        self.assertRegex(text,r'\| LS \|[-.<>o]+\| 192\.0\.2\.1 \|')   # the path to the host
        self.assertIn('RTT ms',text)                               # the framed braille graph
        text=run('lan_scan','1:udpdrop=1',900,folder=self.dir)
        self.assertIn('ROUTER',text)
        self.assertRegex(text,r'[+`]-- 192\.0\.2\.42')            # hosts branch off the trunk
        self.assertRegex(text,r'SWEEP [.:o@]+')                    # the subnet strip
        text=run('throughput','',20,folder=self.dir)
        self.assertIn('===SPEED',text)
        self.assertRegex(text,r'=+O=+')                            # the gauge's hub on its base

    def test_iperf2_tcp(self):
        srv=socket.socket(); srv.bind(('127.0.0.1',0)); srv.listen(1); srv.settimeout(20)
        port=srv.getsockname()[1]; got=bytearray()
        def serve():
            try:
                c,_=srv.accept()
            except OSError:
                return
            c.settimeout(20)
            while True:
                try:
                    b=c.recv(65536)
                except OSError:
                    break
                if not b: break
                got.extend(b)
            c.close()
        t=threading.Thread(target=serve); t.start()
        try:
            text=run('throughput','10:key=e;12:dialogtext=127.0.0.1:%d;20:key=s'%port,300,folder=self.dir)
        finally:
            t.join(30); srv.close()
        self.assertIn(' DONE ',text)
        self.assertGreater(len(got),24)
        self.assertEqual(bytes(got[:24]),bytes(24))                # iperf2 client header, no flags
        self.assertEqual(bytes(got[24:34]),b'0123456789')
        self.assertRegex(text,r'TCP 10\.0 s  [\d.]+ MB  [\d.]+ Mbit/s')

    def test_iperf2_udp_reads_the_server_report(self):
        s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.bind(('127.0.0.1',0)); s.settimeout(30)
        port=s.getsockname()[1]; seen={'n':0,'flags':None}
        def serve():
            while True:
                try:
                    d,a=s.recvfrom(4096)
                except OSError:
                    return
                seq=struct.unpack_from('>i',d)[0]
                if seen['flags'] is None: seen['flags']=struct.unpack_from('>I',d,12)[0]
                if seq>=0:
                    seen['n']+=1
                    continue
                # iperf2 server report: datagram header, then server_hdr
                rep=d[:12]+struct.pack(">IiiiiiiiII",0x80000000,0,seen["n"]*1470,5,0,2,1,seen["n"],0,250)
                s.sendto(rep,a)
                return
        t=threading.Thread(target=serve); t.start()
        try:
            text=run('throughput','10:key=u;12:key=t;14:key=t;14:key=t;20:key=e;22:dialogtext=127.0.0.1:%d;30:key=s'%port,
                     400,folder=self.dir)
        finally:
            t.join(30); s.close()
        self.assertEqual(seen['flags'],0)
        self.assertGreater(seen['n'],10)
        self.assertIn('SERVER REPORT',text)
        self.assertIn('lost 2/%d'%seen['n'],text)
        self.assertIn('jitter 0.250 ms',text)
        self.assertIn('1 out of order',text)


if __name__=='__main__':
    unittest.main()
