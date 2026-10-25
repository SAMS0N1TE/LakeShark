"""The board's web screen in a browser on this computer, over USB.

    python lsweb.py [COMx] [--port 8770] [--baud 2000000]

Serves the board's own page (components/apps/tui/web) on 127.0.0.1 and
carries its WebSocket to the board's USB console: the page asks for
'term web ack z', this runs 'term serial <baud> ack z' on the port and
passes the frames through; 'term ack', 'term in <hex>' and 'term ping'
go back as console lines. Nothing listens beyond this computer, so there
is no password. Needs pyserial.
"""
import base64
import hashlib
import os
import socket
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

WEB = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "components", "apps", "tui", "web")
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

    def feed(self, data):
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
            frames += self.buf[:10 + n]
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
            return self.s.read(n) if self.up.is_set() else b""
        except Exception:
            self.lost()
            return b""

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
        s = self.s
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
CLIENTS = 0                                     # pages connected now (lsdesk waits for none)
WATCHING = None                                 # the session showing the screen: one at a time
LAST_SEEN = time.time()                         # when one last said anything


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
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
        gone = threading.Event()
        taken = threading.Event()                # another page started watching

        def pump():                              # the board's bytes to the page while the screen runs
            updates, nbytes, since = 0, 0, time.time()
            split = Split()
            while not gone.is_set():
                if DEBUG and time.time() - since >= 5:
                    dt = time.time() - since
                    print("%.1f updates/s, %.0f KB/s" % (updates / dt, nbytes / dt / 1024), flush=True)
                    updates, nbytes, since = 0, 0, time.time()
                if taken.is_set():                       # the newer page has the board now
                    try:
                        ws_send(sock, b"term: web taken", text=True)
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
                    frames, lines = split.feed(data)
                    try:
                        if frames:
                            ws_send(sock, frames)
                        for line in lines:               # the board's replies, for the settings panel
                            ws_send(sock, ("console " + line[:300]).encode(), text=True)
                    except OSError:
                        gone.set()
        threading.Thread(target=pump, daemon=True).start()
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
                    ws_send(sock, b"ok local", text=True)
                elif text.startswith("term web ack"):
                    if WATCHING is not None and WATCHING is not taken:
                        WATCHING.set()                   # the page watching till now steps aside
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
                        ws_send(sock, begin.encode(), text=True)
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
    global BOARD
    BOARD = Board(find, baud)
    return ThreadingHTTPServer(("127.0.0.1", port), Handler)


def main():
    args = sys.argv[1:]
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
