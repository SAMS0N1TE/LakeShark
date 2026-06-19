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
            fm = re.search(r"fmmode=(\w+)", line)
            self.state = {
                "mode": m.group(1),
                "freq": float(m.group(2)),
                "vol": int(m.group(3)),
                "gain": float(m.group(4)),
                "mute": bool(int(m.group(5))),
                "fmmode": fm.group(1) if fm else "",
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
<title>LakeShark Control Panel</title><style>
*{box-sizing:border-box}
body{background:#008080;color:#000;font:12px Tahoma,'MS Sans Serif',Geneva,sans-serif;margin:0;padding:14px}
.win{max-width:600px;margin:0 auto;background:#c0c0c0;
 border:2px solid;border-color:#dfdfdf #000 #000 #dfdfdf;box-shadow:1px 1px 0 #808080}
.title{background:linear-gradient(90deg,#000080,#1084d0);color:#fff;font-weight:bold;
 padding:3px 6px;margin:2px;display:flex;justify-content:space-between;align-items:center}
.title .x{background:#c0c0c0;color:#000;border:2px solid;border-color:#dfdfdf #000 #000 #dfdfdf;
 width:18px;height:16px;line-height:11px;text-align:center;font-weight:bold}
.client{padding:8px}
fieldset{border:2px groove #dfdfdf;margin:0 0 8px;padding:7px 9px 9px}
legend{padding:0 4px;font-weight:bold}
.grid{display:grid;grid-template-columns:auto 1fr auto 1fr;gap:5px 8px;align-items:center}
.lbl{color:#000;white-space:nowrap}
.fld{border:2px solid;border-color:#808080 #fff #fff #808080;background:#fff;
 padding:2px 6px;font:12px 'Courier New',monospace;min-height:19px}
.row{display:flex;gap:6px;align-items:center;flex-wrap:wrap;margin:5px 0}
.row .lbl{width:58px}
button{font:12px Tahoma,sans-serif;background:#c0c0c0;color:#000;cursor:pointer;padding:4px 10px;
 min-width:62px;border:2px solid;border-color:#dfdfdf #000 #000 #dfdfdf}
button:active,button.on{border-color:#000 #dfdfdf #dfdfdf #000;background:#b6b6b6;
 box-shadow:inset 1px 1px 0 #808080}
button.on{font-weight:bold}
input[type=text]{border:2px solid;border-color:#808080 #fff #fff #808080;background:#fff;
 padding:3px 4px;font:12px 'Courier New',monospace;width:130px}
input[type=range]{-webkit-appearance:none;appearance:none;height:21px;background:transparent;flex:1;min-width:120px}
input[type=range]::-webkit-slider-runnable-track{height:4px;background:#808080;
 border:1px solid;border-color:#000 #fff #fff #000}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:11px;height:19px;margin-top:-9px;
 background:#c0c0c0;border:2px solid;border-color:#dfdfdf #000 #000 #dfdfdf}
input[type=range]::-moz-range-track{height:4px;background:#808080;border:1px solid #000}
input[type=range]::-moz-range-thumb{width:9px;height:17px;background:#c0c0c0;border:2px solid;
 border-color:#dfdfdf #000 #000 #dfdfdf;border-radius:0}
#log{border:2px solid;border-color:#808080 #fff #fff #808080;background:#fff;height:150px;
 overflow:auto;padding:4px;font:11px 'Courier New',monospace;white-space:pre-wrap;color:#000}
.status .x{background:#c0c0c0;color:#888}
</style></head><body>
<div class=win>
 <div class=title><span>&#9698; LakeShark Control Panel</span><span class=x>x</span></div>
 <div class=client>
  <fieldset><legend>Status</legend><div class=grid>
   <span class=lbl>Mode</span><span class=fld id=s_mode>--</span>
   <span class=lbl>Freq (MHz)</span><span class=fld id=s_freq>--</span>
   <span class=lbl>Volume</span><span class=fld id=s_vol>--</span>
   <span class=lbl>Gain (dB)</span><span class=fld id=s_gain>--</span>
   <span class=lbl>Mute</span><span class=fld id=s_mute>--</span>
   <span class=lbl>FM sub-mode</span><span class=fld id=s_fm>--</span>
  </div></fieldset>
  <fieldset><legend>Mode</legend><div class=row>
   <button id=m_p25 onclick="cmd('mode p25')">P25</button>
   <button id=m_adsb onclick="cmd('mode adsb')">ADS-B</button>
   <button id=m_fm onclick="cmd('mode fm')">FM</button></div></fieldset>
  <fieldset><legend>FM sub-mode</legend><div class=row>
   <button id=f_listen onclick="cmd('fm listen')">LISTEN</button>
   <button id=f_scan onclick="cmd('fm scan')">SCAN</button>
   <button id=f_pocsag onclick="cmd('fm pocsag')">POCSAG</button>
   <button id=f_wfm onclick="cmd('fm wfm')">WFM</button></div></fieldset>
  <fieldset><legend>Controls</legend>
   <div class=row><span class=lbl>Volume</span>
    <input type=range id=vol min=0 max=100 oninput="vlbl.textContent=this.value"
     onchange="cmd('vol '+this.value)"><span class=fld id=vlbl style=width:34px>--</span></div>
   <div class=row><span class=lbl>Freq</span>
    <input type=text id=freq placeholder="MHz e.g. 154.785">
    <button onclick="cmd('freq '+freq.value)">Tune</button></div>
   <div class=row><span class=lbl>Gain</span>
    <input type=text id=gain placeholder="dB e.g. 30">
    <button onclick="cmd('gain '+gain.value)">Set</button>
    <button onclick="cmd('gain auto')">Auto</button></div>
   <div class=row><span class=lbl>&nbsp;</span>
    <button onclick="cmd('mute')">Mute</button>
    <button onclick="cmd('status')">Refresh</button></div>
  </fieldset>
  <fieldset><legend>Serial log</legend><div id=log></div></fieldset>
 </div>
</div>
<script>
function cmd(c){fetch('/cmd',{method:'POST',body:c});}
let dragging=false;
const volEl=document.getElementById('vol');
volEl.addEventListener('mousedown',()=>dragging=true);
volEl.addEventListener('mouseup',()=>dragging=false);
function setOn(ids,active){for(const id of ids){var e=document.getElementById(id);
 if(e)e.classList.toggle('on',id===active);}}
async function tick(){
 try{
  const r=await fetch('/state');const d=await r.json();const s=d.state||{};
  if(s.mode!==undefined){
   document.getElementById('s_mode').textContent=s.mode;
   document.getElementById('s_freq').textContent=s.freq.toFixed(4);
   document.getElementById('s_vol').textContent=s.vol;
   document.getElementById('s_gain').textContent=s.gain.toFixed(1);
   document.getElementById('s_mute').textContent=s.mute?'ON':'off';
   document.getElementById('s_fm').textContent=(s.fmmode||'--').toUpperCase();
   setOn(['m_p25','m_adsb','m_fm'],{'P25':'m_p25','ADS-B':'m_adsb','FM':'m_fm'}[s.mode]);
   setOn(['f_listen','f_scan','f_pocsag','f_wfm'],
     (s.mode==='FM')?({'listen':'f_listen','scan':'f_scan','pocsag':'f_pocsag','wfm':'f_wfm'}[s.fmmode]):null);
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
