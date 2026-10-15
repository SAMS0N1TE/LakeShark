"""Watch the bench boards continuously so a freeze or crash leaves evidence.

Holds each board's console open (DTR/RTS low, so opening never resets it),
writes everything it prints to logs/<name>-<date>.log with laptop timestamps,
and polls `perf` and `memory` every POLL seconds. One line per anomaly goes to
events.log (and stdout):

  BOOT       the board restarted (boot banner seen)
  PANIC      panic / abort / watchdog / Guru text in its output
  STALL      the UI stopped presenting frames (perf fps below 2)
  NOREPLY    the console stopped answering polls
  LOWMEM     internal or DMA heap below the floor
  BACK       a board that had NOREPLY or STALL answers normally again

On STALL or NOREPLY it immediately asks for `trail`, `top` and `heap` so the
state is in the log before anyone resets the board.

Commands can still be sent while it runs: write lines to inbox/<name>.cmd; the
file is consumed and the replies land in that board's log.

usage: python board_watch.py SERIAL=NAME [SERIAL=NAME ...] [--skip COMx ...]
  e.g. python bench/tools/board_watch.py 5C84301528=SX 5B91081095=LR
"""
import os, re, sys, threading, time, datetime
import serial
from serial.tools import list_ports

HERE = os.environ.get("LS_WATCH_DIR", os.path.join(os.path.expanduser("~"), "LakeSharkWatch"))
LOGS, INBOX = os.path.join(HERE, "logs"), os.path.join(HERE, "inbox")
EVENTS = os.path.join(HERE, "events.log")
BOARDS = dict(a.split("=", 1) for a in sys.argv[1:] if "=" in a)   # USB serial -> name
FORBIDDEN = {sys.argv[i + 1] for i, a in enumerate(sys.argv[:-1]) if a == "--skip"}   # ports never opened
POLL, NOREPLY_AFTER = 15, 3                            # seconds, missed polls
MIN_INTERNAL, MIN_DMA = 20000, 12000                   # bytes

PANIC_RE = re.compile(r"Guru Meditation|abort\(\)|panic'ed|Backtrace:|task_wdt|"
                      r"Interrupt wdt|rst:0x(?![1] \(POWERON)|HP_SYS_HP_WDT|stack overflow|assert failed", re.I)
BOOT_RE = re.compile(r"ESP-ROM:|boot: ESP-IDF|cpu_start: Pro cpu start")
PERF_RE = re.compile(r"fps=([\d.]+) frame_ms_p50=([\d.]+) frame_ms_p95=([\d.]+) "
                     r"frame_ms_max=([\d.]+).*late_frames=(\d+) screen=(\S+)")
MEM_RE = re.compile(r"internal_free=(\d+).*dma_free=(\d+)")

os.makedirs(LOGS, exist_ok=True)
os.makedirs(INBOX, exist_ok=True)
elock = threading.Lock()


def now():
    return datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")


def event(name, kind, text):
    line = "%s %-3s %-8s %s" % (now(), name, kind, text)
    with elock:
        with open(EVENTS, "a", encoding="utf-8") as f:
            f.write(line + "\n")
        print(line, flush=True)


def port_for(serial_no):
    for p in list_ports.comports():
        if p.serial_number == serial_no and p.device not in FORBIDDEN:
            return p.device
    return None


class Watch(threading.Thread):
    def __init__(self, serial_no, name):
        super().__init__(daemon=True)
        self.sn, self.name = serial_no, name
        self.s = None
        self.buf = ""
        self.last_reply = time.time()
        self.missed = 0
        self.state = "ok"            # ok | stall | noreply
        self.last_perf = None

    def log(self, text):
        path = os.path.join(LOGS, "%s-%s.log" % (self.name, datetime.date.today().isoformat()))
        with open(path, "a", encoding="utf-8", errors="replace") as f:
            f.write(text)

    def open(self):
        while True:
            dev = port_for(self.sn)
            if dev:
                try:
                    s = serial.Serial()
                    s.port, s.baudrate, s.timeout = dev, 115200, 0.2
                    s.dtr = s.rts = False
                    s.open()
                    self.s = s
                    event(self.name, "ATTACH", dev)
                    return
                except serial.SerialException as e:
                    pass
            time.sleep(5)

    def send(self, cmd):
        try:
            self.s.write((cmd + "\r\n").encode())
            self.log("%s >>> %s\n" % (now(), cmd))
        except serial.SerialException:
            self.s = None

    def snapshot(self, why):
        self.log("%s ---- snapshot (%s)\n" % (now(), why))
        for c in ("trail", "top", "heap", "crash"):
            self.send(c)
            time.sleep(1.5)

    def handle_line(self, line):
        self.log("%s %s\n" % (now(), line))
        if BOOT_RE.search(line):
            event(self.name, "BOOT", line.strip()[:120])
        if PANIC_RE.search(line):
            event(self.name, "PANIC", line.strip()[:160])
        if ("UI stalled" in line or "perf_error=ui_timeout" in line) and self.state != "stall":
            self.state = "stall"
            event(self.name, "STALL", line.strip()[:160])
            threading.Thread(target=self.snapshot, args=("stall",), daemon=True).start()
        m = PERF_RE.search(line)
        if m:
            self.last_reply = time.time()
            self.missed = 0
            fps, p95, mx, screen = float(m.group(1)), float(m.group(3)), float(m.group(4)), m.group(6)
            self.last_perf = (fps, p95, mx, screen)
            if fps < 2 and self.state != "stall":
                self.state = "stall"
                event(self.name, "STALL", "fps=%.1f max=%.0fms on %s" % (fps, mx, screen))
                threading.Thread(target=self.snapshot, args=("stall",), daemon=True).start()
            elif fps >= 2 and self.state != "ok":
                event(self.name, "BACK", "fps=%.1f on %s" % (fps, screen))
                self.state = "ok"
        m = MEM_RE.search(line)
        if m:
            internal, dma = int(m.group(1)), int(m.group(2))
            if internal < MIN_INTERNAL or dma < MIN_DMA:
                event(self.name, "LOWMEM", "internal=%d dma=%d" % (internal, dma))

    def poll(self):
        while True:
            time.sleep(POLL)
            if not self.s:
                continue
            inbox = os.path.join(INBOX, self.name + ".cmd")
            if os.path.exists(inbox):
                try:
                    cmds = open(inbox, encoding="utf-8").read().splitlines()
                    os.remove(inbox)
                except OSError:
                    cmds = []
                for c in cmds:
                    if c.strip():
                        self.send(c.strip()); time.sleep(1.0)
            if time.time() - self.last_reply > POLL * NOREPLY_AFTER + 5:
                self.missed += 1
                if self.state != "noreply":
                    self.state = "noreply"
                    event(self.name, "NOREPLY", "no perf reply for %ds" % int(time.time() - self.last_reply))
                    threading.Thread(target=self.snapshot, args=("noreply",), daemon=True).start()
            self.send("perf")
            time.sleep(1.0)
            self.send("memory")

    def run(self):
        self.open()
        threading.Thread(target=self.poll, daemon=True).start()
        while True:
            if not self.s:
                event(self.name, "DETACH", "port lost; waiting for it")
                self.open()
                self.last_reply = time.time()
            try:
                data = self.s.read(4096)
            except serial.SerialException:
                self.s = None
                continue
            if not data:
                continue
            self.buf += data.decode("utf-8", "replace")
            while "\n" in self.buf:
                line, self.buf = self.buf.split("\n", 1)
                self.handle_line(line.rstrip("\r"))


if __name__ == "__main__":
    if not BOARDS:
        sys.exit(__doc__)
    event("---", "START", "watching %s" % ", ".join(BOARDS.values()))
    for sn, name in BOARDS.items():
        Watch(sn, name).start()
    while True:
        time.sleep(3600)
