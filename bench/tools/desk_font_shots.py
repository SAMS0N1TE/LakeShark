"""The desktop page's "PC font for text" toggle, off and on, drawn by the
page's own code (web/screen.js) in headless Edge from a recorded cell feed.

No board or serial port. Each FEED is one full update of the cell feed as
`lssim` writes it with LSSIM_FEED_OUT (G, P, C and X records, raw). A local
server on 127.0.0.1 serves the repo and a harness page whose WebSocket is a
stand-in that answers like lsweb.py and plays the feed in 0x1E frames; the
canvas's own bitmap is posted back to the server, so the PNGs are the
backing store pixel for pixel, not a scaled screenshot.

    python bench/tools/desk_font_shots.py OUTDIR FEED [FEED ...]

A feed, in PowerShell (desk with DIAG and SETTINGS open, 7x13):

    $env:LSSIM_GRID='200x60'; $env:LSSIM_FEED_OUT='bench/build-desk/font/desk_text.feed'
    bench/build/lssim.exe desk -l -F 4 -x '!diag,~4,!set,~4' -f 6 -o bench/build-desk/font/desk_text.bmp

Writes NAME-off.png, NAME-on.png and NAME-diff.png (changed pixels in red)
per feed, and checks that every cell that is not text (a code past 255, a
byte CELL_CP in screen.js leaves 0, a picture cell) is identical off and on,
and that some text did change. Also checks screen.js's Cyrillic table against
tui_cyr.h. Exit 1 on a failed check.
"""
import base64
import functools
import http.server
import io
import re
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
WEB = ROOT / "components/apps/tui/web"
EDGE = r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"

HARNESS = r"""<!doctype html><html><head><meta charset="utf-8">
<base href="/components/apps/tui/web/">
<style>html,body{margin:0;background:#000}</style></head><body>
<div id="stage" style="width:1600px;height:900px"><canvas id="screen"></canvas>
<div id="note"><p id="say"></p><form id="login" hidden><input id="pass"></form><button id="again" hidden></button></div>
<aside id="panel" hidden></aside></div>
<div id="bar"><button id="kbd"></button><button id="gear" hidden></button></div>
<input id="type"><pre id="out">pending</pre>
<script>
localStorage.setItem('lakeshark-token', 'usb');
localStorage.setItem('lakeshark-pcfont', '__PC__');
localStorage.setItem('lakeshark-fit', '0');
const FEED = '/feed/__FEED__';
function crc16(b) { let c = 0xffff; for (const x of b) { c ^= x << 8; for (let k = 0; k < 8; k++) c = c & 0x8000 ? ((c << 1) ^ 0x1021) & 0xffff : (c << 1) & 0xffff; } return c; }
function frame(body) {
  const h = s => new TextEncoder().encode(s), hx = n => n.toString(16).padStart(4, '0');
  const out = new Uint8Array(body.length + 10);
  out[0] = 0x1e; out.set(h(hx(body.length)), 1); out.set(body, 5); out[5 + body.length] = 0x1f;
  out.set(h(hx(crc16(body))), 6 + body.length);
  return out.buffer;
}
class WebSocket {
  constructor() { this.readyState = 0; setTimeout(() => { this.readyState = 1; this.onopen(); }, 0); }
  say(m) { setTimeout(() => this.onmessage({ data: m }), 0); }
  async send(t) {
    if (t.startsWith('token')) this.say('ok local');
    else if (t === 'term caps') this.say('term: caps ev1');
    else if (t.startsWith('term web ack')) {
      const raw = new Uint8Array(await (await fetch(FEED)).arrayBuffer());
      const cols = raw[1] | raw[2] << 8, rows = raw[3] | raw[4] << 8;
      this.say(`term: web begin cols ${cols} rows ${rows} cells`);
      setTimeout(() => {
        for (let i = 0; i < raw.length; i += 4000) this.onmessage({ data: frame(raw.subarray(i, i + 4000)) });
        this.onmessage({ data: frame(new Uint8Array(0)) });
      }, 50);
    } else if (t === 'term ack' && !this.done) {
      this.done = true;
      if (localStorage.getItem('lakeshark-pcfont') === '1') await pcFontStart();
      await document.fonts.ready;
      setTimeout(() => requestAnimationFrame(() => requestAnimationFrame(() => {
        fetch('/shot/__FEED__/__PC__?gamma=' + encodeURIComponent(pcGamma.toFixed(3) + ' ' + [...document.fonts].map(f => f.family + ':' + f.weight + ':' + f.status).join(',')), { method: 'POST', body: document.getElementById('screen').toDataURL('image/png') });
      })), 300);
    }
  }
}
</script>
<script>var exports = {};</script>
<script src="xterm-headless.js"></script>
<script src="screen.js"></script>
</body></html>"""


class Handler(http.server.SimpleHTTPRequestHandler):
    feeds = {}
    shots = {}
    gammas = {}

    def log_message(self, *a):
        pass

    def do_GET(self):
        m = re.match(r"^/harness/(\w[\w.-]*)/([01])$", self.path)
        if m:
            body = HARNESS.replace("__FEED__", m.group(1)).replace("__PC__", m.group(2)).encode()
            return self.reply(body, "text/html; charset=utf-8")
        if self.path.startswith("/feed/"):
            return self.reply(self.feeds[self.path[6:]], "application/octet-stream")
        return super().do_GET()

    def do_POST(self):
        body = self.rfile.read(int(self.headers["Content-Length"]))
        key, _, query = self.path[6:].partition("?")
        self.shots[key] = body.decode()
        self.gammas[key] = __import__("urllib.parse").parse.unquote(query.partition("=")[2])
        self.reply(b"", "text/plain")

    def reply(self, body, kind):
        self.send_response(200)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)


def stop_edge(profile):
    tag = str(profile.resolve()).replace("'", "''")
    subprocess.run(["powershell", "-NoProfile", "-Command",
                    "Get-CimInstance Win32_Process -Filter \"Name='msedge.exe'\" | Where-Object { $_.CommandLine -like "
                    f"'*{tag}*' }} | ForEach-Object {{ Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }}"],
                   capture_output=True, timeout=60)
    time.sleep(0.5)


def shoot(port, name, pc, profile):
    url = f"http://127.0.0.1:{port}/harness/{name}/{pc}"
    key = f"{name}/{pc}"
    Handler.shots.pop(key, None)
    # Edge's launcher can hand over to another process and return at once, so
    # wait for the page's own post, then stop every Edge on this profile.
    subprocess.Popen([EDGE, "--headless=new", "--disable-gpu", "--no-first-run", "--hide-scrollbars",
                      f"--user-data-dir={profile.resolve()}", "--force-device-scale-factor=1",
                      "--window-size=1600,1000", url])
    end = time.time() + 90
    while key not in Handler.shots and time.time() < end:
        time.sleep(0.2)
    stop_edge(profile)
    m = re.search(r"data:image/png;base64,([A-Za-z0-9+/=]+)", Handler.shots.get(key, ""))
    if not m:
        raise SystemExit(f"{name} pc={pc}: no canvas came back")
    return Image.open(io.BytesIO(base64.b64decode(m.group(1)))).convert("RGB")


def cells_of(raw):
    """(cols, rows, cw, ch, codes, kinds) from one full update."""
    cols, rows = struct.unpack_from("<HH", raw, 1)
    cw, ch = raw[6], raw[7]
    codes, kinds = [32] * (cols * rows), [0] * (cols * rows)
    at = 8 + 33
    while at < len(raw):
        t = raw[at]
        first, n = struct.unpack_from("<HH", raw, at + 1)
        at += 5
        size = 3 if t == 0x43 else 8
        for j in range(n):
            codes[first + j] = struct.unpack_from("<H", raw, at)[0]
            kinds[first + j] = raw[at + 3] if t == 0x58 else 0
            at += size
    return cols, rows, cw, ch, codes, kinds


def text_bytes():
    """CELL_CP's text bytes from screen.js, checked against tui_cyr.h."""
    js = (WEB / "screen.js").read_text(encoding="utf-8")
    slot_cp = [int(x, 16) for x in re.findall(r"0x([0-9a-f]+)", re.search(r"CYR_SLOT_CP = \[(.*?)\];", js, re.S).group(1))]
    ranges = re.findall(r"\[0x([0-9a-f]+), 0x([0-9a-f]+)\]", re.search(r"CYR_SLOT_BYTES = \[(.*?)\];\n", js, re.S).group(1))
    js_map = {}
    s = 0
    for a, b in ranges:
        for c in range(int(a, 16), int(b, 16) + 1):
            js_map[c] = slot_cp[s]
            s += 1
    hdr = (ROOT / "components/apps/tui/tuilib/tui_cyr.h").read_text()
    c_map = {int(b, 16): int(cp, 16) for cp, b in re.findall(r"\{0x([0-9A-F]+),0x([0-9A-F]+)\}", hdr)}
    if js_map != c_map:
        raise SystemExit(f"screen.js Cyrillic table differs from tui_cyr.h: {sorted(set(js_map.items()) ^ set(c_map.items()))}")
    return set(range(0x21, 0x7F)) | set(js_map)


def main():
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    text = text_bytes()
    feeds = [Path(p) for p in sys.argv[2:]]
    Handler.feeds = {p.stem: p.read_bytes() for p in feeds}
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), functools.partial(Handler, directory=str(ROOT)))
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    port = srv.server_address[1]
    profile = out / "edge-profile"
    bad = 0
    for p in feeds:
        name = p.stem
        cols, rows, cw, ch, codes, kinds = cells_of(Handler.feeds[name])
        off, on = shoot(port, name, 0, profile), shoot(port, name, 1, profile)
        off.save(out / f"{name}-off.png")
        on.save(out / f"{name}-on.png")
        if off.size != (cols * cw, rows * ch) or on.size != off.size:
            print(f"{name}: canvas {off.size}/{on.size}, expected {(cols * cw, rows * ch)}")
            bad += 1
            continue
        a, b = off.load(), on.load()
        diff = off.copy()
        dp = diff.load()
        nontext_bad = text_changed = text_cells = 0
        for i, code in enumerate(codes):
            is_text = code < 256 and code in text and kinds[i] != 1
            text_cells += is_text
            x0, y0 = (i % cols) * cw, (i // cols) * ch
            changed = False
            for y in range(y0, y0 + ch):
                for x in range(x0, x0 + cw):
                    if a[x, y] != b[x, y]:
                        changed = True
                        dp[x, y] = (255, 0, 0)
            if changed and is_text:
                text_changed += 1
            elif changed:
                nontext_bad += 1
                if nontext_bad <= 5:
                    print(f"{name}: non-text cell {i % cols},{i // cols} code {code:#x} kind {kinds[i]} changed")
        diff.save(out / f"{name}-diff.png")
        print(f"{name}: {cols}x{rows} at {cw}x{ch}; text cells {text_cells}, redrawn {text_changed}; "
              f"non-text cells changed {nontext_bad}; PC face gamma and fonts {Handler.gammas.get(name + '/1')}")
        bad += nontext_bad > 0 or text_changed == 0
    srv.shutdown()
    print("FAIL" if bad else "PASS")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
