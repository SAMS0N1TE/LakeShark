#!/usr/bin/env python3
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
    import serial.tools.list_ports
except ImportError:
    raise SystemExit("pyserial not found - install it with:  pip install pyserial")

_FONT_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "W95FA.woff")
try:
    with open(_FONT_FILE, "rb") as _f:
        FONT_BYTES = _f.read()
except Exception:
    FONT_BYTES = b""


class Radio:
    STATUS_RE = re.compile(
        r"(\S+)\s+freq=([\d.]+)\s*MHz\s+vol=(\d+)\s+"
        r"gain=([\d.]+)\s*dB\s+mute=(\d+)"
    )
    POC_RE = re.compile(r"page RIC=(\d+)\s+F=(\d+)\s+([TAN])\s+'(.*)'")

    def __init__(self, port, baud=115200):
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = baud
        self.ser.timeout = 0.2
        self.ser.dtr = False
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
        self.aircraft = {}
        self.pages = []
        self._rx = b""
        threading.Thread(target=self._reader, daemon=True).start()
        threading.Thread(target=self._poller, daemon=True).start()
        self.send("feed on")

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


PAGE = r"""<!doctype html>
<html>
<head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>LakeShark</title>
    <style>
        /* ====================================================================
           LakeShark — Headless Control · DITHERED / E-INK restyle
           --------------------------------------------------------------------
           Same markup, IDs, endpoints and commands as the original — only the
           skin changes. Aesthetic borrowed from the mesh-core e-ink console:
             · fine particle grain on the page + baked speckle tiles per panel
             · Bayer 8x8 ordered dither for the volume meter / hairlines
             · crisp flat (no-bevel) borders, uppercase caps, tabular numerals
           Colours kept from the original LakeShark palette.
        ==================================================================== */
        @font-face {
            font-family: 'W95FA';
            src: url('W95FA.woff') format('woff');
            font-display: swap;
        }
        :root {
            --paper:   #b8b8b8;   /* page                          */
            --panel:   #3a3a3a;   /* dark boxes                    */
            --panel-2: #3f3f3f;   /* value / input surfaces        */
            --panel-3: #2b2b2b;   /* log / active-button surface   */
            --ink:     #141414;   /* crisp hard border ("cut edge")*/
            --hair:    #6f6f6f;   /* light rules                   */
            --txt:     #ffffff;
            --txt-2:   #e6e6e6;
            --accent:  #8f4b4b;   /* dusty red                     */
            --teal:    #165050;   /* hover                         */
            --teal-2:  #2ca0a0;   /* active                        */
            --grain:   none;      /* dark-speck tile  (light surfaces) */
            --grain-t: none;      /* light-speck tile (dark surfaces)  */
            --dith:    none;      /* ordered Bayer tile (dithered gradient bars) */
        }
        * {
            box-sizing: border-box;
            font-family: 'W95FA', 'MS Sans Serif', Tahoma, Geneva, sans-serif;
        }
        body {
            background-color: var(--paper);
            /* e-ink particle grain: two offset fine dot tiles give even flat
               areas a subtle print texture. Cheap, static, no JS. */
            background-image:
                radial-gradient(rgba(0,0,0,.14) .5px, transparent .6px),
                radial-gradient(rgba(0,0,0,.09) .5px, transparent .6px);
            background-size: 3px 3px, 3px 3px;
            background-position: 0 0, 1.5px 1.5px;
            color: var(--ink);
            font-size: 13px;
            line-height: 1.6;
            margin: 0;
            padding: 12px 9px;
            -webkit-font-smoothing: antialiased;
        }
        .wrap {
            max-width: 780px;
            margin: 0 auto;
        }
        h1 {
            font-weight: 400;
            font-size: 22px;
            letter-spacing: 10px;
            text-transform: uppercase;
            margin: 0 0 4px;
            padding-bottom: 10px;
            color: #1a1a1a;
            text-align: center;
        }
        /* dithered title rule (drawn by JS) replaces the flat border */
        #rule { display:block; width:100%; height:6px; margin:0 0 20px; image-rendering: pixelated; }

        section { margin-bottom: 18px; }

        /* section header band — accent-red gradient with an ordered dither
           laid over it (structured Bayer texture, not random noise) */
        .hd {
            font-weight: 400;
            background-color: var(--accent);
            background-image: var(--dith), linear-gradient(180deg,#a75c5c,#844747 55%,#683737);
            background-repeat: repeat, no-repeat;
            background-size: auto, 100% 100%;
            border: 2px solid var(--ink);
            border-bottom: 0;
            font-size: 10px;
            letter-spacing: 3px;
            text-transform: uppercase;
            color: #ffffff;
            padding: 5px 10px;
            text-align: center;
        }
        .hd .n { color: #f0d2d2; letter-spacing: 1px; }

        /* panel — its own sheet of e-ink paper: dark surface + light-speck grain,
           hard border, no bevel */
        .box {
            border: 2px solid var(--ink);
            background-color: var(--panel);
            background-image: var(--grain-t);
            background-repeat: repeat;
            background-origin: border-box;
            padding: 14px;
        }
        .grid {
            display: grid;
            grid-template-columns: auto 1fr auto 1fr;
            gap: 10px 16px;
            align-items: center;
        }
        .lbl {
            color: #ffffff;
            font-size: 11px;
            letter-spacing: .12em;
            text-transform: uppercase;
            white-space: nowrap;
        }
        .val {
            font-size: 16px;
            background-color: var(--panel-2);
            background-image: var(--grain-t);
            background-repeat: repeat;
            border: 1px solid var(--ink);
            padding: 4px 10px;
            color: var(--txt-2);
            min-height: 28px;
            font-variant-numeric: tabular-nums;
            letter-spacing: .01em;
        }
        /* big status readouts get a dark-speck grain clipped to the glyphs — a
           dithered "LCD ink" look on the white numerals */
        .num {
            background-image: var(--grain), linear-gradient(#ffffff,#ffffff);
            background-repeat: repeat;
            -webkit-background-clip: text; background-clip: text;
            -webkit-text-fill-color: transparent; color: transparent;
        }
        .row {
            display: flex;
            gap: 4px;
            align-items: center;
            flex-wrap: wrap;
            margin: 3px 0;
        }
        .row .lbl { width: 72px; }

        button {
            font-size: 12px;
            letter-spacing: .06em;
            text-transform: uppercase;
            background-color: var(--accent);
            background-image: var(--grain-t);
            background-repeat: repeat;
            background-origin: border-box;
            color: #ffffff;
            border: 2px solid var(--ink);
            padding: 7px 16px;
            cursor: pointer;
            min-width: 84px;
            transition: background-color .12s;
        }
        button:hover  { background-color: var(--teal); }
        button:active { background-color: var(--teal-2); }
        button.on {
            background-color: var(--panel-3);
            background-image: var(--grain-t);
            color: #f0f0f0;
            border-color: var(--ink);
            box-shadow: inset 0 0 0 1px var(--teal-2);
        }
        input[type="text"] {
            background-color: var(--panel-2);
            background-image: var(--grain-t);
            background-repeat: repeat;
            border: 2px solid var(--ink);
            padding: 6px 8px;
            font-size: 16px;
            width: 180px;
            color: var(--txt-2);
        }
        input[type="text"]::placeholder { color: #9a9a9a; }

        /* volume slider — the track fills with a dithered accent colour up to the
           thumb (the "level" lives in the slider itself). --pct is set by JS for
           WebKit; Firefox fills via ::-moz-range-progress natively. */
        input[type="range"] {
            -webkit-appearance: none; appearance: none;
            flex: 1; min-width: 160px; height: 34px; background: transparent;
        }
        input[type="range"]:focus { outline: none; }
        input[type="range"]::-webkit-slider-runnable-track {
            width: 100%; height: 24px; cursor: pointer;
            border: 2px solid var(--ink);
            background-image:
                var(--dith),
                linear-gradient(90deg, var(--accent) 0 var(--pct,0%), #2b2b2b var(--pct,0%) 100%);
            background-repeat: repeat, no-repeat;
        }
        input[type="range"]::-webkit-slider-thumb {
            height: 32px; width: 14px;
            border: 2px solid var(--ink);
            background: var(--teal-2); cursor: pointer;
            -webkit-appearance: none; appearance: none; margin-top: -6px;
        }
        input[type="range"]::-moz-range-track {
            width: 100%; height: 24px; cursor: pointer;
            border: 2px solid var(--ink);
            background-image: var(--dith), linear-gradient(#2b2b2b,#2b2b2b);
            background-repeat: repeat, no-repeat;
        }
        input[type="range"]::-moz-range-progress {
            height: 24px;
            background-image: var(--dith), linear-gradient(var(--accent),var(--accent));
            background-repeat: repeat, no-repeat;
        }
        input[type="range"]::-moz-range-thumb {
            height: 32px; width: 14px; border: 2px solid var(--ink);
            background: var(--teal-2); cursor: pointer; box-sizing: border-box;
        }

        #log {
            background-color: var(--panel-3);
            background-image: var(--grain-t);
            background-repeat: repeat;
            border: 2px solid var(--ink);
            height: 210px;
            overflow: auto;
            padding: 8px;
            font-size: 14px;
            white-space: pre-wrap;
            color: #ffffff;
        }
        .scroll {
            max-height: 240px;
            overflow: auto;
            border: 2px solid var(--ink);
            background-color: var(--panel-2);
            background-image: var(--grain-t);
            background-repeat: repeat;
        }
        table { border-collapse: collapse; width: 100%; font-size: 14px; }
        th, td {
            border: 1px solid #565656;
            padding: 5px 10px;
            text-align: left;
            white-space: nowrap;
        }
        th {
            background-color: var(--accent);
            background-image: var(--dith), linear-gradient(180deg,#a75c5c,#844747 55%,#683737);
            background-repeat: repeat, no-repeat;
            background-size: auto, 100% 100%;
            font-weight: 400;
            letter-spacing: .1em;
            text-transform: uppercase;
            font-size: 11px;
            color: #ffffff;
            position: sticky; top: 0;
        }
        td {
            background-color: var(--panel-2);
            color: #ffffff;
            font-variant-numeric: tabular-nums;
        }
        .empty {
            color: #ffffff;
            padding: 11px;
            font-style: italic;
            font-size: 15px;
        }
        .hint { color:#c8b0b0; letter-spacing:.04em; }

        /* ASCII shark — its own sheet of grained paper */
        .sh-wrap {
            border: 2px solid var(--ink);
            background-color: var(--panel);
            background-image: var(--grain-t);
            background-repeat: repeat;
            background-origin: border-box;
            padding: 12px 0;
            overflow: hidden;
        }
        .sh {
            font-family: monospace;
            color: #cfcfcf;
            background: transparent;
            font-size: 11px;
            white-space: pre;
            line-height: 1;
            display: block;
            width: fit-content; max-width: 100%;
            margin: 0 auto;
            overflow-x: auto;
            scrollbar-width: none; -ms-overflow-style: none;
        }
        .sh::-webkit-scrollbar { display: none; }
    </style>
</head>
<body>
    <div class="wrap">
        <h1>LakeShark &mdash; Headless Control</h1>
        <canvas id="rule" height="6"></canvas>

        <section>
            <div class="hd">Status</div>
            <div class="box grid">
                <span class="lbl">Mode</span><span class="val" id="s_mode">--</span>
                <span class="lbl">Freq MHz</span><span class="val num" id="s_freq">--</span>
                <span class="lbl">Volume</span><span class="val num" id="s_vol">--</span>
                <span class="lbl">Gain dB</span><span class="val num" id="s_gain">--</span>
                <span class="lbl">Mute</span><span class="val" id="s_mute">--</span>
                <span class="lbl">FM mode</span><span class="val" id="s_fm">--</span>
                <span class="lbl">ADS-B feed</span><span class="val" id="s_feed">--</span>
            </div>
        </section>

        <section>
            <div class="hd">Mode</div>
            <div class="box row">
                <button id="m_p25" onclick="cmd('mode p25')">P25</button>
                <button id="m_adsb" onclick="cmd('mode adsb')">ADS-B</button>
                <button id="m_fm" onclick="cmd('mode fm')">FM</button>
            </div>
        </section>

        <section>
            <div class="hd">FM sub-mode</div>
            <div class="box row">
                <button id="f_listen" onclick="cmd('fm listen')">LISTEN</button>
                <button id="f_scan" onclick="cmd('fm scan')">SCAN</button>
                <button id="f_pocsag" onclick="cmd('fm pocsag')">POCSAG</button>
                <button id="f_wfm" onclick="cmd('fm wfm')">WFM</button>
            </div>
        </section>

        <section>
            <div class="hd">Controls</div>
            <div class="box">
                <div class="row">
                    <span class="lbl">Volume</span>
                    <input type="range" id="vol" min="0" max="100" oninput="paintVol(this.value)" onchange="cmd('vol '+this.value)">
                    <span class="val num" id="vlbl" style="min-width:34px">--</span>
                </div>
                <div class="row">
                    <span class="lbl">Freq</span>
                    <input type="text" id="freq" placeholder="MHz e.g. 154.785">
                    <button onclick="cmd('freq '+freq.value)">Tune</button>
                </div>
                <div class="row">
                    <span class="lbl">Gain</span>
                    <input type="text" id="gain" placeholder="dB e.g. 30">
                    <button onclick="cmd('gain '+gain.value)">Set</button>
                    <button onclick="cmd('gain auto')">Auto</button>
                </div>
                <div class="row">
                    <span class="lbl">Feed</span>
                    <button onclick="cmd('feed on')">On</button>
                    <button onclick="cmd('feed off')">Off</button>
                    <span class="lbl hint" style="width:auto;text-transform:none;letter-spacing:0">ADS-B JSON to console (CartoTUI)</span>
                </div>
                <div class="row">
                    <span class="lbl"></span>
                    <button onclick="cmd('mute')">Mute</button>
                    <button onclick="cmd('status')">Refresh</button>
                </div>
            </div>
        </section>

        <section>
            <div class="hd">ADS-B aircraft <span class="n" id="ac_n"></span></div>
            <div class="scroll">
                <table id="ac_tbl">
                    <thead>
                        <tr>
                            <th>ICAO</th>
                            <th>Flight</th>
                            <th>Alt ft</th>
                            <th>Spd</th>
                            <th>Hdg</th>
                            <th>Lat</th>
                            <th>Lon</th>
                        </tr>
                    </thead>
                    <tbody id="ac_body"></tbody>
                </table>
                <div class="empty" id="ac_empty">No aircraft (switch to ADS-B mode; needs traffic overhead).</div>
            </div>
        </section>

        <section>
            <div class="hd">POCSAG pages <span class="n" id="pg_n"></span></div>
            <div class="scroll">
                <table id="pg_tbl">
                    <thead>
                        <tr>
                            <th>Time</th>
                            <th>RIC</th>
                            <th>Fn</th>
                            <th>Type</th>
                            <th>Message</th>
                        </tr>
                    </thead>
                    <tbody id="pg_body"></tbody>
                </table>
                <div class="empty" id="pg_empty">No pages (switch to FM &rarr; POCSAG; needs pager traffic).</div>
            </div>
        </section>

        <section>
            <div class="hd">Serial log</div>
            <div id="log"></div>
        </section>

        <section>
            <div class="sh-wrap">
            <div class="sh">




                                        XURX
                                      XTY SY                                ZWV
                                     UW   YT                             YUUXSX
                               ZYXWVTZ     QZ                          VUY WSX
                   ZVVVVVVVVVWXYZ          XPTVWZ                    TV   WV
             YVVVVVY                            YWVVVVVVXWVV       UU    UW
         WVVWZ                                             UVVVVVVTZ    VX
        XTZ                                                            XV
          YVVVQPPTW                                   ZXVVVYQUVVWUT    XV
              XVVVY           Z  YWWWX         YWY  UPV    YV     ZSZ  XU
                   WVVVVVVVVUUQSZ    RUVVVVVVVWZ ZVUVZ              UV  WX
                            TX VQV   WV                              ZTXWV
                             YUTX VUZ VV                               ZVTX
                                    XVVQS




                                                                                    </div>
            </div>
        </section>
    </div>

    <script>
        /* ===================================================================
           DITHER + GRAIN ENGINE  (ordered Bayer 8x8 — from the mesh console)
           =================================================================== */
        const BAYER = (() => { const m = [0,32,8,40,2,34,10,42,48,16,56,24,50,18,58,26,12,44,4,36,14,46,6,38,
            60,28,52,20,62,30,54,22,3,35,11,43,1,33,9,41,51,19,59,27,49,17,57,25,15,47,7,39,13,45,5,37,
            63,31,55,23,61,29,53,21]; return m.map(v => (v + 0.5) / 64); })();
        const _patCache = new Map();
        function dither(ctx, level, fg) {
            level = Math.max(0, Math.min(1, level));
            const key = level.toFixed(3) + '|' + fg;
            if (_patCache.has(key)) return _patCache.get(key);
            const n = 8, c = document.createElement('canvas'); c.width = c.height = n;
            const g = c.getContext('2d'); g.imageSmoothingEnabled = false;
            g.fillStyle = fg;
            for (let y = 0; y < n; y++) for (let x = 0; x < n; x++) {
                if (BAYER[y * 8 + x] < level) g.fillRect(x, y, 1, 1);
            }
            const pat = ctx.createPattern(c, 'repeat'); _patCache.set(key, pat); return pat;
        }

        /* Bake the two speckle "paper" tiles into CSS vars (data URLs) — static
           background-images, no per-frame cost. --grain = dark specks (light
           surfaces); --grain-t = light specks (dark surfaces / numerals). */
        function bakeGrain() {
            const root = document.documentElement.style;
            const mk = (dark) => {
                const N = 64, c = document.createElement('canvas'); c.width = c.height = N;
                const g = c.getContext('2d'); g.imageSmoothingEnabled = false;
                let s = dark ? 1337 : 24681;
                const rnd = () => { s = (s * 1103515245 + 12345) & 0x7fffffff; return s / 0x7fffffff; };
                const col = dark ? '0,0,0' : '255,255,255';
                for (let y = 0; y < N; y++) for (let x = 0; x < N; x++) {
                    const r = rnd();
                    if (r < 0.35) {
                        const a = Math.min(0.30, 0.05 + r * 0.11);
                        g.fillStyle = 'rgba(' + col + ',' + a.toFixed(3) + ')';
                        g.fillRect(x, y, 1, 1);
                    }
                }
                return 'url(' + c.toDataURL() + ')';
            };
            root.setProperty('--grain', mk(true));
            root.setProperty('--grain-t', mk(false));
        }

        /* Bake an ordered Bayer 8x8 dither tile (--dith) — laid over the accent
           gradient on the title bars / slider fill so the gradient reads as a
           genuine dithered band rather than a smooth wash. Structured, not noise. */
        function bakeDither() {
            const n = 8, c = document.createElement('canvas'); c.width = c.height = n;
            const g = c.getContext('2d'); g.imageSmoothingEnabled = false;
            for (let y = 0; y < n; y++) for (let x = 0; x < n; x++) {
                const b = BAYER[y * 8 + x];
                if (b < 0.5)       { g.fillStyle = 'rgba(0,0,0,0.32)';       g.fillRect(x, y, 1, 1); }
                else if (b > 0.88) { g.fillStyle = 'rgba(255,255,255,0.14)'; g.fillRect(x, y, 1, 1); }
            }
            document.documentElement.style.setProperty('--dith', 'url(' + c.toDataURL() + ')');
        }

        /* Give every grained element a random tile phase so the speckle never
           lines up across a border — each panel reads as its own cut sheet.
           (Title bars are excluded: their gradient + ordered dither must stay put.) */
        function scatterGrain() {
            const SEL = '.box,.val,button,input[type="text"],#log,.scroll,.sh-wrap';
            document.querySelectorAll(SEL).forEach(el => {
                if (el.dataset.grained) return;
                el.dataset.grained = '1';
                el.style.backgroundPosition = ((Math.random() * 64) | 0) + 'px ' + ((Math.random() * 64) | 0) + 'px';
            });
        }

        /* dithered title hairline under the H1 */
        function drawRule() {
            const c = document.getElementById('rule');
            const w = c.clientWidth || 760; c.width = w; c.height = 6;
            const ctx = c.getContext('2d'); ctx.imageSmoothingEnabled = false;
            ctx.clearRect(0, 0, w, 6);
            ctx.fillStyle = dither(ctx, 0.85, '#6f6f6f'); ctx.fillRect(0, 0, w, 2);
            ctx.fillStyle = dither(ctx, 0.55, '#8f4b4b'); ctx.fillRect(0, 2, w, 2);
            ctx.fillStyle = dither(ctx, 0.28, '#6f6f6f'); ctx.fillRect(0, 4, w, 2);
        }

        /* volume slider fill — set --pct so the WebKit track fills to the thumb
           and keep the read-out label in sync (Firefox fills natively) */
        function paintVol(v) {
            const n = Math.max(0, Math.min(100, +v || 0));
            document.getElementById('vlbl').textContent = v;
            volEl.style.setProperty('--pct', n + '%');
        }

        /* ===================================================================
           APP LOGIC  (identical endpoints / IDs / commands as the original)
           =================================================================== */
        function cmd(c) {
            fetch('/cmd', { method: 'POST', body: c }).catch(() => {});
        }

        let dragging = false;
        const volEl = document.getElementById('vol');
        volEl.addEventListener('mousedown', () => dragging = true);
        volEl.addEventListener('mouseup', () => dragging = false);

        function setOn(ids, active) {
            for (const id of ids) {
                var e = document.getElementById(id);
                if (e) e.classList.toggle('on', id === active);
            }
        }

        async function tick() {
            try {
                const r = await fetch('/state');
                const d = await r.json();
                applyState(d);
            } catch (e) {
                // offline preview: seed demo data once so the skin is visible with
                // no live device. Never fires when the Python server is running
                // (a reachable /state returns 200, so fetch does not throw).
                if (!window._demo) { window._demo = true; applyState(DEMO); }
            }
        }

        function applyState(d) {
            const s = d.state || {};
            if (s.mode !== undefined) {
                document.getElementById('s_mode').textContent = s.mode;
                document.getElementById('s_freq').textContent = s.freq.toFixed(4);
                document.getElementById('s_vol').textContent = s.vol;
                document.getElementById('s_gain').textContent = s.gain.toFixed(1);
                document.getElementById('s_mute').textContent = s.mute ? 'ON' : 'off';
                document.getElementById('s_fm').textContent = (s.fmmode || '--').toUpperCase();
                document.getElementById('s_feed').textContent = (s.feed || '--').toUpperCase();

                setOn(['m_p25', 'm_adsb', 'm_fm'], { 'P25': 'm_p25', 'ADS-B': 'm_adsb', 'FM': 'm_fm' }[s.mode]);
                setOn(['f_listen', 'f_scan', 'f_pocsag', 'f_wfm'],
                    (s.mode === 'FM') ? ({ 'listen': 'f_listen', 'scan': 'f_scan', 'pocsag': 'f_pocsag', 'wfm': 'f_wfm' }[s.fmmode]) : null
                );

                if (!dragging) {
                    volEl.value = s.vol;
                    paintVol(s.vol);
                }
            }

            renderAircraft(d.aircraft || []);
            renderPages(d.pages || []);

            const lg = document.getElementById('log');
            const atBottom = lg.scrollTop + lg.clientHeight >= lg.scrollHeight - 20;
            lg.textContent = (d.log || []).join('\n');
            if (atBottom) lg.scrollTop = lg.scrollHeight;
        }

        function esc(s) {
            return String(s == null ? '' : s).replace(/[&<>]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;' }[c]));
        }

        function renderAircraft(ac) {
            document.getElementById('ac_n').textContent = ac.length ? ('(' + ac.length + ')') : '';
            document.getElementById('ac_empty').style.display = ac.length ? 'none' : 'block';
            document.getElementById('ac_tbl').style.display = ac.length ? 'table' : 'none';
            document.getElementById('ac_body').innerHTML = ac.map(a => {
                const pos = a.pos ? (a.lat.toFixed(4) + '</td><td>' + a.lon.toFixed(4)) : '--</td><td>--';
                return '<tr><td>' + esc(a.icao) + '</td><td>' + esc(a.cs || '') + '</td><td>' + (a.alt || 0) +
                    '</td><td>' + (a.vel || 0) + '</td><td>' + (a.hdg || 0) + '</td><td>' + pos + '</td></tr>';
            }).join('');
        }

        function renderPages(pg) {
            document.getElementById('pg_n').textContent = pg.length ? ('(' + pg.length + ')') : '';
            document.getElementById('pg_empty').style.display = pg.length ? 'none' : 'block';
            document.getElementById('pg_tbl').style.display = pg.length ? 'table' : 'none';
            document.getElementById('pg_body').innerHTML = pg.slice().reverse().map(p => {
                const tm = new Date(p.ts * 1000).toLocaleTimeString();
                return '<tr><td>' + tm + '</td><td>' + p.ric + '</td><td>' + p.func + '</td><td>' + p.type +
                    '</td><td>' + esc(p.text) + '</td></tr>';
            }).join('');
        }

        /* demo data — only used for offline preview (see tick's catch) */
        const DEMO = {
            state: { mode: 'FM', freq: 154.785, vol: 62, gain: 30.0, mute: false, fmmode: 'pocsag', feed: 'on' },
            aircraft: [
                { icao: 'A1B2C3', cs: 'UAL482', alt: 34000, vel: 451, hdg: 92, pos: true, lat: 43.0721, lon: -89.4008 },
                { icao: 'D4E5F6', cs: 'DAL119', alt: 28950, vel: 402, hdg: 271, pos: true, lat: 43.1120, lon: -89.3320 }
            ],
            pages: [
                { ts: Date.now() / 1000 - 40, ric: 1234567, func: 3, type: 'A', text: 'STRUCTURE FIRE 400 BLK STATE ST' },
                { ts: Date.now() / 1000 - 8,  ric: 8901234, func: 0, type: 'N', text: 'UNIT 12 RESPOND CODE 3' }
            ],
            log: [
                'FM     freq=154.7850 MHz vol=62 gain=30.0 dB mute=0 fmmode=pocsag feed=on',
                "page RIC=1234567 F=3 A 'STRUCTURE FIRE 400 BLK STATE ST'",
                "page RIC=8901234 F=0 N 'UNIT 12 RESPOND CODE 3'",
                '{"app":"ADS-B","icao":"A1B2C3","cs":"UAL482","alt":34000}'
            ]
        };

        /* boot */
        bakeGrain();
        bakeDither();
        scatterGrain();
        drawRule();
        paintVol(0);
        window.addEventListener('resize', () => { drawRule(); });

        setInterval(tick, 800);
        tick();
    </script>
</body>
</html>"""


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
    
def prompt_for_port():
    ports = serial.tools.list_ports.comports()
    
    if not ports:
        print("No serial ports dude.")
        manual_port = input("Please gimmie the port bud (e.g., COM3 or /dev/ttyACM0): ")
        
        if not manual_port.strip():
            raise SystemExit("No serial ports found! Please stand up and turn 360 degrees, then plug in your device.")
        
        return manual_port.strip()
        
    print("Avaible serial ports:")
    for i, port in enumerate(ports):
        print(f"[{i}] {port.device} - {port.description}")
        
    while True:    
        choice = input("Enter the number of the port you want to use pleaseeeeeee: ")
    
        try:
            selected_port = ports[int(choice)].device
            print(f"Selected: {selected_port}")
            return selected_port
        except (ValueError, IndexError):
            # raise SystemExit("Invalid selection. Nice job, I'm dying now. Goodbye World.")
            print("Invalid selection. Please type the number in the brackets.")

def main():
    ap = argparse.ArgumentParser(description="LakeShark headless control panel")
    ap.add_argument("-p", "--port", default=None, help="serial port")
    ap.add_argument("-b", "--baud", type=int, default=115200)
    ap.add_argument("--http", type=int, default=8674, help="local web port")
    ap.add_argument("--no-browser", action="store_true")
    args = ap.parse_args()
    
    port_to_use = args.port
    if port_to_use is None:
        port_to_use = prompt_for_port()

    Handler.radio = Radio(port_to_use, args.baud)
    url = "http://127.0.0.1:%d" % args.http
    httpd = ThreadingHTTPServer(("127.0.0.1", args.http), Handler)
    print("LakeShark control panel on %s  ->  %s" % (port_to_use, url))
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
