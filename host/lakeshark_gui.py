#!/usr/bin/env python3
"""LakeShark headless control panel.

A tiny local web GUI that drives the headless firmware's serial console
(P25 / ADS-B / FM, volume, frequency, gain, mute). The headless board has no
screen, so this runs on your computer and talks to it over the USB serial port.

Usage:
    python3 lakeshark_gui.py                 # /dev/ttyACM0, http://127.0.0.1:8674
    python3 lakeshark_gui.py -p /dev/ttyUSB0 --http 9000

Only dependency is pyserial (`pip install pyserial`).
"""
import argparse
import json
import re
import threading
import time
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:
    import serial
except ImportError:
    raise SystemExit("pyserial not found - install it with:  pip install pyserial")


class Radio:
    """Owns the serial port: writes commands, tails output, parses `status`."""

    STATUS_RE = re.compile(
        r"mode=(\S+)\s+freq=([\d.]+)\s*MHz\s+vol=(\d+)\s+"
        r"gain=([\d.]+)\s*dB\s+mute=(\d+)"
    )

    def __init__(self, port, baud=115200):
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = baud
        self.ser.timeout = 0.2
        self.ser.dtr = False           # don't reset the board on connect
        self.ser.rts = False
        self.ser.open()
        try:
            self.ser.dtr = False
            self.ser.rts = False
        except Exception:
            pass
        self.lock = threading.Lock()
        self.log = []
        self.state = {}
        self._rx = b""
        threading.Thread(target=self._reader, daemon=True).start()
        threading.Thread(target=self._poller, daemon=True).start()

    def send(self, cmd):
        with self.lock:
            self.ser.write((cmd.strip() + "\r\n").encode())

    def _reader(self):
        while True:
            try:
                data = self.ser.read(512)
            except Exception:
                time.sleep(0.3)
                continue
            if not data:
                continue
            self._rx += data
            while b"\n" in self._rx:
                raw, self._rx = self._rx.split(b"\n", 1)
                line = raw.decode("utf-8", "replace").rstrip("\r")
                if line:
                    self.log.append(line)
                    if len(self.log) > 500:
                        self.log = self.log[-500:]
                    self._parse(line)

    def _parse(self, line):
        m = self.STATUS_RE.search(line)
        if m:
            self.state = {
                "mode": m.group(1),
                "freq": float(m.group(2)),
                "vol": int(m.group(3)),
                "gain": float(m.group(4)),
                "mute": bool(int(m.group(5))),
                "t": time.time(),
            }

    def _poller(self):
        while True:
            try:
                self.send("status")
            except Exception:
                pass
            time.sleep(1.5)


PAGE = """<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>LakeShark</title><style>
:root{--gold:#E0C27A;--bg:#070A09;--panel:#0e1513;--edge:#C9A24A;--txt:#cfe0d8;--dim:#7c8a83}
*{box-sizing:border-box;font-family:'DejaVu Sans Mono',Consolas,monospace}
body{background:var(--bg);color:var(--txt);margin:0;padding:18px;max-width:760px;margin:0 auto}
h1{color:var(--gold);letter-spacing:4px;font-size:26px;margin:4px 0 14px;text-align:center}
.panel{background:var(--panel);border:1px solid var(--edge);border-radius:8px;padding:14px;margin-bottom:14px}
.lcd{display:flex;justify-content:space-between;flex-wrap:wrap;gap:10px}
.lcd .cell{flex:1;min-width:110px;text-align:center}
.lcd .k{color:var(--dim);font-size:11px;text-transform:uppercase;letter-spacing:1px}
.lcd .v{color:var(--gold);font-size:22px;margin-top:3px}
.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin:8px 0}
.row label{color:var(--dim);width:64px;font-size:13px}
button{background:#16201d;color:var(--txt);border:1px solid var(--edge);border-radius:6px;
 padding:9px 12px;cursor:pointer;font-size:14px}
button:hover{background:#1d2a26}button.on{background:var(--edge);color:#0a0a0a;font-weight:bold}
input[type=text]{background:#0a100e;color:var(--gold);border:1px solid var(--edge);
 border-radius:6px;padding:8px;width:120px;font-size:15px}
input[type=range]{flex:1;accent-color:var(--gold)}
#log{background:#050807;border:1px solid #1c2a26;border-radius:6px;height:200px;
 overflow:auto;padding:8px;font-size:12px;color:#9fb3aa;white-space:pre-wrap}
.muted{color:#e39b96 !important}
</style></head><body>
<h1>&#9698; LAKESHARK</h1>
<div class=panel><div class=lcd>
 <div class=cell><div class=k>Mode</div><div class=v id=s_mode>--</div></div>
 <div class=cell><div class=k>Freq MHz</div><div class=v id=s_freq>--</div></div>
 <div class=cell><div class=k>Vol</div><div class=v id=s_vol>--</div></div>
 <div class=cell><div class=k>Gain dB</div><div class=v id=s_gain>--</div></div>
 <div class=cell><div class=k>Mute</div><div class=v id=s_mute>--</div></div>
</div></div>
<div class=panel>
 <div class=row><label>Mode</label>
  <button id=m_p25 onclick="cmd('mode p25')">P25</button>
  <button id=m_adsb onclick="cmd('mode adsb')">ADS-B</button>
  <button id=m_fm onclick="cmd('mode fm')">FM</button></div>
 <div class=row><label>Volume</label>
  <input type=range id=vol min=0 max=100 oninput="vlbl.textContent=this.value"
   onchange="cmd('vol '+this.value)"><span id=vlbl style="width:34px;color:var(--gold)">--</span></div>
 <div class=row><label>Freq</label>
  <input type=text id=freq placeholder="MHz e.g. 154.785">
  <button onclick="cmd('freq '+freq.value)">Tune</button></div>
 <div class=row><label>Gain</label>
  <input type=text id=gain placeholder="dB e.g. 30">
  <button onclick="cmd('gain '+gain.value)">Set</button>
  <button onclick="cmd('gain auto')">Auto</button></div>
 <div class=row><label>&nbsp;</label>
  <button onclick="cmd('mute')">Mute / Unmute</button>
  <button onclick="cmd('status')">Refresh</button></div>
</div>
<div class=panel><div class=k style="color:var(--dim);font-size:11px;margin-bottom:6px">SERIAL LOG</div>
 <div id=log></div></div>
<script>
function cmd(c){fetch('/cmd',{method:'POST',body:c});}
let dragging=false;
const volEl=document.getElementById('vol');
volEl.addEventListener('mousedown',()=>dragging=true);
volEl.addEventListener('mouseup',()=>dragging=false);
async function tick(){
 try{
  const r=await fetch('/state');const d=await r.json();const s=d.state||{};
  if(s.mode!==undefined){
   document.getElementById('s_mode').textContent=s.mode;
   document.getElementById('s_freq').textContent=s.freq.toFixed(4);
   document.getElementById('s_vol').textContent=s.vol;
   document.getElementById('s_gain').textContent=s.gain.toFixed(1);
   const mu=document.getElementById('s_mute');mu.textContent=s.mute?'ON':'off';
   mu.className='v'+(s.mute?' muted':'');
   for(const id of ['m_p25','m_adsb','m_fm'])document.getElementById(id).classList.remove('on');
   const map={'P25':'m_p25','ADS-B':'m_adsb','FM':'m_fm'};
   if(map[s.mode])document.getElementById(map[s.mode]).classList.add('on');
   if(!dragging){volEl.value=s.vol;document.getElementById('vlbl').textContent=s.vol;}
  }
  const lg=document.getElementById('log');const atBottom=lg.scrollTop+lg.clientHeight>=lg.scrollHeight-20;
  lg.textContent=(d.log||[]).join('\\n');
  if(atBottom)lg.scrollTop=lg.scrollHeight;
 }catch(e){}
}
setInterval(tick,800);tick();
</script></body></html>"""


class Handler(BaseHTTPRequestHandler):
    radio = None

    def log_message(self, *a):
        pass

    def _reply(self, code, ctype, body):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/":
            self._reply(200, "text/html; charset=utf-8", PAGE.encode())
        elif self.path == "/state":
            body = json.dumps({
                "state": self.radio.state,
                "log": self.radio.log[-60:],
            }).encode()
            self._reply(200, "application/json", body)
        else:
            self._reply(404, "text/plain", b"not found")

    def do_POST(self):
        if self.path == "/cmd":
            n = int(self.headers.get("Content-Length", 0))
            cmd = self.rfile.read(n).decode("utf-8", "replace").strip()
            if cmd:
                self.radio.send(cmd)
            self._reply(200, "text/plain", b"ok")
        else:
            self._reply(404, "text/plain", b"not found")


def main():
    ap = argparse.ArgumentParser(description="LakeShark headless control panel")
    ap.add_argument("-p", "--port", default="/dev/ttyACM0", help="serial port")
    ap.add_argument("-b", "--baud", type=int, default=115200)
    ap.add_argument("--http", type=int, default=8674, help="local web port")
    ap.add_argument("--no-browser", action="store_true")
    args = ap.parse_args()

    Handler.radio = Radio(args.port, args.baud)
    url = "http://127.0.0.1:%d" % args.http
    httpd = ThreadingHTTPServer(("127.0.0.1", args.http), Handler)
    print("LakeShark control panel on %s  ->  %s" % (args.port, url))
    if not args.no_browser:
        try:
            webbrowser.open(url)
        except Exception:
            pass
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
