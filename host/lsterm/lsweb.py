"""The board's web screen in a browser on this computer, over USB.

    python lsweb.py [COMx] [--port 8770] [--baud 2000000]
    python lsweb.py --host 192.168.1.17  (open board directly; no bridge/serial)

Serves the board's own page (components/apps/tui/web) on 127.0.0.1 and
carries its WebSocket to the board's USB console: the page asks for
'term web ack z', this runs 'term serial <baud> ack z' on the port and
passes the frames through; 'term ack', 'term in <hex>' and 'term ping'
go back as console lines. Nothing listens beyond this computer, so there
is no password. Needs pyserial.

The bridge also lends the board's console to other jobs while it holds the
port, so they need not fight it for the port (app-sdk/lapboard.py's
BridgeLink): /board/rpc runs console lines and returns the replies,
/board/tail gives the console's lines, /app/install and /app/job upload an
app. LakeShark Studio (app-sdk/studio) is served at /studio/.
"""
import base64
import hashlib
import json
import os
import queue
import socket
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

WEB = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "components", "apps", "tui", "web")
SDK = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "app-sdk")
TYPES = {".html": "text/html; charset=utf-8", ".js": "text/javascript", ".png": "image/png"}
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def ws_send(sock, data, text=False):
    head = bytes([0x81 if text else 0x82])
    n = len(data)
    head += bytes([n]) if n < 126 else bytes([126]) + struct.pack(">H", n) if n < 65536 else bytes([127]) + struct.pack(">Q", n)
    sock.sendall(head + data)


def ws_recv(sock):
    """One message from the browser: (opcode, payload), or None when it went."""
    def exact(n):
        b = b""
        while len(b) < n:
            got = sock.recv(n - len(b))
            if not got:
                return None
            b += got
        return b
    h = exact(2)
    if not h:
        return None
    op, n = h[0] & 15, h[1] & 127
    if n == 126:
        n = struct.unpack(">H", exact(2))[0]
    elif n == 127:
        n = struct.unpack(">Q", exact(8))[0]
    mask = exact(4) if h[1] & 128 else b"\0\0\0\0"
    data = bytearray(exact(n) or b"")
    for i in range(len(data)):
        data[i] ^= mask[i % 4]
    return op, bytes(data)


def usb_serial(device):
    """The USB serial number behind a port, which survives a replug that moves its COM number."""
    import serial.tools.list_ports
    for p in serial.tools.list_ports.comports():
        if p.device == device:
            return p.serial_number
    return None


def port_of(serial_number):
    import serial.tools.list_ports
    for p in serial.tools.list_ports.comports():
        if serial_number and p.serial_number == serial_number:
            return p.device
    return None


class Split:
    """The serial stream as screen frames (0x1E or 0x1C, 4 hex length, payload,
    0x1F, 4 hex CRC) and console text between them, so the page gets frames as
    data and the board's replies (vol, mesh, say ...) as lines."""

    def __init__(self):
        self.buf = bytearray()
        self.text = bytearray()

    def feed(self, data, on_frame=None):
        self.buf += data
        frames = bytearray()
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
                self.text += self.buf[:1]
                del self.buf[:1]
                continue
            if len(self.buf) < 10 + n:
                break
            frame = bytes(self.buf[:10 + n])
            if on_frame is not None:
                on_frame(frame)
            else:
                frames += frame
            del self.buf[:10 + n]
        *done, rest = bytes(self.text).split(b"\n")
        self.text = bytearray(rest[-2048:])
        lines = [l.decode("utf-8", "replace").strip("\r").strip() for l in done]
        return bytes(frames), [l for l in lines if l and not l.startswith(("lakeshark>", "term "))]


class Board:
    """The USB console, one browser at a time. When the board goes (unplugged,
    or the port taken), it is looked for again every second by its USB serial
    number, and `up` says whether it is here. `find` gives the port to try."""

    def __init__(self, find, baud):
        self.find, self.baud = find, baud
        self.s = None
        self.device = None
        self.lock = threading.Lock()
        self.rpc_lock = threading.Lock()     # a console job reading the port itself (ConsoleSession)
        self.up = threading.Event()
        threading.Thread(target=self.keep, daemon=True).start()

    def keep(self):
        import serial
        while True:
            if not self.up.is_set():
                device = self.find()
                if device:
                    try:
                        s = serial.Serial()
                        s.port, s.baudrate, s.timeout = device, 115200, 0.01
                        s.dtr = s.rts = False            # high on a CH343 resets the board
                        s.open()
                        try:
                            s.set_buffer_size(rx_size=1 << 20, tx_size=1 << 14)
                        except (AttributeError, NotImplementedError):
                            pass
                        self.s, self.device = s, device
                        self.up.set()
                        print("the board is on %s" % device, flush=True)
                    except (OSError, serial.SerialException):
                        pass
            time.sleep(1)

    def lost(self):
        if not self.up.is_set():
            return
        self.up.clear()
        print("the board went away; looking for it", flush=True)
        try:
            self.s.close()
        except Exception:
            pass

    def read(self, n):
        try:
            if not self.up.is_set():
                return b""
            s = self.s
            s.timeout = None                  # block for arrival, not a 64 KB timed batch
            first = s.read(1)
            waiting = min(s.in_waiting, n - 1) if first else 0
            return first + (s.read(waiting) if waiting else b"")
        except Exception:
            self.lost()
            return b""

    def interrupt_read(self):
        """Wake a streaming reader for shutdown/takeover; no polling timeout."""
        try:
            if self.s is not None:
                self.s.cancel_read()
        except (AttributeError, OSError):
            pass

    def line(self, text):
        if not self.up.is_set():
            return
        try:
            with self.lock:
                self.s.write(text.encode() + b"\r")
        except Exception:
            self.lost()

    def start(self, z, cells=False):
        """Ask for the feed until the board says it began; its 'cols C rows R' line."""
        try:
            return self._start(z, cells)
        except Exception:
            self.lost()
            return None

    def _start(self, z, cells):
        if not self.up.is_set():
            return None
        with self.rpc_lock:                  # an install in progress finishes first
            return self._start_locked(z, cells)

    def _start_locked(self, z, cells):
        s = self.s
        s.timeout = 0.01                 # bounded startup retry reads only
        s.baudrate = self.baud           # a feed left running by a bridge that was killed: stop it at its rate
        time.sleep(0.05)
        self.line("")
        self.line("term off")
        time.sleep(0.4)
        s.baudrate = 115200
        time.sleep(0.05)
        s.reset_input_buffer()
        got, deadline, asked = b"", time.time() + 30, 0.0
        while time.time() < deadline:
            got += s.read(4096)
            i = got.find(b"term: serial begin")
            if i >= 0 and b"\n" in got[i:]:
                begin = got[i:].split(b"\n")[0].decode(errors="replace").strip()
                time.sleep(0.05)
                s.baudrate = self.baud
                time.sleep(0.1)
                self.line("")
                self.line("term redraw")
                return begin.replace("serial begin", "web begin")
            if time.time() - asked > 1.5:        # a board that was just reset is still booting
                self.line("term serial %d ack%s%s" % (self.baud, " z" if z else "", " cells" if cells else ""))
                asked = time.time()
        return None

    def stop(self):
        self.line("term off")
        time.sleep(0.3)
        try:
            if self.up.is_set():
                self.s.baudrate = 115200
        except Exception:
            self.lost()


BOARD = None
DEBUG = bool(os.environ.get("LSWEB_DEBUG"))    # print what the page types and taps
DEBUG_EVERY = float(os.environ.get("LSWEB_DEBUG_EVERY", 5))   # seconds between its throughput lines
CLIENTS = 0                                     # pages connected now (lsdesk waits for none)
WATCHING = None                                 # the session showing the screen: one at a time
LAST_SEEN = time.time()                         # when one last said anything
ACTIVE = set()                                  # the streaming Events of sessions: a pump reads the port
LISTENERS = []                                  # queues that hear every console line
LINES, LINE_CV = [], threading.Condition()      # the console's recent lines, numbered
LINE_SEQ = 0
JOB = {"state": "idle"}                         # the last /app/install


def pump_active():
    return any(e.is_set() for e in list(ACTIVE))


PORT = 8770
STUDIO_OPENED = [0.0]
STUDIO_WINDOW = None                            # set by lsapp.py: open Studio's url in an app window
# The desk window keeps drawing and answering while another window covers it.
KEEP_AWAKE = ["--disable-background-timer-throttling", "--disable-renderer-backgrounding",
              "--disable-backgrounding-occluded-windows"]


def open_studio():
    """The desk's OPEN STUDIO: the page cannot open a window for a board message
    (no click behind it, and app windows block pop-ups), so the bridge does it,
    as its own app window like the desktop's."""
    if time.time() - STUDIO_OPENED[0] < 3:              # one press, one window
        return
    STUDIO_OPENED[0] = time.time()
    import subprocess
    url = "http://127.0.0.1:%d/studio/" % PORT
    if STUDIO_WINDOW:                                   # the desktop app (lsapp.py) opens its own window
        STUDIO_WINDOW(url)
        return
    pf = [os.environ.get(k, "") for k in ("ProgramFiles(x86)", "ProgramFiles", "LOCALAPPDATA")]
    for base in pf:
        for rel in (r"Microsoft\Edge\Application\msedge.exe", r"Google\Chrome\Application\chrome.exe"):
            exe = os.path.join(base, rel)
            if base and os.path.isfile(exe):
                profile = os.path.join(os.environ.get("LOCALAPPDATA", os.path.expanduser("~")), "LakeShark", "window")
                subprocess.Popen([exe, "--app=" + url, "--user-data-dir=" + profile, "--no-first-run",
                                  "--no-default-browser-check", "--window-size=1500,900"] + KEEP_AWAKE)
                return
    import webbrowser
    webbrowser.open(url)


def heard(line):
    """A console line from the board, whoever read it."""
    global LINE_SEQ
    if line.strip() == "studio: open":
        threading.Thread(target=open_studio, daemon=True).start()
    with LINE_CV:
        LINE_SEQ += 1
        LINES.append((LINE_SEQ, line))
        del LINES[:-2000]
        LINE_CV.notify_all()
    for q in list(LISTENERS):
        q.put(line)


def sdk():
    if SDK not in sys.path:
        sys.path.insert(0, os.path.normpath(SDK))
    import lapboard
    return lapboard


class ConsoleSession:
    """A conversation with the board's console that shares the port with the
    screen: while a page's feed runs, its pump reads the lines; otherwise this
    reads the port itself, and a feed that wants to start waits for it."""

    def __init__(self):
        self.q = queue.Queue()
        self.reader = None
        self.open = False

    def __enter__(self):
        if not BOARD or not BOARD.up.is_set():
            raise sdk().BoardError("no board on the bridge (plugged in?)")
        BOARD.rpc_lock.acquire()
        self.open = True
        LISTENERS.append(self.q)
        if not pump_active():
            self._direct()
        return self

    def __exit__(self, *exc):
        self.open = False
        if self.q in LISTENERS:
            LISTENERS.remove(self.q)
        if self.reader:
            self.reader.join(1)
        BOARD.rpc_lock.release()

    def _direct(self):
        if self.reader and self.reader.is_alive():
            return
        split = sdk().Lines()

        def read():
            while self.open and not pump_active():
                try:
                    BOARD.s.timeout = 0.05
                    data = BOARD.s.read(4096)
                except Exception:
                    BOARD.lost()
                    return
                for line in split.feed(data) if data else ():
                    heard(line)
        self.reader = threading.Thread(target=read, daemon=True)
        self.reader.start()

    def send(self, line):
        BOARD.line(line)

    def get(self, timeout):
        end = time.time() + max(0.0, timeout)
        while True:
            try:
                return self.q.get(timeout=max(0.0, min(0.2, end - time.time())))
            except queue.Empty:
                if not pump_active():
                    self._direct()           # the page went away mid-job: read it ourselves
                if time.time() >= end:
                    return None

    def expect(self, pattern, timeout):
        return sdk().Session.expect(self, pattern, timeout)


class LocalLink:
    """lapboard's link interface on this bridge's own port, for Studio mounted here."""
    kind = "bridge"

    def describe(self):
        return "the desktop bridge (%s)" % (BOARD.device if BOARD and BOARD.up.is_set() else "no board")

    def install(self, data, name, progress=None):
        with ConsoleSession() as c:
            c.send("")
            return sdk().do_install(c, data, name, progress)

    def rpc(self, lines, until, timeout=5.0):
        with ConsoleSession() as c:
            return sdk().do_rpc(c, lines, until, timeout)

    def tail(self, on_line, stop):
        seq = LINE_SEQ
        while not stop():
            seq, lines = tail_since(seq, 1.0)
            for line in lines:
                on_line(line)


def tail_since(seq, wait):
    """Console lines after seq, waiting up to `wait` s for one. With no page
    streaming, nobody reads the port: read it for the wait."""
    if not pump_active() and BOARD and BOARD.up.is_set() and BOARD.rpc_lock.acquire(timeout=wait):
        try:
            split = sdk().Lines()
            end = time.time() + wait
            while time.time() < end and not pump_active():
                BOARD.s.timeout = 0.05
                data = BOARD.s.read(4096)
                for line in split.feed(data) if data else ():
                    heard(line)
        except Exception:
            BOARD.lost()
        finally:
            BOARD.rpc_lock.release()
    else:
        with LINE_CV:
            if LINE_SEQ <= seq:
                LINE_CV.wait(wait)
    with LINE_CV:
        return LINE_SEQ, [t for n, t in LINES if n > seq]


def install_job(data, name):
    def progress(sent, total, text):
        JOB.update(sent=sent, total=total, message=text)
    try:
        reply = LocalLink().install(data, name, progress)
        JOB.update(state="done", message="installed as " + name, reply=reply, sent=len(data))
    except Exception as e:
        JOB.update(state="error", message=str(e))


STUDIO = None


def studio():
    """LakeShark Studio, mounted at /studio/ with this bridge's board."""
    global STUDIO
    if STUDIO is None:
        sys.path.insert(0, os.path.normpath(os.path.join(SDK, "studio")))
        import server
        STUDIO = server.Studio(provider=LocalLink, mounted=True)
    return STUDIO


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def local(self, write=False):
        host = (self.headers.get("Host") or "").split(":")[0]
        return host in ("127.0.0.1", "localhost") and (not write or self.headers.get("X-Studio") == "1")

    def send_json(self, obj, code=200):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def studio_route(self, method):
        if self.path == "/studio":
            self.send_response(301)
            self.send_header("Location", "/studio/")
            self.end_headers()
            return True
        if not self.path.startswith("/studio/"):
            return False
        try:
            s = studio()
        except ImportError:
            self.send_error(404, "app-sdk/studio is not next to this bridge")
            return True
        if not s.handle(self, method, self.path[7:]):
            self.send_error(404)
        return True

    def do_POST(self):
        global JOB
        if self.studio_route("POST"):
            return
        if not self.local(write=True):
            return self.send_json({"ok": False, "message": "only from this computer, with X-Studio: 1"}, 403)
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n) if n else b""
        from urllib.parse import parse_qs, urlparse
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        lb = sdk()
        try:
            if u.path == "/board/rpc":
                j = json.loads(body.decode() or "{}")
                with ConsoleSession() as c:
                    lines = lb.do_rpc(c, [str(x)[:200] for x in j.get("lines", [])][:8], j.get("until", "^APP "),
                                      min(float(j.get("timeout", 5)), 30))
                return self.send_json({"ok": True, "lines": lines})
            if u.path == "/app/install":
                name = q.get("name", "")
                if JOB.get("state") == "running":
                    return self.send_json({"ok": False, "message": "an install is already running"})
                if not BOARD or not BOARD.up.is_set():
                    return self.send_json({"ok": False, "message": "no board on the bridge (plugged in?)"})
                JOB = {"state": "running", "sent": 0, "total": len(body), "message": "starting", "name": name}
                threading.Thread(target=install_job, args=(body, name), daemon=True).start()
                return self.send_json({"ok": True})
        except (lb.BoardError, ValueError) as e:
            return self.send_json({"ok": False, "message": str(e)})
        self.send_error(404)

    def do_GET(self):
        if self.studio_route("GET"):
            return
        if self.path.startswith(("/board/", "/app/")):
            if not self.local():
                return self.send_json({"ok": False}, 403)
            from urllib.parse import parse_qs, urlparse
            u = urlparse(self.path)
            q = {k: v[0] for k, v in parse_qs(u.query).items()}
            if u.path == "/app/job":
                return self.send_json(JOB)
            if u.path == "/board/tail":
                since = int(q["since"]) if "since" in q else LINE_SEQ
                seq, lines = tail_since(since, min(float(q.get("wait", 2)), 10))
                return self.send_json({"seq": seq, "lines": lines})
            return self.send_error(404)
        if self.path.startswith("/ws"):
            return self.socket_session()
        if self.path == "/status":                    # for a launcher or a script: is it connected
            import json
            body = json.dumps({"board": BOARD.device if BOARD and BOARD.up.is_set() else None,
                               "pages": CLIENTS, "watching": WATCHING is not None}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if DEBUG and self.path.startswith("/cmd?"):   # LSWEB_DEBUG: a console line, e.g. /cmd?tui%20cost
            from urllib.parse import unquote
            BOARD.line(unquote(self.path[5:]))
            self.send_response(204)
            self.end_headers()
            return
        name = "screen.html" if self.path in ("/", "/index.html") else self.path.lstrip("/").split("?")[0]
        path = os.path.normpath(os.path.join(WEB, name))
        if not path.startswith(os.path.normpath(WEB)) or not os.path.isfile(path):
            self.send_error(404)
            return
        body = open(path, "rb").read()
        if name == "screen.html":       # no password over USB: the page starts signed in
            body = body.replace(b'<script src="screen.js">',
                                b"<script>localStorage.setItem('lakeshark-token','usb')</script>"
                                b'<script src="screen.js">', 1)
        self.send_response(200)
        self.send_header("Content-Type", TYPES.get(os.path.splitext(path)[1], "application/octet-stream"))
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def socket_session(self):
        key = self.headers.get("Sec-WebSocket-Key", "")
        accept = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
        self.send_response(101)
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept)
        self.end_headers()
        sock = self.connection
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        global CLIENTS, LAST_SEEN, WATCHING
        CLIENTS += 1
        LAST_SEEN = time.time()
        streaming = threading.Event()
        ACTIVE.add(streaming)
        gone = threading.Event()
        taken = threading.Event()                # another page started watching

        send_lock = threading.Lock()

        def socket_send(data, text=False):
            # The reader and command/reconnect thread share one framed socket.
            with send_lock:
                ws_send(sock, data, text=text)

        def pump():                              # the board's bytes to the page while the screen runs
            updates, nbytes, since = 0, 0, time.time()
            split = Split()
            while not gone.is_set():
                if DEBUG and time.time() - since >= DEBUG_EVERY:
                    dt = time.time() - since
                    print("%.1f updates/s, %.0f KB/s" % (updates / dt, nbytes / dt / 1024), flush=True)
                    updates, nbytes, since = 0, 0, time.time()
                if taken.is_set():                       # the newer page has the board now
                    try:
                        socket_send(b"term: web taken", text=True)
                        sock.shutdown(socket.SHUT_RDWR)
                    except OSError:
                        pass
                    gone.set()
                    break
                if not streaming.is_set():
                    time.sleep(0.02)
                    continue
                if not BOARD.up.is_set():                # unplugged: the page shows it and comes back
                    gone.set()
                    try:
                        sock.shutdown(socket.SHUT_RDWR)
                    except OSError:
                        pass
                    break
                # no lock: a read waiting for bytes must not hold up the acks
                # going the other way, or the board waits on them
                data = BOARD.read(65536)
                if data and DEBUG:                      # console replies between the frames
                    for line in data.split(b"\n"):
                        if b"tui:" in line or b"fps=" in line or b"[app " in line:
                            print(line.decode(errors="replace").strip()[-160:], flush=True)
                if data:
                    updates += data.count(b"\x1e0000\x1fffff")     # the empty frame that ends each update
                    nbytes += len(data)
                    try:
                        _, lines = split.feed(data, on_frame=lambda frame: socket_send(frame))
                        for line in lines:               # the board's replies, for the settings panel
                            heard(line)
                            socket_send(("console " + line[:300]).encode(), text=True)
                    except OSError:
                        gone.set()
        threading.Thread(target=pump, daemon=True).start()

        # A window behind another (Studio) has its timers throttled to once a
        # minute, and the board ends a feed it hears nothing from for 10 s: back
        # at 115200 with this still reading at the feed's rate, so the screen and
        # the console both stop for good. While the page is here, ping for it.
        def keep_alive():
            while not gone.wait(2.0):
                if streaming.is_set() and not taken.is_set():
                    BOARD.line("term ping")
        threading.Thread(target=keep_alive, daemon=True).start()
        try:
            while True:
                m = ws_recv(sock)
                if not m or m[0] == 8:
                    break
                LAST_SEEN = time.time()
                if m[0] != 1:
                    continue
                text = m[1].decode(errors="replace")
                if text.startswith(("token ", "login ")):
                    socket_send(b"ok local", text=True)
                elif text.startswith("term web ack"):
                    if WATCHING is not None and WATCHING is not taken:
                        WATCHING.set()                   # the page watching till now steps aside
                        BOARD.interrupt_read()
                        time.sleep(0.1)
                    WATCHING = taken
                    streaming.clear()
                    words = text.split()
                    begin = BOARD.start("z" in words, "cells" in words)
                    if not begin:                        # no board yet: the page tries again on its own
                        break
                    if taken.is_set():                   # another page took over while this one started
                        break
                    try:
                        socket_send(begin.encode(), text=True)
                    except OSError:
                        break
                    streaming.set()
                elif text.startswith("cmd "):            # the settings panel: a console line, over USB only
                    BOARD.line(text[4:].replace("\r", " ").replace("\n", " ")[:200])
                elif text.startswith("term "):
                    if DEBUG and text.startswith("term in "):
                        print("in", bytes.fromhex(text[8:]), flush=True)
                    BOARD.line(text)
        finally:
            gone.set()
            ACTIVE.discard(streaming)
            BOARD.interrupt_read()
            if WATCHING is taken:
                WATCHING = None
            if taken.is_set():
                streaming.clear()                        # the newer page runs the feed: leave it on
            CLIENTS -= 1
            LAST_SEEN = time.time()
            if streaming.is_set():
                BOARD.stop()


def serve(find, port=8770, baud=2000000):
    """Start the bridge for the board `find()` names; returns the server (call serve_forever)."""
    global BOARD, PORT
    BOARD = Board(find, baud)
    PORT = port
    return ThreadingHTTPServer(("127.0.0.1", port), Handler)


def main():
    args = sys.argv[1:]
    if "--host" in args:
        # Ethernet needs no PC bridge. Reuse the launcher validation/config.
        from lsdesk import arguments, board_url, desktop_host, load, open_window
        opts = arguments(args)
        host = desktop_host(load(), opts.host)
        open_window(board_url(host))
        return
    port = int(args[args.index("--port") + 1]) if "--port" in args else 8770
    baud = int(args[args.index("--baud") + 1]) if "--baud" in args else 2000000
    com = next((a for a in args if a.upper().startswith("COM") or a.startswith("/dev/")), None)
    if not com:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from lsterm import only_board
        com = only_board()
    sn = usb_serial(com)
    server = serve(lambda: port_of(sn) or com, port, baud)
    print("the board on %s: open http://127.0.0.1:%d/" % (com, port), flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
