"""Drive `lssim <app> --term` like a person at a terminal and check what it draws.

  python -I term_check.py <lssim.exe> [app] [--keys "f1,down,enter"] [--tap C:R] [--dump]

Feeds the keys as terminal bytes, reads the escape stream back through a
small interpreter of exactly what ls_term_vt.c emits (cursor moves, 24-bit
SGR, UTF-8 text, the mode switches), then quits with Ctrl-]. Prints the
screen as text and exits 1 when the stream holds anything the interpreter
does not know, when nothing was drawn, or when lssim does not quit.
"""
import re
import subprocess
import sys
import threading
import time

KEYS = {"up": b"\x1b[A", "down": b"\x1b[B", "right": b"\x1b[C", "left": b"\x1b[D",
        "enter": b"\r", "esc": b"\x1b", "tab": b"\t", "f1": b"\x1bOP", "f2": b"\x1bOQ",
        "f3": b"\x1bOR", "f4": b"\x1bOS", "f5": b"\x1b[15~", "backspace": b"\x7f"}


class Screen:
    def __init__(self):
        self.cells = {}
        self.r = self.c = 0
        self.fg = self.bg = None
        self.unknown = []

    def feed(self, data):
        text = data.decode("utf-8", "replace")
        i = 0
        while i < len(text):
            ch = text[i]
            if ch == "\x1b":
                m = re.match(r"\x1b\[([?<]?)([\d;]*)([A-Za-z~])", text[i:])
                if m:
                    self.csi(m.group(1), m.group(2), m.group(3))
                    i += m.end()
                    continue
                m = re.match(r"\x1b\][^\x07]*\x07", text[i:])
                if m:
                    i += m.end()
                    continue
                self.unknown.append(repr(text[i:i + 8]))
                i += 1
                continue
            self.cells[(self.r, self.c)] = (ch, self.fg, self.bg)
            self.c += 1
            i += 1

    def csi(self, priv, params, final):
        p = [int(x) for x in params.split(";") if x] if params else []
        if final == "H" and not priv:
            self.r, self.c = (p[0] - 1 if p else 0), (p[1] - 1 if len(p) > 1 else 0)
        elif final == "m" and not priv:
            if p[:2] == [38, 2] and len(p) >= 10 and p[5:7] == [48, 2]:
                self.fg, self.bg = tuple(p[2:5]), tuple(p[7:10])
            elif p[:2] == [38, 2] and len(p) == 5:      # only the colour that changed
                self.fg = tuple(p[2:5])
            elif p[:2] == [48, 2] and len(p) == 5:
                self.bg = tuple(p[2:5])
            elif p in ([], [0]):
                self.fg = self.bg = None
            else:
                self.unknown.append("SGR %s" % params)
        elif final == "J" and p == [2]:
            self.cells.clear()
        elif priv == "?" and final in "hl":
            pass                                   # modes: alt screen, cursor, wrap, mouse, sync
        elif final == "t":
            pass                                   # window size request
        else:
            self.unknown.append("CSI %s%s%s" % (priv, params, final))

    def text(self):
        if not self.cells:
            return ""
        rows = max(r for r, _ in self.cells) + 1
        cols = max(c for _, c in self.cells) + 1
        return "\n".join("".join(self.cells.get((r, c), (" ",))[0] for c in range(cols)).rstrip()
                         for r in range(rows))


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    exe = sys.argv[1]
    app = sys.argv[2] if len(sys.argv) > 2 and not sys.argv[2].startswith("--") else "home"
    keys = sys.argv[sys.argv.index("--keys") + 1].split(",") if "--keys" in sys.argv else []
    taps = sys.argv[sys.argv.index("--tap") + 1].split(",") if "--tap" in sys.argv else []
    p = subprocess.Popen([exe, app, "-l", "--term"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL)
    got = bytearray()
    t = threading.Thread(target=lambda: [got.extend(b) for b in iter(lambda: p.stdout.read(4096), b"")],
                         daemon=True)
    t.start()
    time.sleep(1.5)
    for k in keys:
        p.stdin.write(KEYS.get(k, k.encode())); p.stdin.flush(); time.sleep(0.4)
    for tap in taps:
        c, r = (int(x) for x in tap.split(":"))
        p.stdin.write(b"\x1b[<0;%d;%dM\x1b[<0;%d;%dm" % (c + 1, r + 1, c + 1, r + 1)); p.stdin.flush()
        time.sleep(0.6)
    time.sleep(0.8)
    p.stdin.write(b"\x1d"); p.stdin.flush()
    try:
        p.wait(5)
        quit_ok = True
    except subprocess.TimeoutExpired:
        p.kill()
        quit_ok = False
    t.join(2)
    s = Screen()
    s.feed(bytes(got))
    out = s.text()
    if "--dump" in sys.argv or True:
        print(out)
    print("bytes=%d cells=%d unknown=%d quit=%s" % (len(got), len(s.cells), len(s.unknown), quit_ok))
    for u in s.unknown[:5]:
        print("  unknown:", u)
    left = got.endswith(b"\x1b[?1049l")
    print("restored terminal:", left)
    expect = sys.argv[sys.argv.index("--expect") + 1] if "--expect" in sys.argv else ""
    seen = expect in out
    if expect:
        print("expected %r on screen: %s" % (expect, seen))
    sys.exit(0 if s.cells and not s.unknown and quit_ok and left and seen else 1)


if __name__ == "__main__":
    main()
