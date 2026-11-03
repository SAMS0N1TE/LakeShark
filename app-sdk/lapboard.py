"""A LakeShark board's console, for native apps: install, list, info, rm, logs.

The board's `app` command (components/apps/tui/ls_lap_console.c) takes an
upload as lines:

    app put NAME.lap SIZE SHA256      -> APP OK put chunk=72 next=0
    app data SEQ BASE64               -> APP OK data next=N received=M   (72 bytes a line)
    app end                           -> APP OK end NAME.lap | APP ERR verify WHY
    app list                          -> APP OK app id=.. name=.. ... , then APP OK list end
    app info ID                       -> APP OK app ... [APP OK runtime ...] | APP ERR absent ..
    app rm ID [all]                   -> APP OK rm refresh=queued | APP ERR ..

Two ways to the board:

  BridgeLink  through the desktop bridge (host/lsterm/lsweb.py, which lsdesk
              runs) when it holds the board's port: HTTP on 127.0.0.1.
  SerialLink  the USB serial port directly, when nothing else holds it.

link() picks one. Nothing here listens on a network.
"""
import base64
import hashlib
import json
import os
import queue
import re
import threading
import time
import urllib.error
import urllib.request

CHUNK = 72                      # LS_LAP_CHUNK_BYTES
BRIDGE_PORT = 8770              # lsdesk's default (host/lsterm/lsdesk.py)
BOARD_VIDS = (0x1A86, 0x303A)   # CH343 (Nano, T-Display), Espressif USB


class BoardError(Exception):
    pass


# --------------------------------------------------------------- the protocol

class Session:
    """A console conversation: send(line), get(timeout) -> line or None."""

    def send(self, line):
        raise NotImplementedError

    def get(self, timeout):
        raise NotImplementedError

    def expect(self, pattern, timeout):
        """The next line matching pattern (a compiled regex or str); BoardError on timeout."""
        rx = re.compile(pattern) if isinstance(pattern, str) else pattern
        end = time.time() + timeout
        while True:
            left = end - time.time()
            if left <= 0:
                raise BoardError("the board did not answer (waited %.0f s)" % timeout)
            line = self.get(left)
            if line is not None and rx.search(line):
                return line


def app_name(data):
    """The file name the board keeps an app under: its id, from the manifest."""
    import lap
    m = lap.Elf(data).manifest
    return m["id"] + ".lap", m


def do_install(sess, data, name, progress=None, window=4):
    """Upload `data` as /sdcard/apps/NAME. progress(sent, total, text) is called
    as it goes. `window` data lines may be in flight before their replies.
    Returns the board's final reply line."""
    total = len(data)
    if not 52 <= total <= 512 * 1024:
        raise BoardError("app must be 52..524288 bytes")
    sha = hashlib.sha256(data).hexdigest()
    say = progress or (lambda *a: None)
    say(0, total, "asking the board")
    sess.send("app put %s %d %s" % (name, total, sha))
    r = sess.expect(r"^APP (OK put|ERR)", 10)
    if r.startswith("APP ERR"):
        raise BoardError(r)
    chunks = [data[o:o + CHUNK] for o in range(0, total, CHUNK)]
    sent = acked = 0
    reply = re.compile(r"^APP (OK data|ERR)")
    while acked < len(chunks):
        while sent < len(chunks) and sent - acked < max(1, window):
            sess.send("app data %d %s" % (sent, base64.b64encode(chunks[sent]).decode()))
            sent += 1
        r = sess.expect(reply, 10)
        if r.startswith("APP ERR"):
            raise BoardError(r)
        m = re.search(r"next=(\d+) received=(\d+)", r)
        if not m or int(m.group(1)) != acked + 1:
            raise BoardError("out of step: " + r)
        acked += 1
        say(int(m.group(2)), total, "sending")
    say(total, total, "checking on the board")
    sess.send("app end")
    r = sess.expect(r"^APP (OK end|ERR)", 30)
    if r.startswith("APP ERR"):
        raise BoardError(r)
    say(total, total, "installed")
    return r


def parse_app(line):
    """APP OK app id=.. name=.. -> dict; the name and version are percent-encoded."""
    from urllib.parse import unquote
    out = {}
    for word in line.split()[3:]:
        k, _, v = word.partition("=")
        out[k] = unquote(v)
    return out


def do_rpc(sess, lines, until, timeout=5.0):
    """Collect through `until`, or until 250 ms quiet when until is None.

    Quiet mode is bounded by timeout and permits silent commands (term ev).
    Use an explicit terminator for transfers and other slow, framed replies.
    """
    rx = re.compile(until) if until is not None else None
    for line in lines:
        sess.send(line)
    got, end = [], time.monotonic() + timeout
    quiet_end = end
    while time.monotonic() < min(end, quiet_end):
        line = sess.get(min(end, quiet_end) - time.monotonic())
        if line is None:
            continue
        got.append(line)
        if rx is None:
            quiet_end = time.monotonic() + 0.25
        elif rx.search(line):
            return got
    if rx is None:
        return got
    raise BoardError("the board did not answer: " + "; ".join(lines))


# ------------------------------------------------------------------ a splitter

class Lines:
    """Bytes from the board's console to lines. A screen feed left running
    (0x1E/0x1C, 4 hex length, payload, 0x1F, 4 hex CRC) is skipped."""

    def __init__(self):
        self.buf = bytearray()
        self.text = bytearray()

    def feed(self, data):
        self.buf += data
        while self.buf:
            i = next((k for k, b in enumerate(self.buf) if b in (0x1E, 0x1C)), -1)
            if i < 0:
                self.text += self.buf
                self.buf.clear()
                break
            self.text += self.buf[:i]
            del self.buf[:i]
            if len(self.buf) < 5:
                break
            try:
                n = int(self.buf[1:5], 16)
            except ValueError:
                del self.buf[:1]
                continue
            if len(self.buf) < 10 + n:
                break
            del self.buf[:10 + n]
        *done, rest = bytes(self.text).split(b"\n")
        self.text = bytearray(rest[-4096:])
        out = []
        for raw in done:
            line = raw.decode("utf-8", "replace").strip("\r").strip()
            if line.startswith("lakeshark>"):            # the prompt, with what was typed after it
                line = line[len("lakeshark>"):].strip()
                if not line.startswith("APP "):
                    continue
            if line:
                out.append(line)
        return out


# ---------------------------------------------------------------- serial link

def board_ports():
    """Ports that look like a LakeShark board, minus those ~/.lsdesk.json skips."""
    import serial.tools.list_ports
    try:
        cfg = json.load(open(os.path.join(os.path.expanduser("~"), ".lsdesk.json"), encoding="utf-8"))
    except (OSError, ValueError):
        cfg = {}
    skip = set(x.upper() for x in cfg.get("skip", []))
    ports = [p for p in serial.tools.list_ports.comports()
             if p.vid in BOARD_VIDS and p.device.upper() not in skip and (p.serial_number or "").upper() not in skip]
    want = cfg.get("board")
    ports.sort(key=lambda p: p.serial_number != want)
    return ports


class SerialSession(Session):
    def __init__(self, port, baud=115200, opener=None):
        if opener:
            self.s = opener(port)
        else:
            import serial
            s = serial.Serial()
            s.port, s.baudrate, s.timeout = port, baud, 0.05
            s.dtr = s.rts = False                    # high on a CH343 resets the board
            s.open()
            self.s = s
        self.q = queue.Queue()
        self.split = Lines()
        self.alive = True
        self.t = threading.Thread(target=self._read, daemon=True)
        self.t.start()

    def _read(self):
        while self.alive:
            try:
                data = self.s.read(4096)
            except Exception:
                self.alive = False
                self.q.put(None)
                return
            if data:
                for line in self.split.feed(data):
                    self.q.put(line)

    def send(self, line):
        self.s.write(line.encode() + b"\r")

    def get(self, timeout):
        try:
            return self.q.get(timeout=max(0.0, timeout))
        except queue.Empty:
            return None

    def close(self):
        self.alive = False
        self.t.join(1)
        try:
            self.s.close()
        except Exception:
            pass


class SerialLink:
    """The board's USB serial port, opened for each job."""
    kind = "serial"

    def __init__(self, port=None, opener=None):
        self.opener = opener
        if not port and not opener:
            ports = board_ports()
            if not ports:
                raise BoardError("no LakeShark board on USB (or name it: --port COMx)")
            if len(ports) > 1 and not ports[0].serial_number:
                raise BoardError("several boards: name one with --port (%s)" % ", ".join(p.device for p in ports))
            port = ports[0].device
        self.port = port

    def describe(self):
        return "USB serial %s" % self.port

    def session(self):
        return SerialSession(self.port, opener=self.opener)

    def install(self, data, name, progress=None):
        s = self.session()
        try:
            s.send("")
            return do_install(s, data, name, progress)
        finally:
            s.close()

    def rpc(self, lines, until, timeout=5.0):
        s = self.session()
        try:
            return do_rpc(s, lines, until, timeout)
        finally:
            s.close()

    def tail(self, on_line, stop):
        s = self.session()
        try:
            while not stop():
                line = s.get(0.5)
                if line is not None:
                    on_line(line)
        finally:
            s.close()


# ---------------------------------------------------------------- bridge link

def bridge_url():
    try:
        cfg = json.load(open(os.path.join(os.path.expanduser("~"), ".lsdesk.json"), encoding="utf-8"))
    except (OSError, ValueError):
        cfg = {}
    return "http://127.0.0.1:%d" % int(os.environ.get("LSDK_BRIDGE_PORT") or cfg.get("port", BRIDGE_PORT))


def _http(url, data=None, timeout=10, method=None):
    req = urllib.request.Request(url, data=data, method=method or ("POST" if data is not None else "GET"))
    req.add_header("X-Studio", "1")                  # the bridge takes jobs only with it
    if data is not None:
        req.add_header("Content-Type", "application/octet-stream")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode() or "null")


class BridgeLink:
    """The desktop bridge (lsweb.py) holds the port; it runs the job."""
    kind = "bridge"

    def __init__(self, url):
        self.url = url

    def describe(self):
        return "the desktop bridge at %s" % self.url

    def status(self):
        return _http(self.url + "/status", timeout=1.5)

    def install(self, data, name, progress=None):
        from urllib.parse import quote
        j = _http(self.url + "/app/install?name=" + quote(name), data=data, timeout=15)
        if not j or not j.get("ok"):
            raise BoardError((j or {}).get("message", "the bridge refused the job"))
        say = progress or (lambda *a: None)
        while True:
            time.sleep(0.25)
            st = _http(self.url + "/app/job", timeout=5)
            say(st.get("sent", 0), st.get("total", len(data)), st.get("message", ""))
            if st.get("state") == "done":
                return st.get("reply", "")
            if st.get("state") == "error":
                raise BoardError(st.get("message", "install failed"))

    def rpc(self, lines, until, timeout=5.0):
        body = json.dumps({"lines": lines, "until": until, "timeout": timeout}).encode()
        j = _http(self.url + "/board/rpc", data=body, timeout=timeout + 5)
        if not j.get("ok"):
            raise BoardError(j.get("message", "no answer"))
        return j["lines"]

    def tail(self, on_line, stop):
        since = None
        while not stop():
            q = "/board/tail?wait=2" + ("&since=%d" % since if since is not None else "")
            j = _http(self.url + q, timeout=10)
            since = j.get("seq", since)
            for line in j.get("lines", []):
                on_line(line)


def find_bridge():
    """The desktop bridge, if it is running and has the board; else None."""
    url = bridge_url()
    try:
        st = _http(url + "/status", timeout=1.0)
    except (OSError, urllib.error.URLError, ValueError):
        return None
    if not isinstance(st, dict) or "board" not in st:
        return None
    if not st.get("board"):
        raise BoardError("the desktop bridge is running but has no board (plugged in?)")
    return BridgeLink(url)


def link(port=None, via="auto"):
    """The way to the board: the bridge when it holds the port, else serial."""
    if via in ("auto", "bridge") and not port:
        b = find_bridge()
        if b:
            return b
        if via == "bridge":
            raise BoardError("the desktop bridge is not running (start LakeShark Desktop)")
    return SerialLink(port)


# ---------------------------------------------------------- high-level calls

def install(lk, data, progress=None):
    name, manifest = app_name(data)
    reply = lk.install(data, name, progress)
    return name, manifest, reply


def list_apps(lk):
    return [parse_app(l) for l in lk.rpc(["app list"], r"^APP (OK list end|ERR)") if l.startswith("APP OK app ")]


def info(lk, app_id):
    lines = lk.rpc(["app info " + app_id], r"^APP (OK app|ERR)")
    for l in lines:
        if l.startswith("APP ERR"):
            raise BoardError(l)
    return [parse_app(l) for l in lines if l.startswith("APP OK app ")]


def remove(lk, app_id, data=False):
    lines = lk.rpc(["app rm %s%s" % (app_id, " all" if data else "")], r"^APP (OK rm|ERR)")
    last = lines[-1]
    if last.startswith("APP ERR"):
        raise BoardError(last)
    return last


def open_on_board(lk, app_id):
    """Open the app in a desk window (the Nano's LakeShark Desktop)."""
    return lk.rpc(["desk open " + app_id], r"^desk: ", timeout=3)
