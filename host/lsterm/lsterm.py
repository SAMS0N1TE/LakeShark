"""The LakeShark screen in this terminal: keys, mouse clicks and all.

  python lsterm.py                       the one board on USB
  python lsterm.py COM5                 a board on USB (or --serial <USB serial number>)
  python lsterm.py --install-profile     a "LakeShark" Windows Terminal profile with the right font
  python lsterm.py tcp://192.168.1.50    a board on Wi-Fi with 'term net on' (asks its password)
  python lsterm.py --sim [app]           no board: the firmware's screens in the simulator
  --plain                                the board's screen alone: no frame or panel (lspanel.py)

Ctrl-] leaves. Ctrl-L repaints. Ctrl-P snapshot, Ctrl-R record, Ctrl-T console,
Ctrl-F files, Ctrl-G help (the panel: lspanel.py). The board draws everything; this program
only carries bytes, so it works the same inside an SSH session.

Use Windows Terminal (or any terminal with 24-bit colour). Fine pixel art
comes as 2x2 quadrant blocks, which every font has; --sextants sends the
board's 2x3 sextants instead, for a font that has them (a missing one
shows as a box).
The window needs at least the board's grid: 115 x 27 for the default font.

Needs pyserial for USB: pip install pyserial.
"""
import ctypes
import os
import queue
import socket
import sys
import threading
import time

LEAVE = b"\x1b[?1006l\x1b[?1003l\x1b[?1000l\x1b[0m\x1b[?7h\x1b[?25h\x1b[?1049l"
QUIT = 0x1D

# Windows keys that come as a virtual-key code with no character.
_VK_SEQ = {
    0x26: b"\x1b[A", 0x28: b"\x1b[B", 0x27: b"\x1b[C", 0x25: b"\x1b[D",
    0x24: b"\x1b[H", 0x23: b"\x1b[F", 0x21: b"\x1b[5~", 0x22: b"\x1b[6~",
    0x2D: b"\x1b[2~", 0x2E: b"\x1b[3~",
    0x70: b"\x1bOP", 0x71: b"\x1bOQ", 0x72: b"\x1bOR", 0x73: b"\x1bOS",
    0x74: b"\x1b[15~", 0x75: b"\x1b[17~", 0x76: b"\x1b[18~", 0x77: b"\x1b[19~",
    0x78: b"\x1b[20~", 0x79: b"\x1b[21~", 0x7A: b"\x1b[23~", 0x7B: b"\x1b[24~",
}


def _sextant_to_quadrant():
    """U+1FB00..1FB3B (2x3 sextants) as the nearest 2x2 quadrant blocks, the
    same fold the board's ls_vt_resolve does. Many fonts have the blocks but
    not the sextants, and show a box for each missing one."""
    quad = " ▘▝▀▖▌▞▛▗▚▐▜▄▙▟█"
    table = {}
    for i in range(60):
        bits = i + 1                    # 21 and 42 are U+258C and U+2590, not sextants
        bits += bits >= 21
        bits += bits >= 42
        q = (1 if bits & 5 else 0) | (2 if bits & 10 else 0) | (4 if bits & 16 else 0) | (8 if bits & 32 else 0)
        table[chr(0x1FB00 + i).encode()] = quad[q].encode()
    import re
    pattern = re.compile(rb"\xf0\x9f[\xac\xad][\x80-\xbf]")
    return lambda b: pattern.sub(lambda m: table.get(m.group(0), m.group(0)), b)


FOLD = _sextant_to_quadrant()            # off with --sextants
PLAIN = False                            # --plain: the board's screen alone, no frame or panel


class _Key(ctypes.Structure):
    _fields_ = [("down", ctypes.c_int32), ("repeat", ctypes.c_uint16), ("vk", ctypes.c_uint16),
                ("scan", ctypes.c_uint16), ("ch", ctypes.c_wchar), ("state", ctypes.c_uint32)]


class _Mouse(ctypes.Structure):
    _fields_ = [("x", ctypes.c_int16), ("y", ctypes.c_int16), ("buttons", ctypes.c_uint32),
                ("state", ctypes.c_uint32), ("flags", ctypes.c_uint32)]


class _Event(ctypes.Union):
    _fields_ = [("key", _Key), ("mouse", _Mouse), ("pad", ctypes.c_byte * 16)]


class _InputRecord(ctypes.Structure):
    _fields_ = [("type", ctypes.c_uint16), ("u", _Event)]


class _ScreenInfo(ctypes.Structure):
    _fields_ = [("size", ctypes.c_int16 * 2), ("cursor", ctypes.c_int16 * 2), ("attr", ctypes.c_uint16),
                ("left", ctypes.c_int16), ("top", ctypes.c_int16), ("right", ctypes.c_int16),
                ("bottom", ctypes.c_int16), ("max", ctypes.c_int16 * 2)]


class Terminal:
    """Raw keyboard in, bytes out, the way the board expects them."""

    def __init__(self):
        self.keys = queue.Queue()
        self.out = sys.stdout.buffer
        self._resized = 0.0
        if os.name == "nt":
            import ctypes
            k32 = ctypes.windll.kernel32
            self._k32 = k32
            self._hin, self._hout = k32.GetStdHandle(-10), k32.GetStdHandle(-11)
            self._in_mode, self._out_mode = ctypes.c_uint32(), ctypes.c_uint32()
            k32.GetConsoleMode(self._hin, ctypes.byref(self._in_mode))
            k32.GetConsoleMode(self._hout, ctypes.byref(self._out_mode))
            # VT input and mouse records with no line editing, echo, Ctrl-C or
            # quick edit; VT output; UTF-8.
            k32.SetConsoleMode(self._hin, 0x0200 | 0x0080 | 0x0010 | 0x0008)
            k32.SetConsoleMode(self._hout, self._out_mode.value | 0x0001 | 0x0004 | 0x0008)
            self._cp = k32.GetConsoleOutputCP()
            k32.SetConsoleOutputCP(65001)
            self._buttons = 0
            self._at = None

            # Read the console's records, not msvcrt.getwch: that turns an arrow
            # into "\xe0M", which the board takes as the letter M, and it drops
            # the mouse. Records with a character carry whatever the terminal
            # sent (including its own VT sequences); the rest are made into
            # the sequences a terminal would have sent.
            def pump():
                recs, n = (_InputRecord * 64)(), ctypes.c_uint32()
                while k32.ReadConsoleInputW(self._hin, recs, 64, ctypes.byref(n)):
                    got = b"".join(self._record(r) for r in recs[:n.value])
                    if got:
                        self.keys.put(got)
        else:
            import termios
            import tty
            self._fd = sys.stdin.fileno()
            self._saved = termios.tcgetattr(self._fd)
            tty.setraw(self._fd)
            import signal
            signal.signal(signal.SIGWINCH, lambda *_: setattr(self, "_resized", time.time()))

            def pump():
                while True:
                    b = os.read(self._fd, 256)
                    if not b:
                        return
                    self.keys.put(b)
        threading.Thread(target=pump, daemon=True).start()

    def _record(self, r):
        """One console input record as the bytes a VT terminal sends for it."""
        if r.type == 1:
            k = r.u.key
            if not k.down:
                return b""
            if k.ch != "\0":
                return k.ch.encode("utf-8", "replace") * max(1, k.repeat)
            return _VK_SEQ.get(k.vk, b"") * max(1, k.repeat)
        if r.type == 4:                                  # the window changed size
            self._resized = time.time()
        if r.type != 2:
            return b""
        m = r.u.mouse
        info = _ScreenInfo()
        self._k32.GetConsoleScreenBufferInfo(self._hout, ctypes.byref(info))
        x, y = m.x - info.left + 1, m.y - info.top + 1
        if m.flags & 4:                                  # wheel: up is a positive delta
            return b"\x1b[<%d;%d;%dM" % (65 if m.buttons & 0x80000000 else 64, x, y)
        if m.flags & 1 and not m.buttons & 1:            # moved, no button: once a cell
            if (x, y) == self._at:
                return b""
            self._at = (x, y)
            return b"\x1b[<35;%d;%dM" % (x, y)
        was, now = self._buttons & 1, m.buttons & 1
        self._buttons = m.buttons
        if now and not was:
            return b"\x1b[<0;%d;%dM" % (x, y)
        if was and not now:
            return b"\x1b[<0;%d;%dm" % (x, y)
        return b""

    def read(self):
        """Every key byte waiting, without blocking. A resize loses what the
        terminal had drawn, and the board only sends changed cells, so once
        the window has settled this asks for the whole screen (Ctrl-L)."""
        got = bytearray()
        if self._resized and time.time() - self._resized > 0.3:
            self._resized = 0.0
            got += b"\x0c"
        while True:
            try:
                got += self.keys.get_nowait()
            except queue.Empty:
                return bytes(got)

    def write(self, b):
        if FOLD:
            b = FOLD(b)
        self.raw(b)

    def raw(self, b):
        self.out.write(b)
        self.out.flush()

    def enter(self):
        """The frame's screen: its own, mouse motion too (the panel lights what it is over)."""
        self.raw(b"\x1b[?1049h\x1b[?25l\x1b[?7l\x1b[?1000h\x1b[?1003h\x1b[?1006h\x1b]0;LakeShark\x07\x1b[0m\x1b[2J")

    def restore(self):
        self.write(LEAVE)
        if os.name == "nt":
            self._k32.SetConsoleMode(self._hin, self._in_mode.value)
            self._k32.SetConsoleMode(self._hout, self._out_mode.value)
            self._k32.SetConsoleOutputCP(self._cp)
        else:
            import termios
            termios.tcsetattr(self._fd, termios.TCSADRAIN, self._saved)


def crc16(b):
    c = 0xFFFF
    for x in b:
        c ^= x << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


class Frames:
    """Pull the board's frames out of the console stream. The rest is the
    console's own text (command replies), kept for lines()."""

    def __init__(self):
        self.buf = bytearray()
        self.text = bytearray()
        self.bad = 0

    def lines(self):
        """Whole lines of console text since the last call."""
        *done, rest = bytes(self.text).split(b"\n")
        self.text = bytearray(rest[-4096:])
        return [l.decode("utf-8", "replace").strip("\r") for l in done]

    def feed(self, data):
        self.buf += data
        out = []
        while True:
            i = self.buf.find(0x1E)
            if i < 0:
                self.text += self.buf
                self.buf.clear()
                return out
            self.text += self.buf[:i]
            del self.buf[:i]
            if len(self.buf) < 5:
                return out
            try:
                n = int(self.buf[1:5], 16)
            except ValueError:
                del self.buf[:1]
                continue
            if len(self.buf) < 5 + n + 5:
                return out
            body, tail = bytes(self.buf[5:5 + n]), self.buf[5 + n:10 + n]
            try:
                good = tail[:1] == b"\x1f" and int(tail[1:5], 16) == crc16(body)
            except ValueError:
                good = False
            if good:
                out.append(body)
                del self.buf[:10 + n]
            else:
                self.bad += 1
                del self.buf[:1]


def find_port(serial_number):
    from serial.tools import list_ports
    for p in list_ports.comports():
        if p.serial_number == serial_number:
            return p.device
    sys.exit("no board with USB serial %s" % serial_number)


def only_board():
    """The one USB serial port that looks like a LakeShark board (CH343 or CP210x/CH340 bridges)."""
    from serial.tools import list_ports
    found = [p.device for p in list_ports.comports() if p.vid in (0x1A86, 0x10C4, 0x303A)]
    if len(found) == 1:
        return found[0]
    sys.exit("found %s; name one: lsterm.py COMx" % (", ".join(found) if found else "no board on USB"))


def install_profile():
    """A Windows Terminal profile of its own (a fragment: your own settings are not touched)."""
    import json
    here = os.path.abspath(__file__)
    folder = os.path.join(os.environ["LOCALAPPDATA"], "Microsoft", "Windows Terminal", "Fragments", "LakeShark")
    os.makedirs(folder, exist_ok=True)
    profile = {"profiles": [{
        "name": "LakeShark",
        "commandline": '"%s" "%s"' % (sys.executable, here),
        "font": {"face": "Cascadia Mono"},     # the copy Windows Terminal ships has the sextants
        "colorScheme": "Campbell",
        "closeOnExit": "graceful",
    }]}
    path = os.path.join(folder, "lakeshark.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(profile, f, indent=2)
    print("added the LakeShark profile to Windows Terminal (%s); open a new tab from it" % path)


def start_feed(s, baud):
    """Ask for the screen feed until the board says it began. Opening the port
    can reset the board (a CH343 on Windows does even with DTR and RTS low),
    so one seen booting gets until its 'term' command is registered."""
    s.write(b"\r")
    got, deadline, asked, booting = b"", time.time() + 4, 0.0, False
    while time.time() < deadline:
        got += s.read(4096)
        if b"term: serial begin" in got:
            return got[got.find(b"term: serial begin"):].split(b"\n")[0].decode(errors="replace")
        if not booting and any(m in got for m in (b"ESP-ROM", b"rst:", b"boot: ")):
            booting, deadline = True, time.time() + 30
            print("the board is starting up...")
        if time.time() - asked > 1.5:
            if asked and not booting and b"Unrecognized command" in got:
                break
            s.write(b"term serial %d ack\r" % baud)    # ack: the board waits for this terminal to draw
            asked = time.time()
    sys.exit("the board did not start a terminal (is it running the TUI, and new enough for 'term'?)\n%s"
             % got.decode(errors="replace")[-300:])


def run_serial(port, baud):
    import serial
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.004     # also how long a key can wait to go out
    s.dtr = s.rts = False          # high on a CH343 resets the board
    s.open()
    try:
        s.set_buffer_size(rx_size=1 << 20, tx_size=1 << 14)
    except (AttributeError, NotImplementedError):
        pass
    info = start_feed(s, baud)
    words = info.split()
    cols, rows = int(words[words.index("cols") + 1]), int(words[words.index("rows") + 1])
    time.sleep(0.05)
    s.baudrate = baud
    time.sleep(0.1)
    s.write(b"\rterm redraw\r")     # whatever went out before the switch was lost
    if not PLAIN:
        return run_framed(s, "%s  %.1f Mbaud" % (port, baud / 1e6), cols, rows)
    size = os.get_terminal_size() if sys.stdout.isatty() else None
    if size and (size.columns < cols or size.lines < rows):
        print("this window is %dx%d; the board's screen is %dx%d. Make it bigger or the edges are cut off."
              % (size.columns, size.lines, cols, rows))
        time.sleep(2)
    term = Terminal()
    frames = Frames()
    last_ping = time.time()
    try:
        while True:
            data = s.read(65536)
            drawn = 0
            for body in frames.feed(data):
                if body:
                    term.write(body)
                else:               # the end of one screen update, now on the screen
                    drawn += 1
            if drawn:
                s.write(b"term ack\r" * drawn)
            if frames.bad:
                frames.bad = 0
                s.write(b"term redraw\r")
            keys = term.read()
            if QUIT in keys or 3 in keys:
                break
            for i in range(0, len(keys), 96):
                s.write(b"term in " + keys[i:i + 96].hex().encode() + b"\r")
            if time.time() - last_ping > 3:
                s.write(b"term ping\r")
                last_ping = time.time()
    finally:
        s.write(b"term off\r")
        time.sleep(0.3)
        term.restore()
        s.close()


def term_size():
    try:
        size = os.get_terminal_size()
        return size.columns, size.lines
    except OSError:
        return 200, 80


def run_framed(s, link, cols, rows):
    """The board's screen in lsterm's frame, with the panel (lspanel.py)."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import lsframe
    import lspanel
    scr = lsframe.Screen(cols, rows)
    scr.fold = FOLD is not None
    say = lambda line: s.write(line.encode() + b"\r")
    keys_to_board = lambda k: [s.write(b"term in " + k[i:i + 96].hex().encode() + b"\r")
                               for i in range(0, len(k), 96)]
    frame = lspanel.Frame(scr, "LAKESHARK", device=say, key=keys_to_board, link=link)
    scr.on_size = lambda c, r: frame.screen_grid_changed()
    term = Terminal()
    term.enter()
    size = term_size()
    frame.layout(*size)
    frames = Frames()
    frame.start()
    last_ping = last_tick = last_paint = time.time()
    count = [0, 0]
    try:
        while not frame.quitting:
            data = s.read(65536)
            drawn = 0
            for body in frames.feed(data):
                if not body:                    # the end of one screen update, now on the screen
                    drawn += 1
                    continue
                count[1] += len(body)
                if frame.rec:
                    frame.rec.out(FOLD(body) if FOLD else body)
                term.raw(scr.feed(body))
            if drawn:
                count[0] += drawn
                frame.frames_at = time.time()
                s.write(b"term ack\r" * drawn)
            for line in frames.lines():
                frame.board_said(line)
            if frames.bad:
                frames.bad = 0
                s.write(b"term redraw\r")
            now = time.time()
            rate = None
            if now - last_tick >= 1.0:
                rate = (count[0] / (now - last_tick), count[1] / (now - last_tick))
                count = [0, 0]
                last_tick = now
                if term_size() != size:
                    size = term_size()
                    frame.layout(*size)
            keys = term.read()
            if keys:
                keys_to_board(frame.route(keys))
            if keys or rate or now - last_paint >= 0.1:   # the panel: what changed, ten times a second
                term.raw(frame.tick(rate))
                last_paint = now
            if now - last_ping > 3:
                s.write(b"term ping\r")
                last_ping = now
    finally:
        if frame.rec:
            frame.record()
        s.write(b"term off\r")
        time.sleep(0.3)
        term.restore()
        s.close()


def run_tcp(host, port):
    sock = socket.create_connection((host, port), timeout=5)
    sock.settimeout(0.004)
    term = Terminal()
    iac = 0
    try:
        while True:
            try:
                data = sock.recv(65536)
                if not data:
                    break
            except socket.timeout:
                data = b""
            out = bytearray()
            for x in data:              # telnet option talk is not for the screen
                if iac == 0:
                    if x == 0xFF:
                        iac = 1
                    else:
                        out.append(x)
                elif iac == 1:
                    iac = 3 if x == 0xFA else 2 if x >= 0xFB else 0
                elif iac == 2:
                    iac = 0
                elif x == 0xF0:
                    iac = 0
            if out:
                term.write(bytes(out))
            keys = term.read()
            if QUIT in keys:
                sock.sendall(bytes([QUIT]))
                time.sleep(0.2)
                break
            if keys:
                sock.sendall(keys)
    finally:
        term.restore()
        sock.close()


def main():
    args = sys.argv[1:]
    if args and args[0] in ("-h", "--help"):
        sys.exit(__doc__)
    if args and args[0] == "--install-profile":
        return install_profile()
    global FOLD, PLAIN
    if "--sextants" in args:
        args.remove("--sextants")
        FOLD = None
    if "--plain" in args:
        args.remove("--plain")
        PLAIN = True
    if not args or args[0] == "--baud":
        args = [only_board()] + args
    if args[0] == "--sim":
        here = os.path.dirname(os.path.abspath(__file__))
        exe = os.environ.get("LSSIM_EXE") or \
            os.path.join(here, "..", "..", "bench", "build", "lssim.exe" if os.name == "nt" else "lssim")   # lsdk sim names its own
        app = args[1] if len(args) > 1 else "home"
        if app.startswith("lap:"):              # a native app built into the simulator: lap:carrier
            os.environ["LAPSIM"], app = app[4:], "lap"
            os.environ.setdefault("LSSIM_IMMERSIVE", "1")
        if "--grid" in args:                    # a Nano's terminal grid, e.g. --grid 160x45
            os.environ["LSSIM_GRID"] = args[args.index("--grid") + 1]
        sys.exit(os.spawnv(os.P_WAIT, exe, [exe, app, "-l", "--term"]))
    baud = int(args[args.index("--baud") + 1]) if "--baud" in args else 2000000    # faster gained nothing on a T-Display-P4
    if args[0].startswith("tcp://"):
        host, _, port = args[0][6:].partition(":")
        run_tcp(host, int(port or 2323))
    elif args[0] == "--serial":
        run_serial(find_port(args[1]), baud)
    else:
        run_serial(args[0], baud)


if __name__ == "__main__":
    main()
