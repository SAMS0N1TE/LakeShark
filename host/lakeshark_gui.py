#!/usr/bin/env python3
"""LakeShark headless control panel.

A tiny local web GUI that drives the headless firmware's serial console
(P25 / ADS-B / FM incl. POCSAG, volume, frequency, gain, mute). The headless
board has no screen, so this runs on your computer and talks to it over the USB
serial port.

Usage:
    python3 lakeshark_gui.py                 # /dev/ttyACM0, http://127.0.0.1:8674
    python3 lakeshark_gui.py -p /dev/ttyUSB0 --http 9000

Only dependency is pyserial (`pip install pyserial`).
"""
import argparse
import json
import os
import re
import threading
import time
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:
    import serial
except ImportError:
    raise SystemExit("pyserial not found - install it with:  pip install pyserial")

# Windows 95 UI font (W95FA by Alina Sava, SIL OFL), served locally so the GUI
# needs no internet. Falls back to a sans stack if the file is missing.
_FONT_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "W95FA.woff")
try:
    with open(_FONT_FILE, "rb") as _f:
        FONT_BYTES = _f.read()
except Exception:
    FONT_BYTES = b""


class Radio:
    """Owns the serial port: writes commands, tails output, parses `status`."""

    STATUS_RE = re.compile(
        r"mode=(\S+)\s+freq=([\d.]+)\s*MHz\s+vol=(\d+)\s+"
        r"gain=([\d.]+)\s*dB\s+mute=(\d+)"
    )
    POC_RE = re.compile(r"page RIC=(\d+)\s+F=(\d+)\s+([TAN])\s+'(.*)'")

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
        self.aircraft = {}     # icao -> latest fields
        self.pages = []        # POCSAG pages, newest last
        self._rx = b""
        threading.Thread(target=self._reader, daemon=True).start()
        threading.Thread(target=self._poller, daemon=True).start()
        self.send("feed on")   # ADS-B JSON feed to the console (CartoTUI + this GUI)

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
            feed = re.search(r"feed=(\w+)", line)
            self.state = {
                "mode": m.group(1),
                "freq": float(m.group(2)),
                "vol": int(m.group(3)),
                "gain": float(m.group(4)),
                "mute": bool(int(m.group(5))),
                "fmmode": fm.group(1) if fm else "",
                "feed": feed.group(1) if feed else "",
                "t": time.time(),
            }
            return
        if '"app":"ADS-B"' in line and '"icao"' in line:
            self._contact(line)
            return
        p = self.POC_RE.search(line)
        if p:
            self.pages.append({
                "ts": time.time(), "ric": int(p.group(1)),
                "func": int(p.group(2)), "type": p.group(3), "text": p.group(4),
            })
            if len(self.pages) > 200:
                self.pages = self.pages[-200:]

    def _contact(self, line):
        try:
            o = json.loads(line[line.index("{"):line.rindex("}") + 1])
        except Exception:
            return
        icao = o.get("icao")
        if not icao:
            return
        if o.get("k") == "contact_lost":
            self.aircraft.pop(icao, None)
            return
        a = self.aircraft.get(icao, {"icao": icao})
        for k in ("cs", "alt", "vel", "hdg", "vs", "lat", "lon", "pos", "shaky"):
            if k in o:
                a[k] = o[k]
        a["t"] = time.time()
        self.aircraft[icao] = a

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
@font-face{font-family:'W95FA';src:url('/W95FA.woff') format('woff');font-display:swap}
*{box-sizing:border-box;font-family:'W95FA','MS Sans Serif',Tahoma,Geneva,sans-serif}
body{background:#b6b6b6;color:#141414;font-size:16px;line-height:1.5;margin:0;padding:24px}
.wrap{max-width:780px;margin:0 auto}
h1{font-weight:700;font-size:22px;letter-spacing:2px;text-transform:uppercase;
 margin:0 0 20px;padding-bottom:10px;border-bottom:1px solid #6f6f6f;color:#1a1a1a}
section{margin-bottom:18px}
.hd{font-weight:700;font-size:14px;letter-spacing:1px;text-transform:uppercase;
 color:#555;margin:0 0 7px}
.box{border:1px solid #888;background:#c5c5c5;padding:14px}
.grid{display:grid;grid-template-columns:auto 1fr auto 1fr;gap:10px 16px;align-items:center}
.lbl{color:#333;font-size:15px;white-space:nowrap}
.val{font-size:16px;background:#ededed;border:1px solid #8a8a8a;
 padding:4px 10px;color:#000;min-height:28px}
.row{display:flex;gap:9px;align-items:center;flex-wrap:wrap;margin:8px 0}
.row .lbl{width:72px}
button{font-size:15px;background:#d2d2d2;color:#141414;border:1px solid #707070;
 padding:8px 16px;cursor:pointer;min-width:84px}
button:hover{background:#c6c6c6}
button:active{background:#9d9d9d}
button.on{background:#2b2b2b;color:#f0f0f0;border-color:#1a1a1a}
input[type=text]{background:#ededed;border:1px solid #8a8a8a;padding:7px 8px;
 font-size:16px;width:180px;color:#000}
input[type=range]{flex:1;min-width:160px;height:24px;accent-color:#3a3a3a}
#log{background:#ededed;border:1px solid #8a8a8a;height:210px;overflow:auto;padding:8px;
 font-size:14px;white-space:pre-wrap;color:#141414}
.scroll{max-height:240px;overflow:auto;border:1px solid #888;background:#ededed}
table{border-collapse:collapse;width:100%;font-size:14px}
th,td{border:1px solid #aaa;padding:5px 10px;text-align:left;white-space:nowrap}
th{background:#bcbcbc;font-weight:700;color:#222;position:sticky;top:0}
td{background:#ededed;color:#000}
.empty{color:#666;padding:11px;font-style:italic;font-size:15px}
</style></head><body>
<div class=wrap>
<h1>LakeShark &mdash; Headless Control</h1>
<section><div class=hd>Status</div><div class="box grid">
 <span class=lbl>Mode</span><span class=val id=s_mode>--</span>
 <span class=lbl>Freq MHz</span><span class=val id=s_freq>--</span>
 <span class=lbl>Volume</span><span class=val id=s_vol>--</span>
 <span class=lbl>Gain dB</span><span class=val id=s_gain>--</span>
 <span class=lbl>Mute</span><span class=val id=s_mute>--</span>
 <span class=lbl>FM mode</span><span class=val id=s_fm>--</span>
 <span class=lbl>ADS-B feed</span><span class=val id=s_feed>--</span>
</div></section>
<section><div class=hd>Mode</div><div class="box row">
 <button id=m_p25 onclick="cmd('mode p25')">P25</button>
 <button id=m_adsb onclick="cmd('mode adsb')">ADS-B</button>
 <button id=m_fm onclick="cmd('mode fm')">FM</button></div></section>
<section><div class=hd>FM sub-mode</div><div class="box row">
 <button id=f_listen onclick="cmd('fm listen')">LISTEN</button>
 <button id=f_scan onclick="cmd('fm scan')">SCAN</button>
 <button id=f_pocsag onclick="cmd('fm pocsag')">POCSAG</button>
 <button id=f_wfm onclick="cmd('fm wfm')">WFM</button></div></section>
<section><div class=hd>Controls</div><div class=box>
 <div class=row><span class=lbl>Volume</span>
  <input type=range id=vol min=0 max=100 oninput="vlbl.textContent=this.value"
   onchange="cmd('vol '+this.value)"><span class=val id=vlbl style=min-width:34px>--</span></div>
 <div class=row><span class=lbl>Freq</span>
  <input type=text id=freq placeholder="MHz e.g. 154.785">
  <button onclick="cmd('freq '+freq.value)">Tune</button></div>
 <div class=row><span class=lbl>Gain</span>
  <input type=text id=gain placeholder="dB e.g. 30">
  <button onclick="cmd('gain '+gain.value)">Set</button>
  <button onclick="cmd('gain auto')">Auto</button></div>
 <div class=row><span class=lbl>Feed</span>
  <button onclick="cmd('feed on')">On</button>
  <button onclick="cmd('feed off')">Off</button>
  <span class=lbl style="width:auto;color:#555">ADS-B JSON to console (CartoTUI)</span></div>
 <div class=row><span class=lbl></span>
  <button onclick="cmd('mute')">Mute</button>
  <button onclick="cmd('status')">Refresh</button></div>
</div></section>
<section><div class=hd>ADS-B aircraft <span id=ac_n style=color:#555></span></div>
 <div class=scroll><table id=ac_tbl><thead><tr><th>ICAO</th><th>Flight</th><th>Alt ft</th>
  <th>Spd</th><th>Hdg</th><th>Lat</th><th>Lon</th></tr></thead><tbody id=ac_body></tbody></table>
  <div class=empty id=ac_empty>No aircraft (switch to ADS-B mode; needs traffic overhead).</div></div></section>
<section><div class=hd>POCSAG pages <span id=pg_n style=color:#555></span></div>
 <div class=scroll><table id=pg_tbl><thead><tr><th>Time</th><th>RIC</th><th>Fn</th>
  <th>Type</th><th>Message</th></tr></thead><tbody id=pg_body></tbody></table>
  <div class=empty id=pg_empty>No pages (switch to FM &rarr; POCSAG; needs pager traffic).</div></div></section>
<section><div class=hd>Serial log</div><div id=log></div></section>
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
   document.getElementById('s_feed').textContent=(s.feed||'--').toUpperCase();
   setOn(['m_p25','m_adsb','m_fm'],{'P25':'m_p25','ADS-B':'m_adsb','FM':'m_fm'}[s.mode]);
   setOn(['f_listen','f_scan','f_pocsag','f_wfm'],
     (s.mode==='FM')?({'listen':'f_listen','scan':'f_scan','pocsag':'f_pocsag','wfm':'f_wfm'}[s.fmmode]):null);
   if(!dragging){volEl.value=s.vol;document.getElementById('vlbl').textContent=s.vol;}
  }
  renderAircraft(d.aircraft||[]);
  renderPages(d.pages||[]);
  const lg=document.getElementById('log');const atBottom=lg.scrollTop+lg.clientHeight>=lg.scrollHeight-20;
  lg.textContent=(d.log||[]).join('\\n');
  if(atBottom)lg.scrollTop=lg.scrollHeight;
 }catch(e){}
}
function esc(s){return String(s==null?'':s).replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));}
function renderAircraft(ac){
 document.getElementById('ac_n').textContent=ac.length?('('+ac.length+')'):'';
 document.getElementById('ac_empty').style.display=ac.length?'none':'block';
 document.getElementById('ac_tbl').style.display=ac.length?'table':'none';
 document.getElementById('ac_body').innerHTML=ac.map(a=>{
  const pos=a.pos?(a.lat.toFixed(4)+'</td><td>'+a.lon.toFixed(4)):'--</td><td>--';
  return '<tr><td>'+esc(a.icao)+'</td><td>'+esc(a.cs||'')+'</td><td>'+(a.alt||0)+
   '</td><td>'+(a.vel||0)+'</td><td>'+(a.hdg||0)+'</td><td>'+pos+'</td></tr>';}).join('');
}
function renderPages(pg){
 document.getElementById('pg_n').textContent=pg.length?('('+pg.length+')'):'';
 document.getElementById('pg_empty').style.display=pg.length?'none':'block';
 document.getElementById('pg_tbl').style.display=pg.length?'table':'none';
 document.getElementById('pg_body').innerHTML=pg.slice().reverse().map(p=>{
  const tm=new Date(p.ts*1000).toLocaleTimeString();
  return '<tr><td>'+tm+'</td><td>'+p.ric+'</td><td>'+p.func+'</td><td>'+p.type+
   '</td><td>'+esc(p.text)+'</td></tr>';}).join('');
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
        elif self.path == "/W95FA.woff":
            self._reply(200, "font/woff", FONT_BYTES)
        elif self.path == "/state":
            now = time.time()
            ac = [a for a in self.radio.aircraft.values() if now - a["t"] < 90]
            ac.sort(key=lambda a: a["icao"])
            body = json.dumps({
                "state": self.radio.state,
                "aircraft": ac,
                "pages": self.radio.pages[-40:],
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
