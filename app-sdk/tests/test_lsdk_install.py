"""lsdk install against a fake board, with the real framing and no hardware.

    python app-sdk/tests/test_lsdk_install.py

The fake is the serial port itself: a pyserial stand-in whose far end answers
as the board's console does (components/apps/tui/ls_lap_transfer.c and
ls_lap_console.c: app put/data/end/list/info/rm, `desk open`, the prompt
echo, and `term serial` starting a screen feed of 0x1E frames). It is put
behind the real desktop bridge (host/lsterm/lsweb.py), and lsdk is run as a
program against that bridge:

  1. the bridge idle (no page): lsdk install reads the port through the bridge;
  2. a page watching the screen: the install shares the port with the feed,
     and the page still gets its frames and the board's replies;
  3. lsdk list, info and rm through the bridge;
  4. straight over "serial" (lapboard.SerialLink), no bridge;
  5. a corrupted chunk: the board's SHA-256 check refuses it;
  6. Studio mounted at /studio/ answers, and refuses a request without X-Studio.

Needs pyserial (its module is replaced, not opened) and the RISC-V toolchain
for the app it installs. Exit 0 when all pass.
"""
import base64
import hashlib
import json
import os
import re
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
SDK = HERE.parent
REPO = SDK.parent
sys.path.insert(0, str(SDK))
sys.path.insert(0, str(REPO / "host" / "lsterm"))


# ------------------------------------------------------------- the fake board

class FakeBoard:
    """The board's console behind the port: lines in (CR), replies out."""

    def __init__(self):
        self.apps = {}                  # name -> bytes
        self.ids = {}                   # id -> name
        self.up = None                  # the upload in progress
        self.corrupt = False
        self.feed = False
        self.out = bytearray()
        self.cv = threading.Condition()
        self.lines_in = []
        threading.Thread(target=self.screen, daemon=True).start()

    def emit(self, data):
        with self.cv:
            self.out += data
            self.cv.notify_all()

    def say(self, text):
        self.emit(text.encode() + b"\r\n")

    def screen(self):
        """While a feed runs: an update of two frames every 40 ms, as ls_rterm sends."""
        n = 0
        while True:
            time.sleep(0.04)
            if not self.feed:
                continue
            n += 1
            body = b"C" + struct.pack("<HH", 0, 1) + struct.pack("<HB", 65 + n % 26, 7)
            self.emit(frame(body) + frame(b""))

    def line(self, text):
        self.lines_in.append(text)
        if text:
            self.say("lakeshark> " + text)          # the console echoes what it ran
        w = text.split()
        if not w:
            return
        if w[0] == "term":
            if len(w) > 1 and w[1] == "serial":
                self.say("term: serial begin cols 80 rows 30 cells")
                self.feed = True
            elif len(w) > 1 and w[1] == "off":
                self.feed = False
            return
        if w[0] == "desk" and len(w) >= 3 and w[1] == "open":
            self.say("desk: open " + w[2])
            return
        if w[0] != "app":
            self.say("Unrecognized command")
            return
        self.app(w)

    def app(self, w):
        cmd = w[1] if len(w) > 1 else ""
        if cmd == "put":
            if len(w) != 5 or not re.fullmatch(r"[\w.-]+\.lap", w[2]) or len(w[4]) != 64:
                return self.say("APP ERR args invalid upload arguments")
            size = int(w[3])
            if size < 52 or size > 524288:
                return self.say("APP ERR size app must be 52..524288 bytes")
            self.up = {"name": w[2], "size": size, "sha": w[4], "data": bytearray(), "seq": 0}
            return self.say("APP OK put chunk=72 next=0")
        if cmd in ("data", "end") and not self.up:
            return self.say("APP ERR state no upload in progress")
        if cmd == "data":
            if len(w) != 4 or int(w[2]) != self.up["seq"]:
                self.up = None
                return self.say("APP ERR seq sequence must be contiguous from zero")
            chunk = base64.b64decode(w[3])
            if len(w[3]) > 96 or len(chunk) > 72:
                self.up = None
                return self.say("APP ERR base64 invalid base64 chunk")
            if self.corrupt and self.up["seq"] == 3:
                chunk = bytes([chunk[0] ^ 0xFF]) + chunk[1:]
            self.up["data"] += chunk
            self.up["seq"] += 1
            return self.say("APP OK data next=%d received=%d" % (self.up["seq"], len(self.up["data"])))
        if cmd == "end":
            up, self.up = self.up, None
            if len(up["data"]) != up["size"]:
                return self.say("APP ERR size upload size mismatch")
            if hashlib.sha256(up["data"]).hexdigest() != up["sha"]:
                return self.say("APP ERR verify SHA256 mismatch")
            import lap
            m = lap.Elf(bytes(up["data"])).manifest
            self.apps[up["name"]] = bytes(up["data"])
            self.ids[m["id"]] = up["name"]
            return self.say("APP OK end " + up["name"])
        if cmd == "list":
            for app_id in self.ids:
                self.info_line(app_id)
            return self.say("APP OK list end")
        if cmd == "info" and len(w) == 3:
            return self.info_line(w[2]) if w[2] in self.ids else self.say("APP ERR absent unknown app id")
        if cmd == "rm" and len(w) >= 3:
            if w[2] not in self.ids:
                return self.say("APP ERR absent unknown app id")
            del self.apps[self.ids.pop(w[2])]
            return self.say("APP OK rm refresh=queued")
        self.say("APP ERR args use list|put|data|end|rm|info|rx|svc|stop")

    def info_line(self, app_id):
        import lap
        data = self.apps[self.ids[app_id]]
        m = lap.Elf(data).manifest
        enc = lambda s: re.sub(r"[^A-Za-z0-9_.-]", lambda c: "%%%02X" % ord(c.group()), s)
        self.say("APP OK app id=%s name=%s version=%s size=%d signed=no sha256=%s" % (
            app_id, enc(m["name"]), enc(m["version"]), len(data), hashlib.sha256(data).hexdigest()[:16]))


def frame(body):
    crc = 0xFFFF
    for b in body:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return b"\x1e%04x" % len(body) + body + b"\x1f%04x" % crc


class FakeSerial:
    """Enough of pyserial.Serial for lsweb.Board and lapboard.SerialSession."""
    board = None

    def __init__(self, *a, **k):
        self.port = None
        self.baudrate = 115200
        self.timeout = None
        self.dtr = self.rts = False
        self.is_open = False
        self.inbuf = b""
        self.cancelled = False

    def open(self):
        self.is_open = True

    def close(self):
        self.is_open = False

    def set_buffer_size(self, **k):
        pass

    def reset_input_buffer(self):
        with self.board.cv:
            self.board.out.clear()

    def cancel_read(self):
        with self.board.cv:
            self.cancelled = True
            self.board.cv.notify_all()

    @property
    def in_waiting(self):
        return len(self.board.out)

    def read(self, n=1):
        b = self.board
        end = None if self.timeout is None else time.time() + self.timeout
        with b.cv:
            while not b.out and not self.cancelled:
                left = None if end is None else end - time.time()
                if left is not None and left <= 0:
                    return b""
                b.cv.wait(left if left is not None else 0.5)
            self.cancelled = False
            got = bytes(b.out[:n])
            del b.out[:n]
            return got

    def write(self, data):
        self.inbuf += data
        while b"\r" in self.inbuf:
            line, _, self.inbuf = self.inbuf.partition(b"\r")
            self.board.line(line.decode(errors="replace").strip())
        return len(data)


# ------------------------------------------------------------------ helpers

FAILS = []


def check(what, ok, detail=""):
    print(("PASS " if ok else "FAIL ") + what + ("" if ok else "  " + str(detail)[-600:]), flush=True)
    if not ok:
        FAILS.append(what)


def lsdk(*args, port):
    env = dict(os.environ, LSDK_BRIDGE_PORT=str(port))
    r = subprocess.run([sys.executable, str(SDK / "lsdk.py"), *args], capture_output=True, text=True, env=env, timeout=120)
    return r.returncode, r.stdout + r.stderr


def ws_client(port):
    s = socket.create_connection(("127.0.0.1", port))
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall(("GET /ws HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % (port, key)).encode())
    head = b""
    while b"\r\n\r\n" not in head:
        head += s.recv(1)
    return s


def ws_say(s, text):
    data = text.encode()
    mask = os.urandom(4)
    s.sendall(bytes([0x81, 0x80 | len(data)]) + mask + bytes(c ^ mask[i % 4] for i, c in enumerate(data)))


def ws_reader(s, got):
    import lsweb
    while True:
        try:
            m = lsweb.ws_recv(s)
        except OSError:
            return
        if not m:
            return
        got.append(m)


# ------------------------------------------------------------------ the test

def main():
    import serial
    import lapboard
    import lsdk as sdk
    board = FakeBoard()
    FakeSerial.board = board
    serial.Serial = FakeSerial                         # what lsweb.Board opens

    work = Path(tempfile.mkdtemp(prefix="lsdk-install-"))
    try:
        app = sdk.new_app("demo", "blank", work)
        res = sdk.build(app)
    except Exception as e:
        print("SKIP: cannot build an app here (%s)" % e)
        return 0
    if not res.ok:
        print("SKIP: the build failed:", res.problems)
        return 0
    data = Path(res.lap).read_bytes()

    import lsweb
    srv = lsweb.serve(lambda: "FAKE", 0, 2000000)
    port = srv.server_address[1]
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    end = time.time() + 5
    while not lsweb.BOARD.up.is_set() and time.time() < end:
        time.sleep(0.05)
    check("the bridge opened the fake port", lsweb.BOARD.up.is_set())

    # 1. the bridge idle
    t0 = time.time()
    rc, out = lsdk("install", str(app), port=port)
    check("lsdk install through the idle bridge", rc == 0 and board.apps.get("demo.lap") == data, out)
    check("  the bridge said so", "APP OK end demo.lap" in out and "the desktop bridge" in out, out)
    print("     %d bytes in %.1f s" % (len(data), time.time() - t0))

    # 2. a page watching the screen
    s = ws_client(port)
    got = []
    threading.Thread(target=ws_reader, args=(s, got), daemon=True).start()
    ws_say(s, "token usb")
    ws_say(s, "term web ack z cells")
    end = time.time() + 10
    while not any(m[0] == 1 and m[1].startswith(b"term: web begin") for m in got) and time.time() < end:
        time.sleep(0.05)
    check("a page started the screen feed", board.feed, [m[1][:40] for m in got[:5]])
    time.sleep(0.3)
    frames_before = sum(1 for m in got if m[0] == 2)
    board.apps.clear()
    board.ids.clear()
    rc, out = lsdk("install", str(app), "--open", port=port)
    frames_after = sum(1 for m in got if m[0] == 2)
    check("lsdk install while the page streams", rc == 0 and board.apps.get("demo.lap") == data, out)
    check("  and --open opened it in a desk window", "desk: open demo" in out, out)
    check("  the page kept getting frames", frames_after > frames_before + 5, (frames_before, frames_after))
    check("  the page saw the board's replies", any(m[0] == 1 and b"console APP OK end demo.lap" in m[1] for m in got))

    # 3. list, info, rm through the bridge
    rc, out = lsdk("list", port=port)
    check("lsdk list", rc == 0 and "demo" in out and "DEMO" in out, out)
    rc, out = lsdk("info", "--board", "demo", port=port)
    check("lsdk info --board", rc == 0 and '"id": "demo"' in out, out)
    rc, out = lsdk("rm", "demo", port=port)
    check("lsdk rm", rc == 0 and "APP OK rm" in out and "demo" not in board.ids, out)
    rc, out = lsdk("info", "--board", "demo", port=port)
    check("lsdk info on a removed app fails plainly", rc == 1 and "APP ERR absent" in out, out)

    # 3b. lsdk logs: the app's console lines, through the bridge
    env = dict(os.environ, LSDK_BRIDGE_PORT=str(port))
    logs = subprocess.Popen([sys.executable, str(SDK / "lsdk.py"), "logs", "demo"], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, env=env)
    time.sleep(2.5)
    for k in range(3):
        board.say("[app demo] hello number %d" % (k + 1))
        board.say("[app other] not mine")
        time.sleep(0.3)
    time.sleep(2.5)
    logs.terminate()
    said = logs.communicate(timeout=10)[0]
    check("lsdk logs shows the app's lines", "[app demo] hello number 3" in said and "not mine" not in said, said)

    # 6. Studio, mounted on the bridge
    r = urllib.request.urlopen("http://127.0.0.1:%d/studio/" % port, timeout=10)
    check("Studio is served at /studio/", r.status == 200 and b"LakeShark Studio" in r.read())
    try:
        urllib.request.urlopen(urllib.request.Request("http://127.0.0.1:%d/studio/api/stop" % port, data=b"{}"), timeout=5)
        check("Studio refuses a request without X-Studio", False, "accepted")
    except urllib.error.HTTPError as e:
        check("Studio refuses a request without X-Studio", e.code == 403, e.code)
    try:
        urllib.request.urlopen(urllib.request.Request("http://127.0.0.1:%d/app/install?name=x.lap" % port, data=b"x"), timeout=5)
        check("the bridge refuses an install without X-Studio", False, "accepted")
    except urllib.error.HTTPError as e:
        check("the bridge refuses an install without X-Studio", e.code == 403, e.code)
    s.sendall(bytes([0x88, 0x80]) + os.urandom(4))   # a close frame, as a browser leaves
    time.sleep(0.5)
    s.close()
    srv.shutdown()
    board.feed = False

    # 4. straight over serial
    board.apps.clear()
    board.ids.clear()
    seen = []
    lk = lapboard.SerialLink("FAKE", opener=lambda port: _opened())
    name, manifest, reply = lapboard.install(lk, data, lambda a, b, t: seen.append(a))
    check("install over serial (no bridge)", board.apps.get("demo.lap") == data and reply == "APP OK end demo.lap", reply)
    check("  progress went up to the whole file", seen and seen[-1] == len(data) and seen == sorted(seen))

    # 5. a corrupted chunk
    board.corrupt = True
    try:
        lapboard.install(lk, data)
        check("the board refuses a corrupted upload", False, "accepted")
    except lapboard.BoardError as e:
        check("the board refuses a corrupted upload", "SHA256 mismatch" in str(e), e)
    board.corrupt = False

    print("FAIL: %d" % len(FAILS) if FAILS else "PASS: all")
    return 1 if FAILS else 0


def _opened():
    s = FakeSerial()
    s.timeout = 0.05
    return s


if __name__ == "__main__":
    sys.exit(main())
