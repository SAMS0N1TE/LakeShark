"""One console, two operators.

The board has one serial port and only one process can hold it. So a GUI for
the operator and a command line for an agent cannot both open it - whoever
gets there first wins and the other one silently sees a dead board. That is
the whole reason this is a broker rather than two scripts.

    serve   owns the port, keeps a rolling log, answers clients on localhost
    gui     buttons and presets, for driving the board by hand
    send    one command, for scripts and agents
    tail    recent output without sending anything
    pause   drop the port so esptool can have it, then resume

Both clients read bench/lsconsole/commands.json, so a button the operator
presses and a command an agent sends are the same string from the same file.

Serial rules that are not optional, all learned the hard way in this repo:

  * DTR and RTS are set false BEFORE the port is opened. The CH343 auto-reset
    circuit is hardware - assert either line and the board reboots, which eats
    the capture and looks like a crash. PuTTY and idf.py monitor both do this.
  * The first command after opening is reliably swallowed, so a bare newline
    goes first. The cause is leftover bytes in the firmware's line buffer, not
    a missing registration - see the CMD.PY GOTCHA correction in
    LAKESHARK_MARKERS.txt.
  * The board is found by its CH343 serial from bench/configs.json, never by
    COM number. COM numbers move between reboots and machines, and naming a
    port instead of a board is how the LCD once received the nano's image.
"""

import argparse
import json
import os
import socket
import subprocess
import sys
import threading
import time

HOST = "127.0.0.1"
PORT = 8731
HERE = os.path.dirname(os.path.abspath(__file__))
BENCH = os.path.dirname(HERE)

BAUD = 115200
LOG_MAX = 4000          # rolling lines kept for tail and the GUI
PROMPT = "lakeshark>"   # what the console prints when it is ready again
PROMPT_TIMEOUT = 15.0   # backstop when a command never returns to the prompt
SETTLE = 0.25           # after the prompt, catch anything still in flight


def load_commands():
    with open(os.path.join(HERE, "commands.json"), "r", encoding="utf-8") as f:
        return json.load(f)


def load_boards():
    with open(os.path.join(BENCH, "configs.json"), "r", encoding="utf-8") as f:
        return json.load(f)["configs"]


def find_port(board_name):
    """COM port for a board, by its recorded CH343 serial. Never by number."""
    from serial.tools import list_ports

    cfg = None
    for c in load_boards():
        if c.get("name") == board_name:
            cfg = c
            break
    if cfg is None:
        return None, "no board %r in bench/configs.json" % board_name
    serial_no = cfg.get("usb_serial")
    if not serial_no:
        return None, "%r has no usb_serial recorded - add it rather than flashing blind" % board_name

    for p in list_ports.comports():
        if p.serial_number and serial_no in p.serial_number:
            return p.device, None
    return None, "%s (serial %s) is not plugged in" % (board_name, serial_no)


# --------------------------------------------------------------------- broker


#LS-972  One renderer, so the GUI, the CLI and the TCP API cannot disagree
#   about what a timestamp looks like.
#
#   Two forms on purpose:
#     - `stamp()` is what a human reads: HH:MM:SS.mmm, local, fixed width.
#       Milliseconds matter - a mode switch takes ~2 s and a key injection
#       ~60 ms, and seconds-only cannot tell those apart.
#     - `iso()` is what a machine reads: full ISO-8601 with offset, which is
#       unambiguous across a timezone change or a day boundary and is what
#       goes into the structured `events` list.
#
#   The wire format carries BOTH: `lines` stays a list of strings so every
#   existing caller keeps working, and `events` carries {t, iso, text} for
#   anything parsing it as an API rather than reading it.

def stamp(t):
    ms = int((t - int(t)) * 1000)
    return time.strftime("%H:%M:%S", time.localtime(t)) + ".%03d" % ms


def iso(t):
    base = time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime(t))
    ms = int((t - int(t)) * 1000)
    off = -(time.altzone if time.daylight and time.localtime(t).tm_isdst else time.timezone)
    sign = "+" if off >= 0 else "-"
    off = abs(off)
    return "%s.%03d%s%02d:%02d" % (base, ms, sign, off // 3600, (off % 3600) // 60)


def render(entries):
    """(t, text) pairs -> display strings. Tolerates bare strings so a caller
       that has not been updated still gets something readable."""
    out = []
    for e in entries:
        if isinstance(e, (tuple, list)) and len(e) == 2:
            out.append("[%s] %s" % (stamp(e[0]), e[1]))
        else:
            out.append(str(e))
    return out


def events(entries):
    out = []
    for e in entries:
        if isinstance(e, (tuple, list)) and len(e) == 2:
            out.append({"t": round(e[0], 3), "iso": iso(e[0]), "text": e[1]})
        else:
            out.append({"t": None, "iso": None, "text": str(e)})
    return out



class Broker:
    def __init__(self, board):
        self.board = board
        self.ser = None
        self.port_name = None
        self.paused = False
        self.log = []
        #LS-1087  Lines trimmed off the front of the log, ever. See send().
        self.dropped = 0
        self.lock = threading.Lock()      # serialises whole command exchanges
        self.log_lock = threading.Lock()
        self.partial = ""     # bytes with no newline yet - the prompt lives here
        self.err = None

    # -- port ------------------------------------------------------------

    def open(self):
        import serial

        if self.ser is not None:
            return True
        dev, err = find_port(self.board)
        if dev is None:
            self.err = err
            return False
        s = serial.Serial()
        s.port = dev
        s.baudrate = BAUD
        s.timeout = 0.1
        # Before open, never after. See the module docstring.
        s.dtr = False
        s.rts = False
        try:
            s.open()
        except Exception as e:                      # noqa: BLE001
            self.err = "%s: %s" % (dev, e)
            return False
        time.sleep(0.4)
        try:
            s.reset_input_buffer()
            s.write(b"\r\n")                        # absorb the swallowed first
        except Exception:                           # noqa: BLE001
            pass
        self.ser = s
        self.port_name = dev
        self.err = None
        self.note("-- attached %s at %d --" % (dev, BAUD))
        return True

    def close(self):
        if self.ser is not None:
            try:
                self.ser.close()
            except Exception:                       # noqa: BLE001
                pass
            self.note("-- released %s --" % self.port_name)
        self.ser = None
        self.port_name = None

    # -- log -------------------------------------------------------------

    def note(self, text):
        """Record one line WITH the moment it arrived.

        LS-972  Every entry is (epoch_seconds, text). The log used to be bare
        strings, so a transcript could not answer the first question anyone
        asks of it - when, and how long did that take. A bench log without
        time is a list of assertions with no way to check the order of two
        things that happened on different tasks.

        Time is stamped HERE, in the reader, not when a client formats it:
        the gap between the board emitting a line and a GUI drawing it is
        exactly the interval being measured. Meshtastic's device log does the
        same thing for the same reason.
        """
        with self.log_lock:
            self.log.append((time.time(), text))
            if len(self.log) > LOG_MAX:
                n = len(self.log) - LOG_MAX
                del self.log[:n]
                self.dropped += n

    def tail(self, n):
        with self.log_lock:
            return self.log[-n:] if n > 0 else list(self.log)

    #LS-1087  Positions in the log are absolute line numbers, not indices.
    #
    #   The log is capped at LOG_MAX and trimmed from the FRONT, so an index
    #   into it stops meaning anything the moment the cap is reached: the
    #   length pins at 4000 while the content slides underneath it. send()
    #   held one across its wait, so once a session had logged 4000 lines -
    #   a few hours of bench work - `self.log[mark + 1:]` was `[4001:]`,
    #   which is empty forever. From then on EVERY command saw no output,
    #   never matched the prompt, burned the full 15 s timeout and reported
    #   "the console may be busy" while the board was answering in 120 ms
    #   and the reader was faithfully logging every word of it.
    #
    #   That looks like a board degrading over a session rather than like a
    #   bug, and it cost a wrong diagnosis: the UI task's poll rate got the
    #   blame and was changed, and of course nothing improved. The evidence
    #   was in the log the whole time - a `version` round trip stamped at
    #   126 ms sitting inside a reply that claimed 15,285.
    #
    #   `dropped` counts what has fallen off the front, so a mark converts
    #   back to a live index by subtracting it, clamped at 0 for a mark whose
    #   whole window has scrolled away.

    def mark(self):
        """Absolute line number of the next line that will be logged."""
        with self.log_lock:
            return self.dropped + len(self.log)

    def since(self, mark):
        """Every entry logged at or after absolute line number `mark`."""
        with self.log_lock:
            return self.log[max(0, mark - self.dropped):]

    def partial_text(self):
        with self.log_lock:
            return self.partial

    def reader(self):
        """Everything the board says, all the time - not just replies.

        The console is shared with the log, so a subsystem that talks (the BLE
        retry loop emits a dozen lines every five seconds) will overrun a
        buffer that is only drained around a command. Draining continuously is
        also what lets the GUI show a live console rather than a transcript of
        answers.
        """
        buf = b""
        while True:
            if self.ser is None or self.paused:
                time.sleep(0.2)
                continue
            try:
                chunk = self.ser.read(4096)
            except Exception as e:                  # noqa: BLE001
                self.note("-- read failed: %s --" % e)
                self.close()
                time.sleep(1.0)
                continue
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                self.note(line.decode("utf-8", "replace").rstrip("\r"))

    # -- commands --------------------------------------------------------

    def send(self, cmd, wait):
        """Send one line and collect what arrives for `wait` seconds.

        The reply is not separated from the log, because it cannot be reliably:
        both arrive on the same wire. The client is given everything that
        landed in the window and decides what it cares about.
        """
        with self.lock:
            if self.paused:
                return {"ok": False, "error": "port is paused - resume first"}
            if self.ser is None and not self.open():
                return {"ok": False, "error": self.err or "no port"}
            mark = self.mark()          #LS-1087  absolute, see mark()
            t0 = time.time()
            try:
                self.ser.write((cmd + "\r\n").encode())
                self.ser.flush()
            except Exception as e:                  # noqa: BLE001
                self.close()
                return {"ok": False, "error": "write failed: %s" % e}
            self.note(">> " + cmd)
            # Drop whatever prompt is already sitting in the partial buffer.
            # Without this the wait below matches the PREVIOUS command's
            # prompt and returns at once, which truncated `lora rx 8` to its
            # first line and looked exactly like the board going quiet - the
            # same symptom this prompt-wait was written to cure.
            with self.log_lock:
                self.partial = ""

            #LS-962  Wait for the prompt, not for a stopwatch.
            #
            #   A fixed window loses the reply to anything slow. `mode fm`
            #   tears the radio down and brings it back up, which takes longer
            #   than any sensible default, so its answer landed after the
            #   window closed and was reported against the NEXT command - or
            #   against nothing at all. From the outside that looks like the
            #   board ignoring you, which is what it looked like.
            #
            #   `wait` is now a minimum settle rather than the whole budget.
            deadline = time.time() + max(float(wait), PROMPT_TIMEOUT)
            #LS-973  A short floor, now that the prompt is detected properly.
            #   It was 1.0 s, which was a hedge against never matching the
            #   prompt at all; with that fixed it is pure latency on every
            #   command. SETTLE still catches anything that lands late.
            floor = time.time() + min(float(wait), 0.15)
            seen = False
            last_n = 0
            last_change = time.time()
            while time.time() < deadline:
                time.sleep(0.05)
                tail = self.partial_text()
                fresh = [e[1] for e in self.since(mark)[1:]]
                #LS-973  Ready means a line that is JUST the prompt.
                #
                #   Two wrong versions preceded this one, and the timestamps
                #   added in LS-972 are what exposed the second:
                #
                #   1. "any log line containing the prompt" - matched the
                #      board's own echo, `lakeshark> lora rx 10`, so every
                #      command was declared finished the instant it was
                #      echoed and long replies were truncated.
                #   2. "only the partial buffer" - assumed the prompt is
                #      printed with no trailing newline. It is not: this
                #      firmware emits it terminated, so it lands in the log
                #      and `partial` stays empty. Nothing ever matched, and
                #      EVERY command silently burned the full 15 s timeout.
                #      A `version` whose reply arrived in 120 ms took 15,280.
                #      That was invisible until a round-trip time was printed.
                #
                #   The distinction that actually holds: the echo is
                #   "lakeshark> version" and the ready prompt is "lakeshark>"
                #   with nothing after it. Compare the whole stripped line,
                #   not a substring. `partial` is still checked, because a
                #   build that leaves the prompt unterminated is also correct
                #   and should not regress to a timeout.
                ready = tail.strip() == PROMPT or any(
                    ln.strip() == PROMPT for ln in fresh)

                #LS-1003  Silence counts as finished too.
                #
                #   Matching only the prompt means one missed prompt costs the
                #   full 15 s timeout - and because send() holds the lock for
                #   its whole wait, every command queued behind it times out
                #   as well. One miss became five, which looked like a wedged
                #   board and was not: the replies were all in the log.
                #
                #   So: if output arrived and then stopped for 400 ms, the
                #   command is done. The prompt is still the fast path; this
                #   is the floor under it.
                if len(fresh) > 0:
                    if len(fresh) != last_n:
                        last_n = len(fresh)
                        last_change = time.time()
                    elif time.time() - last_change > 0.4:
                        ready = True

                if ready:
                    seen = True
                    if time.time() >= floor:
                        break
            time.sleep(SETTLE)
            entries = self.since(mark)
            #LS-972  Round-trip time, measured rather than guessed at. This is
            #   the number that answers "why did that feel slow" - a mode
            #   switch is ~2 s because it tears the radio down, `lora rx 10`
            #   is 10 s because it was asked to listen for 10, and a key
            #   injection is ~60 ms. Without it the three are indistinguishable
            #   from the outside, which is what prompted this.
            elapsed_ms = int((time.time() - t0) * 1000)
            return {"ok": True, "port": self.port_name,
                    "lines": render(entries),
                    "events": events(entries),
                    "elapsed_ms": elapsed_ms,
                    "prompt": seen,
                    "note": None if seen else
                            "no prompt within %.0fs - the console may be busy"
                            % max(float(wait), PROMPT_TIMEOUT)}

    # -- server ----------------------------------------------------------

    def handle(self, conn):
        f = conn.makefile("rwb")
        try:
            for raw in f:
                try:
                    req = json.loads(raw.decode("utf-8"))
                except Exception:                   # noqa: BLE001
                    continue
                op = req.get("op")
                if op == "send":
                    rep = self.send(req.get("cmd", ""), req.get("wait", 3))
                elif op == "tail":
                    entries = self.tail(int(req.get("n", 200)))
                    rep = {"ok": True,
                           "lines": render(entries),
                           "events": events(entries)}
                elif op == "pause":
                    self.paused = True
                    self.close()
                    rep = {"ok": True, "paused": True}
                elif op == "resume":
                    self.paused = False
                    rep = {"ok": self.open(), "error": self.err}
                elif op == "status":
                    rep = {
                        "ok": True,
                        "board": self.board,
                        "port": self.port_name,
                        "attached": self.ser is not None,
                        "paused": self.paused,
                        "error": self.err,
                        "log_lines": len(self.log),
                    }
                else:
                    rep = {"ok": False, "error": "unknown op %r" % op}
                f.write((json.dumps(rep) + "\n").encode())
                f.flush()
        except Exception:                           # noqa: BLE001
            pass
        finally:
            try:
                conn.close()
            except Exception:                       # noqa: BLE001
                pass

    def serve(self):
        threading.Thread(target=self.reader, daemon=True).start()
        self.open()          # a board that is not plugged in is not an error
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind((HOST, PORT))
        srv.listen(8)
        print("lsconsole: serving %s on %s:%d" % (self.board, HOST, PORT))
        sys.stdout.flush()
        while True:
            conn, _ = srv.accept()
            threading.Thread(target=self.handle, args=(conn,), daemon=True).start()


# --------------------------------------------------------------------- client


def request(req, timeout=60, autostart=True, board=None):
    try:
        s = socket.create_connection((HOST, PORT), timeout=3)
    except OSError:
        if not autostart:
            return {"ok": False, "error": "broker is not running"}
        start_broker(board)
        for _ in range(30):
            time.sleep(0.4)
            try:
                s = socket.create_connection((HOST, PORT), timeout=3)
                break
            except OSError:
                continue
        else:
            return {"ok": False, "error": "could not start the broker"}
    s.settimeout(timeout)
    f = s.makefile("rwb")
    f.write((json.dumps(req) + "\n").encode())
    f.flush()
    line = f.readline()
    s.close()
    if not line:
        return {"ok": False, "error": "broker closed the connection"}
    return json.loads(line.decode("utf-8"))


def start_broker(board=None):
    """Detached, so the first client to need it starts it and nobody owns it."""
    cmd = [sys.executable, os.path.abspath(__file__), "serve"]
    if board:
        cmd += ["--board", board]
    flags = 0
    if os.name == "nt":
        flags = subprocess.CREATE_NEW_PROCESS_GROUP | 0x00000008  # DETACHED
    subprocess.Popen(cmd, creationflags=flags,
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


# ------------------------------------------------------------------------ gui


def run_gui(board):
    import tkinter as tk
    from tkinter import ttk

    spec = load_commands()
    root = tk.Tk()
    root.title("LakeShark console - %s" % board)
    root.geometry("1080x760")

    BG, FG, ACC = "#101418", "#d8e2e8", "#37c8c0"
    PANEL, DIM = "#1b2229", "#6d7a83"
    root.configure(bg=BG)

    style = ttk.Style()
    try:
        style.theme_use("clam")
    except Exception:                               # noqa: BLE001
        pass
    style.configure("TFrame", background=BG)
    style.configure("TLabel", background=BG, foreground=FG)
    style.configure("TLabelframe", background=BG, foreground=ACC)
    style.configure("TLabelframe.Label", background=BG, foreground=ACC)

    out = tk.Text(root, bg="#07090b", fg="#c8d6dc", insertbackground=FG,
                  font=("Consolas", 10), wrap="none", height=18)
    status = tk.Label(root, text="starting...", bg=BG, fg=ACC, anchor="w",
                      font=("Consolas", 9))

    def log(text, tag=None, stamped=False):
        #LS-972  A line the GUI writes itself (">> cmd", an error) gets a
        #   timestamp here; a line that came from the broker already carries
        #   the moment it ARRIVED, which is the more truthful number and
        #   must not be overwritten with the moment it was drawn.
        if not stamped:
            text = "[%s] %s" % (stamp(time.time()), text)
        out.insert("end", text + "\n", tag)
        out.see("end")

    out.tag_configure("sent", foreground=ACC)
    out.tag_configure("err", foreground="#ff6b6b")

    def do(cmd, wait=None):
        """Every path - button, preset, typed - goes through here."""
        if wait is None:
            wait = spec.get("default_wait", 3)
        log(">> " + cmd, "sent")
        root.update_idletasks()

        def worker():
            rep = request({"op": "send", "cmd": cmd, "wait": wait}, board=board)
            def show():
                if not rep.get("ok"):
                    log("!! " + str(rep.get("error")), "err")
                else:
                    for ln in rep.get("lines", []):
                        if ">> " not in ln[:16]:
                            log(ln, stamped=True)
                    if rep.get("elapsed_ms") is not None:
                        busy = "" if rep.get("prompt") else "  no prompt - console may be busy"
                        log("   -- %d ms%s" % (rep["elapsed_ms"], busy),
                            None if rep.get("prompt") else "err", stamped=True)
            root.after(0, show)

        threading.Thread(target=worker, daemon=True).start()

    # -- on-screen keyboard ----------------------------------------------

    #LS-991  The LilyGO T-Deck keyboard, key for key.
    #
    #   The first version was a generic QWERTY grid and it was missing most
    #   of the board: no function row, no DEL, no CTRL/ALT/FN, and none of
    #   the orange FN-layer symbols printed above the keys. That matters
    #   beyond convenience - this GUI is how we prove a key WORKS, so a key
    #   it cannot send is a key nobody has ever tested.
    #
    #   Layout transcribed from the hardware on 2026-09-09:
    #
    #     F1..F11
    #     ESC 1 2 3 4 5 6 7 8 9 0                (shift: ! @ # $ % ^ & * ( ) )
    #     Q W E R T Y U I O P DEL                (shift: ' ~ - + = \ | ; : " )
    #     CAPS A S D F G H J K L ENTER           (shift: ~ [ ] { } ` ' / ? )
    #     ALT Z X C V B N M CTRL UP MIC          (shift: . < > on B N M )
    #     FN LILYGO SHIFT TAB SPACE FN < v >
    #
    #   Every key goes out as `tui key <x>` - the same path the hardware
    #   keyboard's characters take through s_tui_inject into
    #   ls_tui_router_key.
    #
    #   KEYS THE FIRMWARE CANNOT ACCEPT are drawn and disabled rather than
    #   omitted: CTRL, ALT, FN, CAPS, MIC and the LILYGO key have no ls_tk_t
    #   and no console spelling. Showing them greyed says "this exists on the
    #   hardware and nothing is listening" - which is information. Hiding
    #   them would say nothing at all.

    PANEL, DIM, DEAD = "#1b2229", "#6d7a83", "#141a1f"

    # (unshifted, shifted). Shifted values are the orange legends.
    KB_NUM = [("1", "!"), ("2", "@"), ("3", "#"), ("4", "$"), ("5", "%"),
              ("6", "^"), ("7", "&"), ("8", "*"), ("9", "("), ("0", ")")]
    KB_Q   = [("q", "'"), ("w", "~"), ("e", "-"), ("r", "+"), ("t", "="),
              ("y", "\\"), ("u", "|"), ("i", ";"), ("o", ":"), ("p", '"')]
    KB_A   = [("a", "~"), ("s", "["), ("d", "]"), ("f", "{"), ("g", "}"),
              ("h", "`"), ("j", "'"), ("k", "/"), ("l", "?")]
    KB_Z   = [("z", "z"), ("x", "x"), ("c", "c"), ("v", "v"),
              ("b", "."), ("n", "<"), ("m", ">")]

    kb_shift = {"on": False}
    kb_keys = []            # (widget, unshifted, shifted)

    def key_send(name):
        """One key, exactly as the hardware keyboard would deliver it."""
        threading.Thread(
            target=lambda: request({"op": "send", "cmd": "tui key " + name,
                                    "wait": 1.5}, board=board),
            daemon=True).start()

    kb_wrap = tk.Frame(root, bg=BG)
    kb_wrap.pack(fill="x", padx=8, pady=(0, 4))

    kb_head = tk.Frame(kb_wrap, bg=BG)
    kb_head.pack(fill="x")
    tk.Label(kb_head, text="KEYBOARD", bg=BG, fg=ACC,
             font=("Consolas", 9, "bold")).pack(side="left")
    tk.Label(kb_head, text="  every key sends 'tui key <x>' - the same path the "
                           "hardware keyboard uses.  Greyed keys have no firmware binding.",
             bg=BG, fg=DIM, font=("Consolas", 8)).pack(side="left")

    def mkkey(parent, label, name, width=3, dead=False, colour=None):
        b = tk.Button(parent, text=label, width=width,
                      bg=DEAD if dead else (colour or PANEL),
                      fg=DIM if dead else FG,
                      activebackground=PANEL if dead else ACC,
                      relief="flat", bd=0, font=("Consolas", 9),
                      state="disabled" if dead else "normal",
                      disabledforeground=DIM,
                      command=(lambda: None) if dead else (lambda: key_send(name)))
        b.pack(side="left", padx=1, pady=1)
        return b

    def mkpair(parent, pair, width=3):
        lo, hi = pair
        b = mkkey(parent, lo, lo, width)
        kb_keys.append((b, lo, hi))
        return b

    def row():
        f = tk.Frame(kb_wrap, bg=BG)
        f.pack(fill="x")
        return f

    # function row
    r1 = row()
    for i in range(1, 12):
        mkkey(r1, "F%d" % i, "f%d" % i, width=4)

    # number row
    r2 = row()
    mkkey(r2, "ESC", "esc", width=5)
    for pair in KB_NUM:
        mkpair(r2, pair)

    # QWERTY row
    r3 = row()
    for pair in KB_Q:
        mkpair(r3, pair)
    mkkey(r3, "DEL", "backspace", width=5)

    # home row
    r4 = row()
    mkkey(r4, "CAPS", "caps", width=5, dead=True)   # no ls_tk_t for caps
    for pair in KB_A:
        mkpair(r4, pair)
    mkkey(r4, "ENTER", "enter", width=6)

    # bottom letter row
    r5 = row()
    mkkey(r5, "ALT", "alt", width=5)
    for pair in KB_Z:
        mkpair(r5, pair)
    mkkey(r5, "CTRL", "ctrl", width=5)
    mkkey(r5, "^", "up", width=3)
    mkkey(r5, "MIC", "mic", width=4, colour="#3a2233")

    # modifier / space row
    r6 = row()
    mkkey(r6, "FN", "fn", width=4)
    mkkey(r6, "LILY", "lilygo", width=5)

    def toggle_shift():
        kb_shift["on"] = not kb_shift["on"]
        shift_btn.configure(bg=ACC if kb_shift["on"] else PANEL,
                            fg=BG if kb_shift["on"] else FG)
        for b, lo, hi in kb_keys:
            face = hi if kb_shift["on"] else lo
            show = face.upper() if (kb_shift["on"] and face.isalpha()) else face
            b.configure(text=show, command=lambda c=show: key_send(c))

    shift_btn = tk.Button(r6, text="SHIFT", width=6, bg=PANEL, fg=FG,
                          activebackground=ACC, relief="flat", bd=0,
                          font=("Consolas", 9), command=toggle_shift)
    shift_btn.pack(side="left", padx=1)

    mkkey(r6, "TAB", "tab", width=5)
    mkkey(r6, "SPACE", "space", width=16)
    mkkey(r6, "FN", "fn", width=4)
    mkkey(r6, "<", "left", width=3)
    mkkey(r6, "v", "down", width=3)
    mkkey(r6, ">", "right", width=3)

    #LS-991  A completeness row. The hardware reaches these through FN
    #   combinations that are hard to read off the silkscreen, and a symbol
    #   nobody can send is a symbol nobody can test. Everything printable
    #   that is not already on a key above lives here, so the set the GUI can
    #   send is the full printable ASCII range with no gaps.
    r7 = row()
    tk.Label(r7, text=" more ", bg=BG, fg=DIM, font=("Consolas", 8)).pack(side="left")
    for chx in "!@#$%^&*()-_=+[]{};:'\"\\|,.<>/?`~":
        mkkey(r7, chx, chx, width=2)

    # -- controls --------------------------------------------------------

    top = ttk.Frame(root)
    top.pack(fill="x", padx=8, pady=(8, 0))

    nb = ttk.Notebook(top)
    nb.pack(fill="x")

    for group in spec.get("groups", []):
        tab = ttk.Frame(nb)
        nb.add(tab, text=group["name"])
        for i, b in enumerate(group.get("buttons", [])):
            btn = tk.Button(
                tab, text=b["label"], width=11, height=2,
                bg="#1b2732" if b.get("verified", True) else "#2a2018",
                fg=FG, activebackground=ACC, activeforeground="#06181c",
                relief="flat", font=("Consolas", 9, "bold"),
                command=lambda b=b: do(b["cmd"], b.get("wait")))
            btn.grid(row=i // 7, column=i % 7, padx=3, pady=3, sticky="ew")
            tip = b.get("help") or b["cmd"]
            if not b.get("verified", True):
                tip += "   [UNVERIFIED - may not exist in this build]"
            btn.bind("<Enter>", lambda e, t=tip: status.config(text=t))
            btn.bind("<Leave>", lambda e: status.config(text=state_line()))

    # -- presets ---------------------------------------------------------

    pre = ttk.Labelframe(root, text="PRESETS")
    pre.pack(fill="x", padx=8, pady=6)

    def run_preset(p):
        def worker():
            for step in p["steps"]:
                rep = request({"op": "send", "cmd": step,
                               "wait": spec.get("default_wait", 3)}, board=board)
                def show(step=step, rep=rep):
                    log(">> " + step, "sent")
                    if not rep.get("ok"):
                        log("!! " + str(rep.get("error")), "err")
                    else:
                        for ln in rep.get("lines", []):
                            if ">> " not in ln[:16]:
                                log(ln, stamped=True)
                        if rep.get("elapsed_ms") is not None:
                            log("   -- %d ms" % rep["elapsed_ms"], stamped=True)
                root.after(0, show)
        threading.Thread(target=worker, daemon=True).start()

    for i, p in enumerate(spec.get("presets", [])):
        b = tk.Button(pre, text=p["label"], bg="#22303c", fg=FG, relief="flat",
                      activebackground=ACC, font=("Consolas", 9),
                      command=lambda p=p: run_preset(p))
        b.grid(row=0, column=i, padx=3, pady=4, sticky="ew")
        b.bind("<Enter>", lambda e, t=p.get("help", ""): status.config(text=t))
        b.bind("<Leave>", lambda e: status.config(text=state_line()))

    # -- free text -------------------------------------------------------

    row = ttk.Frame(root)
    row.pack(fill="x", padx=8)
    entry = tk.Entry(row, bg="#07090b", fg=FG, insertbackground=FG,
                     font=("Consolas", 11), relief="flat")
    entry.pack(side="left", fill="x", expand=True, ipady=6)
    entry.bind("<Return>", lambda e: (do(entry.get()), entry.delete(0, "end")))
    tk.Button(row, text="SEND", bg=ACC, fg="#06181c", relief="flat",
              font=("Consolas", 9, "bold"),
              command=lambda: (do(entry.get()), entry.delete(0, "end"))
              ).pack(side="left", padx=6)

    out.pack(fill="both", expand=True, padx=8, pady=6)
    status.pack(fill="x", padx=8, pady=(0, 6))

    # -- port sharing ----------------------------------------------------

    bar = ttk.Frame(root)
    bar.pack(fill="x", padx=8, pady=(0, 8))

    def set_pause(on):
        rep = request({"op": "pause" if on else "resume"}, board=board)
        log("-- port %s --" % ("released for flashing" if on else "reattached"),
            "sent" if rep.get("ok") else "err")

    tk.Button(bar, text="RELEASE PORT (before flashing)", bg="#3a2a1a", fg=FG,
              relief="flat", font=("Consolas", 9),
              command=lambda: set_pause(True)).pack(side="left", padx=(0, 6))
    tk.Button(bar, text="REATTACH", bg="#1b2732", fg=FG, relief="flat",
              font=("Consolas", 9),
              command=lambda: set_pause(False)).pack(side="left")

    state = {"text": "starting..."}

    def state_line():
        return state["text"]

    def poll():
        def worker():
            st = request({"op": "status"}, board=board)
            if st.get("ok"):
                state["text"] = "board %s   port %s   %s%s" % (
                    st.get("board"), st.get("port") or "-",
                    "paused" if st.get("paused") else
                    ("attached" if st.get("attached") else "NOT ATTACHED"),
                    "   " + st["error"] if st.get("error") else "")
            else:
                state["text"] = "broker: %s" % st.get("error")
            root.after(0, lambda: status.config(text=state["text"]))
        threading.Thread(target=worker, daemon=True).start()
        root.after(2000, poll)

    poll()
    log("LakeShark console. Buttons and presets come from commands.json,")
    log("which the agent's CLI reads too - same strings, same file.")
    root.mainloop()


# ----------------------------------------------------------------------- main


def main():
    spec_board = None
    try:
        spec_board = load_commands().get("default_board")
    except Exception:                               # noqa: BLE001
        pass

    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("mode", choices=["serve", "gui", "send", "tail",
                                     "pause", "resume", "status", "list"])
    ap.add_argument("command", nargs="?", default="")
    ap.add_argument("--board", default=spec_board or "t-display-p4")
    ap.add_argument("--wait", type=float, default=3.0)
    ap.add_argument("-n", type=int, default=60)
    args = ap.parse_args()

    if args.mode == "serve":
        Broker(args.board).serve()
        return 0

    if args.mode == "gui":
        run_gui(args.board)
        return 0

    if args.mode == "list":
        spec = load_commands()
        for g in spec.get("groups", []):
            print("[%s]" % g["name"])
            for b in g.get("buttons", []):
                flag = "" if b.get("verified", True) else "  (unverified)"
                print("  %-10s %-24s %s%s" % (b["label"], b["cmd"],
                                              b.get("help", ""), flag))
        print("[PRESETS]")
        for p in spec.get("presets", []):
            print("  %-22s %s" % (p["label"], " ; ".join(p["steps"])))
        return 0

    if args.mode == "send":
        if not args.command:
            print("nothing to send", file=sys.stderr)
            return 2
        rep = request({"op": "send", "cmd": args.command, "wait": args.wait},
                      board=args.board)
    elif args.mode == "tail":
        rep = request({"op": "tail", "n": args.n}, board=args.board)
    else:
        rep = request({"op": args.mode}, board=args.board)

    if not rep.get("ok"):
        print("error: %s" % rep.get("error"), file=sys.stderr)
        return 1
    if args.mode == "status":
        print(json.dumps(rep, indent=2))
        return 0
    for ln in rep.get("lines", []):
        print(ln)
    #LS-972  Say how long it took. This is the whole reason the timestamps
    #   exist: a mode switch and a wedged console look identical without it.
    if args.mode == "send" and rep.get("elapsed_ms") is not None:
        note = "" if rep.get("prompt") else "  (no prompt - console may be busy)"
        print("-- %d ms%s" % (rep["elapsed_ms"], note), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
