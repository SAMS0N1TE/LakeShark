"""LakeShark Studio's screenshots, from headless Edge against a local server.

    python bench/tools/studio_shots.py [OUTDIR]      (default bench/build-desk/studio)

No board and no serial port: Studio runs here on 127.0.0.1 with a fresh
workspace, and its board is the fake one from app-sdk/tests/test_lsdk_install.py
behind the real desktop bridge (host/lsterm/lsweb.py), on a port of its own
(LSDK_BRIDGE_PORT), so a real bridge on 8770 is never asked. Edge is driven
over the DevTools protocol with a small WebSocket client below.

Writes the radio template in the simulator (with the SIM radio and with none),
the start page, the edit view in every desk scheme, completion with the
editor split, find and replace, the problems' copy buttons and menus (editor,
files, run panel, logs), the asset view, install, a rearranged dock and the
layout menu, the palette, settings, F1, search and the terminal, and the AI
pages and panel: studio-*.png.
"""
import base64
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SDK = ROOT / "app-sdk"
EDGE = r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"
sys.path[:0] = [str(SDK), str(SDK / "studio"), str(SDK / "tests"), str(ROOT / "host" / "lsterm")]


# ------------------------------------------------------------------ CDP

class CDP:
    def __init__(self, ws_url):
        host_port, path = ws_url[len("ws://"):].split("/", 1)
        host, port = host_port.split(":")
        self.s = socket.create_connection((host, int(port)))
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall(("GET /%s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                        "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % (path, host_port, key)).encode())
        head = b""
        while b"\r\n\r\n" not in head:
            head += self.s.recv(1)
        self.n = 0

    def _send(self, text):
        data = text.encode()
        n = len(data)
        head = bytes([0x81]) + (bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack(">H", n)
                                if n < 65536 else bytes([0x80 | 127]) + struct.pack(">Q", n))
        mask = os.urandom(4)
        self.s.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

    def _exact(self, n):
        b = bytearray()
        while len(b) < n:
            got = self.s.recv(n - len(b))
            if not got:
                raise EOFError
            b += got
        return bytes(b)

    def _recv(self):
        msg = b""
        while True:
            h = self._exact(2)
            n = h[1] & 127
            if n == 126:
                n = struct.unpack(">H", self._exact(2))[0]
            elif n == 127:
                n = struct.unpack(">Q", self._exact(8))[0]
            msg += self._exact(n)
            if h[0] & 0x80:
                return json.loads(msg.decode())

    def call(self, method, **params):
        self.n += 1
        self._send(json.dumps({"id": self.n, "method": method, "params": params}))
        while True:
            m = self._recv()
            if m.get("id") == self.n:
                if "error" in m:
                    raise RuntimeError("%s: %s" % (method, m["error"]))
                return m.get("result", {})

    def js(self, expr, timeout=120):
        r = self.call("Runtime.evaluate", expression=expr, awaitPromise=True, returnByValue=True, timeout=timeout * 1000)
        if r.get("exceptionDetails"):
            raise RuntimeError("page: %s" % r["exceptionDetails"].get("exception", {}).get("description", r["exceptionDetails"]))
        return r.get("result", {}).get("value")

    def shot(self, path):
        png = base64.b64decode(self.call("Page.captureScreenshot", format="png")["data"])
        Path(path).write_bytes(png)
        print("wrote", path, flush=True)

    def key(self, text):
        for ch in text:
            if ch == "\n":
                self.call("Input.dispatchKeyEvent", type="keyDown", key="Enter", code="Enter", windowsVirtualKeyCode=13, text="\r")
                self.call("Input.dispatchKeyEvent", type="keyUp", key="Enter", code="Enter", windowsVirtualKeyCode=13)
                continue
            self.call("Input.dispatchKeyEvent", type="keyDown", text=ch, key=ch, unmodifiedText=ch)
            self.call("Input.dispatchKeyEvent", type="keyUp", key=ch)


def edge(profile):
    if profile.exists():
        shutil.rmtree(profile, ignore_errors=True)
    profile.mkdir(parents=True)
    p = subprocess.Popen([EDGE, "--headless=new", "--disable-gpu", "--no-first-run", "--hide-scrollbars",
                          "--remote-debugging-port=0", "--user-data-dir=" + str(profile), "--window-size=1500,860",
                          "--force-device-scale-factor=1", "about:blank"])
    port_file = profile / "DevToolsActivePort"
    end = time.time() + 30
    while not port_file.exists() and time.time() < end:
        time.sleep(0.2)
    for _ in range(50):                     # Edge may still be writing it
        try:
            port = int(port_file.read_text().split()[0])
            break
        except (OSError, ValueError, IndexError):
            time.sleep(0.2)
    for _ in range(50):
        try:
            pages = json.load(urllib.request.urlopen("http://127.0.0.1:%d/json/list" % port, timeout=2))
            page = next(t for t in pages if t["type"] == "page")
            return p, CDP(page["webSocketDebuggerUrl"])
        except (OSError, StopIteration, ValueError):
            time.sleep(0.2)
    raise SystemExit("Edge's DevTools did not answer")


def stop_edge(profile):
    tag = str(profile.resolve()).replace("'", "''")
    subprocess.run(["powershell", "-NoProfile", "-Command",
                    "Get-CimInstance Win32_Process -Filter \"Name='msedge.exe'\" | Where-Object { $_.CommandLine -like "
                    f"'*{tag}*' }} | ForEach-Object {{ Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }}"],
                   capture_output=True, timeout=60)


# ------------------------------------------------------------------ the scene

def fake_bridge():
    import serial
    import test_lsdk_install as t
    board = t.FakeBoard()
    t.FakeSerial.board = board
    serial.Serial = t.FakeSerial
    import lsweb
    srv = lsweb.serve(lambda: "COM20", 0, 2000000)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    end = time.time() + 5
    while not lsweb.BOARD.up.is_set() and time.time() < end:
        time.sleep(0.05)
    return srv.server_address[1], board


JS_HELPERS = r"""
window.T = {
  wait: (ms) => new Promise((r) => setTimeout(r, ms)),
  ctxAt(sel, dx, dy) {
    const e = document.querySelector(sel);
    const r = e.getBoundingClientRect();
    const x = r.left + (dx ?? r.width / 2), y = r.top + (dy ?? r.height / 2);
    (document.elementFromPoint(x, y) || e).dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, cancelable: true, clientX: x, clientY: y, button: 2 }));
  },
  menu(name, sub) {
    closeMenus();
    document.querySelector(`.menu[data-menu="${name}"]`).dispatchEvent(new MouseEvent('mousedown', { bubbles: true }));
    if (sub) {
      const row = [...document.querySelectorAll('.drop .it')].find((x) => x.textContent.startsWith(sub));
      row.classList.add('hot');
      row.dispatchEvent(new MouseEvent('mouseenter'));
    }
  },
};
"""


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "bench" / "build-desk" / "studio"
    out.mkdir(parents=True, exist_ok=True)
    bport, board = fake_bridge()
    os.environ["LSDK_BRIDGE_PORT"] = str(bport)        # Studio's board: the fake, never 8770
    os.environ["LSDK_STUDIO_SETTINGS"] = str(out / "studio-settings.json")
    work = out / "workspace"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    import lsdk
    lsdk.new_app("demo", "blank", work, "you")
    lsdk.new_app("scope", "radio", work, "you")
    lsdk.new_app("rolling", "dots", work, "you")
    try:                                               # an asset for the asset view
        from PIL import Image, ImageDraw
        (work / "demo" / "assets").mkdir()
        im = Image.new("RGB", (40, 36), (0, 0, 0))
        dr = ImageDraw.Draw(im)
        dr.polygon([(6, 30), (24, 4), (26, 30)], fill=(88, 224, 224))
        dr.rectangle([0, 31, 39, 35], fill=(56, 108, 160))
        im.save(work / "demo" / "assets" / "fin.png")
    except ImportError:
        pass
    import server
    srv = server.serve(0, work)
    sport = srv.server_address[1]
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    url = "http://127.0.0.1:%d/" % sport
    print("Studio on", url, "fake bridge on", bport, flush=True)
    W = lambda p: json.dumps(str(work / p))              # noqa: E731

    profile = out / ("edge-profile-%d" % os.getpid())        # its own each run: an old Edge may hold the last
    proc, cdp = edge(profile)
    shots = []

    def shot(name):
        cdp.shot(out / name)
        shots.append(name)

    try:
        cdp.call("Emulation.setDeviceMetricsOverride", width=1500, height=860, deviceScaleFactor=1, mobile=False)
        cdp.call("Page.enable")
        cdp.call("Page.navigate", url=url)
        time.sleep(3)
        cdp.js(JS_HELPERS)
        js = lambda code, timeout=120: cdp.js("(async () => { " + code + " })()", timeout)   # noqa: E731

        # the radio template, first: running in the simulator with the SIM radio; then a board with none
        js("await openProject(%s); setLayout(PRESETS.Board); await runSim(); await T.wait(5000);" % W("scope"), 600)
        shot("studio-radio-sim.png")
        js("noRadio = true; document.getElementById('noradio').checked = true; await runSim(); await T.wait(4500);", 600)
        shot("studio-radio-noradio.png")
        js("noRadio = false; document.getElementById('noradio').checked = false; setLayout(PRESETS.Default);")

        # the start page: templates, recent apps with their last screens, pins
        js("await openProject(%s); await api('recent', {path: %s, pinned: true}); await refreshState(); showStart(); await T.wait(800);" % (W("rolling"), W("scope")))
        shot("studio-start.png")

        # editing in every desk scheme
        js("await openProject(%s); await openFile('main.c', 90); await T.wait(300);" % W("demo"))
        for key in ("bay", "classic", "contrast", "teal", "amber", "night"):
            js("SET.scheme = '%s'; applySettings(); await T.wait(400);" % key)
            shot("studio-scheme-%s.png" % key)
        js("SET.scheme = 'bay'; applySettings();")

        # completion and hover docs, with the editor split
        js("openSplit('main.c'); await T.wait(300); goTo(85, 200, panes[0]); await T.wait(200);")
        cdp.key("\n    lap_ui_me")
        time.sleep(1.0)
        shot("studio-complete-split.png")
        js("CM.undo(panes[0].view); CM.undo(panes[0].view); closeSplit(); await T.wait(200);")

        # find and replace
        js("goTo(1, 1); run('edit.replace'); await T.wait(300); const v = panes[0].view.dom;"
           " const f = v.querySelector('.cm-search input[name=search]'); f.value = 'hellos'; f.dispatchEvent(new Event('input', {bubbles: true}));"
           " const r = v.querySelector('.cm-search input[name=replace]'); r.value = 'greetings'; r.dispatchEvent(new Event('input', {bubbles: true}));"
           " CM.selectNextOccurrence && 0; await T.wait(300);")
        shot("studio-find-replace.png")
        js("CM.closeSearchPanel(panes[0].view);")

        # a build that fails: squiggles, marks by the scrollbar, the copy buttons, the problems menu
        js("window.ORIG = panes[0].view.state.doc.toString(); const t = ORIG.replace('s.hellos++;', 's.hellos++\\n    lap_ui_meter(3, 16, s.hellos);')"
           ".replace('lap_log(\"hello number %d\", s.hellos);', 'lap_log(\"hello number %d\", s.hellos);\\n    lap_wobble(2);');"
           " panes[0].view.dispatch({changes: {from: 0, to: panes[0].view.state.doc.length, insert: t}}); await build(); goTo(69, 1); await T.wait(500);"
           " T.ctxAt('#problems .p[data-i=\"1\"] .msg', 40);")
        time.sleep(0.4)
        shot("studio-problems-copy.png")
        js("closeMenus(); T.ctxAt('#host-A .cm-line:nth-child(9)', 120, 6);")
        time.sleep(0.3)
        shot("studio-ctx-editor.png")
        js("closeMenus(); panes[0].view.dispatch({changes: {from: 0, to: panes[0].view.state.doc.length, insert: ORIG}}); await save(); showProblems([], '');")

        # the file manager: a folder, an image, several selected, its menu; then the asset view
        js("P.expanded.add('assets'); await reloadTree(); P.sel = new Set(['app.json', 'main.c']); drawTree(); document.getElementById('tree-q').value = ''; await T.wait(200);"
           " T.ctxAt('#tree .row[data-path=\"main.c\"]', 30);")
        time.sleep(0.3)
        shot("studio-ctx-files.png")
        js("closeMenus(); await openFile('assets/fin.png'); await T.wait(800);")
        shot("studio-asset.png")
        js("await openFile('main.c');")

        # the run panel's menu, and the logs' menu
        js("await runSim(); await T.wait(3500); const f = document.getElementById('sim'); const r = f.getBoundingClientRect();"
           " f.contentDocument.dispatchEvent(new MouseEvent('contextmenu', {bubbles: true, cancelable: true, clientX: r.width / 2 - 40, clientY: 140}));")
        time.sleep(0.4)
        shot("studio-ctx-run.png")
        js("closeMenus(); showPanel('simlog'); await T.wait(200); T.ctxAt('#simlog', 60, 10);")
        time.sleep(0.3)
        shot("studio-ctx-log.png")
        js("closeMenus();")

        # install, through the (fake) desktop bridge
        js("install(); for (let i = 0; i < 400; i++) { await T.wait(100); const m = document.getElementById('in-msg'); if (m && m.className === 'dlg-good') break; }", 300)
        time.sleep(0.4)
        shot("studio-install.png")
        print("  the fake board holds:", sorted(board.apps), flush=True)
        js("closeDialog();")

        # the dock: the simulator left of the code, the logs on the right, the API docs floating
        js("setLayout(PRESETS.Default); dockPanel('run', findTabsWith('editor')._key, 'left'); dockPanel('boardlog', findTabsWith('problems')._key, 'right');"
           " dockPanel('outline', findTabsWith('ai')._key, 'bottom'); floatPanel('docs', 1060, 420); LAYOUT.floats[0].w = 420; LAYOUT.floats[0].h = 380; renderDock();"
           " await listDocs(); await T.wait(300); setTail(true); await T.wait(1500);")
        time.sleep(1.5)
        shot("studio-dock-rearranged.png")
        js("T.menu('View', 'Layout'); await T.wait(300);")
        shot("studio-layout-menu.png")
        js("closeMenus(); setLayout(PRESETS.Default);")

        # the command palette, settings, F1, project search, the terminal
        js("palette('cmds'); const q = document.getElementById('pal-q'); q.value = '>sim'; q.dispatchEvent(new Event('input')); await T.wait(200);")
        shot("studio-palette.png")
        js("document.getElementById('palette').hidden = true; settingsPage('keys'); await T.wait(200);")
        shot("studio-settings-keys.png")
        js("closeDialog(); helpKeys(); await T.wait(200);")
        shot("studio-help-keys.png")
        js("closeDialog(); showPanel('search'); document.getElementById('search-q').value = 'lap_ui_keys'; await projectSearch();"
           " showPanel('terminal'); await termRun('info'); await T.wait(2500);")
        shot("studio-search-terminal.png")

        # the AI: Connect an AI, VS Code users, and the panel running a tool with no model
        js("await connectAI(); await T.wait(300);")
        shot("studio-connect-ai.png")
        js("closeDialog(); vscodePage(); await T.wait(200);")
        shot("studio-vscode.png")
        js("closeDialog(); showPanel('ai'); await aiSend('/sim_run'); await T.wait(500);", 600)
        shot("studio-ai-panel.png")
        js("api('stop', {})")
    finally:
        try:
            proc.terminate()
        except OSError:
            pass
        stop_edge(profile)
        shutil.rmtree(profile, ignore_errors=True)
        server.STUDIO.sim.stop()
        srv.shutdown()
    print("%d shots" % len(shots))
    return 0


if __name__ == "__main__":
    sys.exit(main())
