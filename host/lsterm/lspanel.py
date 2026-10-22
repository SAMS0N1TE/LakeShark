"""lsterm's frame: the board's screen in a border, and a panel beside it.

The panel is drawn the way the board draws (ruled boxes, solid keys, green
for what is on) in the board's own colours, read off its title bar, so it
follows THEME and DAYLIGHT. It has four tabs:

  CONTROL  captures, the screen, sound and brightness, the board's own
           settings, your own buttons (~/.lsterm.json), and what the open
           app is for
  CONSOLE  the board's console without leaving the screen: Tab completes
           commands, their words and card paths; Up and Down are history
  FILES    the SD card: open folders, GET, OPEN, PUT, NEW DIR, DELETE
  HELP     the open app's own help from the board, and its guide from docs/apps

Everything the panel asks the board is an ordinary console line; replies come
back as text between the board's frames. Lines the panel sends for itself
are kept out of CONSOLE.

Keys: Ctrl-] leaves, Ctrl-T CONSOLE, Ctrl-F FILES, Ctrl-G HELP, Ctrl-K back to
CONTROL and the board, Ctrl-P snapshot, Ctrl-R record. A click on the board
gives the keyboard back to the board.
"""
import base64
import json
import os
import re
import subprocess
import sys
import threading
import time
import zlib
from collections import deque
from pathlib import Path

import lsframe

CONFIG = Path.home() / ".lsterm.json"
DOCS = Path(__file__).resolve().parent.parent.parent / "docs" / "apps"
DOWNLOADS = lsframe.CAPTURES / "files"
SPIN = "⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏"
SPARK = " ▁▂▃▄▅▆▇█"

_TOKEN = re.compile(rb"\x1b\[<\d+;\d+;\d+[Mm]|\x1b\[[0-9;?]*[ -/]*[@-~]|\x1bO.|\x1b|[\x00-\x7f]|[\xc0-\xff][\x80-\xbf]*")
_MOUSE = re.compile(rb"\x1b\[<(\d+);(\d+);(\d+)([Mm])")
KEYS = {b"\x1b[A": "up", b"\x1b[B": "down", b"\x1b[C": "right", b"\x1b[D": "left", b"\x1b[H": "home",
        b"\x1b[F": "end", b"\x1b[3~": "del", b"\x1b[5~": "pgup", b"\x1b[6~": "pgdn", b"\r": "enter",
        b"\n": "enter", b"\t": "tab", b"\x7f": "bs", b"\x08": "bs", b"\x1b": "esc", b"\x15": "kill",
        b"\x17": "word", b"\x03": "ctrl-c"}
NAMED_KEYS = {"F%d" % (i + 1): s for i, s in enumerate(
    [b"\x1bOP", b"\x1bOQ", b"\x1bOR", b"\x1bOS", b"\x1b[15~", b"\x1b[17~", b"\x1b[18~", b"\x1b[19~",
     b"\x1b[20~", b"\x1b[21~", b"\x1b[23~"])}
NAMED_KEYS.update({"ENTER": b"\r", "ESC": b"\x1b", "TAB": b"\t", "UP": b"\x1b[A", "DOWN": b"\x1b[B",
                   "RIGHT": b"\x1b[C", "LEFT": b"\x1b[D"})


def rgb565(v):
    r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
    return ((r * 255 + 15) // 31) << 16 | ((g * 255 + 31) // 63) << 8 | ((b * 255 + 15) // 31)


def mix(a, b, n, d=4):
    return sum(((((a >> s) & 255) * n + ((b >> s) & 255) * (d - n)) // d) << s for s in (0, 8, 16))


class Theme:
    """The board's colours: Terminal Bay until the board's title bar says otherwise."""

    def __init__(self):
        self.bg, self.accent, self.line = 0x000000, rgb565(0x3C73), rgb565(0x3C73)
        self.on, self.warn, self.bad = rgb565(0x4C6B), rgb565(0xFE4A), rgb565(0xFB4D)
        self.inks()

    def inks(self):
        """Text that reads on this ground: light on the dark themes, dark under DAYLIGHT."""
        light = sum((self.bg >> s) & 255 for s in (0, 8, 16)) > 384
        self.text = 0x303030 if light else rgb565(0xAD97)
        self.bright = 0x000000 if light else rgb565(0xF7BF)
        self.dim = mix(self.text, self.bg, 2)
        self.key = mix(self.accent, self.bg, 1, 5)

    def follow(self, screen):
        """The title bar's ground is the theme's accent, the screen's commonest ground the
        theme's ground, the first ruled box its line colour."""
        if not screen.cells:
            return False
        grounds = {}
        for ch, f, g in screen.cells[0]:
            grounds[g] = grounds.get(g, 0) + 1
        accent = max(grounds, key=grounds.get)
        # The ground is what most of the screen sits on. The help bar along the
        # bottom has a tint of its own, and taken for the ground it hazed the
        # panel and the whole window around the board's black.
        under = {}
        for row in screen.cells[1:-1]:
            for ch, f, g in row:
                under[g] = under.get(g, 0) + 1
        ground = max(under, key=under.get) if under else 0
        line = next((f for row in screen.cells[1:4] for ch, f, g in row if ch in "+|"), self.line)
        if (accent, line, ground) == (self.accent, self.line, self.bg):
            return False
        self.accent, self.line, self.bg = accent, line, ground
        self.inks()
        return True


class Canvas:
    def __init__(self, w, h, fg, bg):
        self.w, self.h = w, h
        self.cells = [[(" ", fg, bg)] * w for _ in range(h)]

    def put(self, x, y, s, fg, bg=None, w=None):
        if not 0 <= y < self.h:
            return
        row = self.cells[y]
        for i, ch in enumerate(s[:w] if w is not None else s):
            if 0 <= x + i < self.w:
                row[x + i] = (ch, fg, row[x + i][2] if bg is None else bg)

    def fill(self, x, y, w, h, fg, bg, ch=" "):
        for r in range(max(0, y), min(self.h, y + h)):
            self.put(x, r, ch * w, fg, bg)

    def box(self, x, y, w, h, fg, title=None, tfg=None, heavy=False):
        if w < 2 or h < 2:
            return
        edge = "=" if heavy else "-"
        self.put(x, y, "+" + edge * (w - 2) + "+", fg)
        self.put(x, y + h - 1, "+" + edge * (w - 2) + "+", fg)
        for r in range(y + 1, y + h - 1):
            self.put(x, r, "|", fg)
            self.put(x + w - 1, r, "|", fg)
        if title and w > len(title) + 4:
            self.put(x + 2, y, " %s " % title, tfg or fg)

    def blit(self, other, x, y, top, rows):
        for r in range(rows):
            if 0 <= top + r < other.h and 0 <= y + r < self.h:
                for c, cell in enumerate(other.cells[top + r]):
                    if 0 <= x + c < self.w:
                        self.cells[y + r][x + c] = cell


def _sgr(fg, bg):
    return b"\x1b[38;2;%d;%d;%d;48;2;%d;%d;%dm" % (fg >> 16, fg >> 8 & 255, fg & 255, bg >> 16, bg >> 8 & 255, bg & 255)


class Painter:
    """Only the cells that changed since the last paint."""

    def __init__(self):
        self.last = None

    def paint(self, c, ox, oy):
        out = bytearray()
        state = None
        for y, row in enumerate(c.cells):
            old = self.last.cells[y] if self.last and self.last.w == c.w and self.last.h == c.h else None
            at = None
            for x, cell in enumerate(row):
                if old and old[x] == cell:
                    continue
                if at != x:
                    out += b"\x1b[%d;%dH" % (oy + y, ox + x)
                ch, fg, bg = cell
                if (fg, bg) != state:
                    out += _sgr(fg, bg)
                    state = (fg, bg)
                out += ch.encode()
                at = x + 1
        self.last = c
        return bytes(out)


def wrap(text, w):
    out = []
    for para in text.split("\n"):
        line = ""
        for word in para.split(" "):
            while len(word) > w:
                if line:
                    out.append(line)
                    line = ""
                out.append(word[:w])
                word = word[w:]
            if len(line) + len(word) + (1 if line else 0) > w:
                out.append(line)
                line = word
            else:
                line = line + " " + word if line else word
        out.append(line)
    return out


def size_text(n):
    if n is None:
        return "dir"
    for unit in ("B", "K", "M", "G"):
        if n < 1024 or unit == "G":
            return ("%d%s" if unit == "B" else "%.1f%s") % (n, unit)
        n /= 1024.0


def open_with_system(path):
    try:
        if os.name == "nt":
            os.startfile(str(path))
        elif sys.platform == "darwin":
            subprocess.Popen(["open", str(path)])
        else:
            subprocess.Popen(["xdg-open", str(path)])
        return True
    except Exception:
        return False


def load_buttons():
    try:
        data = json.loads(CONFIG.read_text(encoding="utf-8"))
        return [b for b in data.get("buttons", []) if b.get("label") and (b.get("send") or b.get("keys"))]
    except (OSError, ValueError):
        return []


def save_buttons(buttons):
    try:
        data = json.loads(CONFIG.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        data = {}
    data["buttons"] = buttons
    CONFIG.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")


EXAMPLE = {"buttons": [
    {"label": "BATTERY", "send": "gauge"},
    {"label": "GPS FIX", "send": "gps"},
    {"label": "RADIOS", "keys": "F3"},
], "_help": "Each button: a label, and 'send' (a console line, or a list of them) or 'keys' "
            "(F1-F11, ENTER, ESC, TAB, UP, DOWN, LEFT, RIGHT, or plain text typed on the board)."}


class Frame:
    TABS = ("CONTROL", "CONSOLE", "FILES", "HELP")

    def __init__(self, screen, title, device=None, key=None, link=""):
        self.scr = screen
        self.title = title
        self._device = device
        self._key = key
        self.link = link
        self.theme = Theme()
        self.painter = Painter()
        self.w = self.h = 0
        self.panel = None
        self.boxed = False
        self.need_clear = True
        self.quitting = False
        self.tab, self.focus = "CONTROL", "board"
        self.hits, self.hover, self.flash = [], None, {}
        self.toast_text, self.toast_t = "Ctrl-] leaves  ^T console  ^F files  ^G help", time.time()
        self.scroll = {t: 0 for t in self.TABS}
        self.content_h = {t: 0 for t in self.TABS}
        self.version = ""
        self.rate = (0.0, 0.0)
        self.spark = deque([0.0] * 20, maxlen=20)
        self.frames_at = 0.0
        self.bright = self.daylight = self.screen_off = self.muted = self.volume = None
        self.rec = None
        self.xfer = None                         # a PNG or a file arriving: dict
        self.upload = None                       # a file going to the card: dict
        # console
        self.lines = deque(maxlen=1000)          # (text, kind)
        self.inp, self.cur, self.hist, self.hidx = "", 0, [], None
        self.comp, self.comp_i = [], -1
        self.cmds, self.actions = {}, []
        self.quiet = deque()                     # lines the panel sent for itself
        self.block, self.block_quiet = "", False
        # files
        self.dir, self.entries, self.sel = "/sdcard", [], 0
        self.listing = None
        self.prompt = None                       # ("put" | "mkdir", label) while FILES asks for text
        self.confirm = 0.0
        self.last_click = (0.0, None)
        # help
        self.app, self.doc, self.guide, self.guide_path = "", {}, [], None
        self.buttons = load_buttons()
        self.polled = {}

    # ------------------------------------------------------------ talking

    def send(self, line, quiet=False):
        if not self._device:
            self.say("no console on this link")
            return
        if quiet:
            self.quiet.append(line)
        self._device(line)

    def key(self, data):
        if self._key:
            self._key(data)

    def say(self, text):
        self.toast_text, self.toast_t = text, time.time()

    def start(self):
        for line in ("version", "display", "tui daylight", "tel", "help", "call", "tui doc"):
            self.send(line, quiet=True)
        self.list_dir(self.dir)

    def poll(self):
        """Now and then: the sound and the screen, which change on the board too."""
        now = time.time()
        for line, every in (("tel", 4), ("display", 12)):
            if now - self.polled.get(line, now - every + 2) >= every:
                self.polled[line] = now
                self.send(line, quiet=True)

    # ------------------------------------------------------------ layout

    def layout(self, w, h):
        self.w, self.h = w, h
        cols, rows = self.scr.cols, self.scr.rows
        self.boxed = w >= cols + 2 and h >= rows + 2
        self.scr.ox, self.scr.oy = (2, 2) if self.boxed else (1, 1)
        self.scr.clip_w, self.scr.clip_h = w, h
        room = w - (cols + 2)
        if self.boxed and room >= 30:
            self.panel = (cols + 3, 1, min(room, 70), rows + 2)
        elif self.boxed and h - rows - 2 >= 18:
            self.panel = (1, rows + 3, w, h - rows - 2)
        else:
            self.panel = None
        self.need_clear = True

    def screen_grid_changed(self):
        if self.rec:
            self.record()
        self.layout(self.w, self.h)

    # ------------------------------------------------------------ painting

    def render(self):
        """What changed, as terminal bytes. Ends on the board's own colour,
        which its next frame takes for granted."""
        out = bytearray(b"\x1b[?2026h")
        # A new app often brings new colours. They repaint the frame, the
        # panel and the margins, never the whole terminal: a clear there
        # blanked the board's screen on every switch until its next frames
        # filled it in.
        restyled = self.theme.follow(self.scr)
        if self.need_clear:
            self.need_clear = False
            out += b"\x1b[0m" + _sgr(self.theme.text, self.theme.bg) + b"\x1b[2J"
            self.key(b"")                    # and the board sends it all again
            restyled = True
        elif restyled:
            out += self.margins()
        if restyled:
            self.painter = Painter()
            if self.boxed:
                c = Canvas(self.scr.cols + 2, self.scr.rows + 2, self.theme.line, self.theme.bg)
                c.box(0, 0, c.w, c.h, self.theme.line, heavy=True)
                out += Painter().paint(c, 1, 1)
            # The border's canvas fills its inside too, so the board's screen
            # goes back over it from the copy, in this same update.
            out += self.scr.repaint()
        if self.panel:
            x, y, w, h = self.panel
            out += self.painter.paint(self.paint(w, h), x, y)
        out += _sgr(self.scr.fg, self.scr.bg) + b"\x1b[?2026l"
        return bytes(out)

    def margins(self):
        """The terminal outside the frame and the panel, in the theme's ground."""
        right = self.scr.cols + (2 if self.boxed else 0)
        bottom = self.scr.rows + (2 if self.boxed else 0)
        if self.panel:
            x, y, w, h = self.panel
            right, bottom = max(right, x + w - 1), max(bottom, y + h - 1)
        out = bytearray(b"\x1b[0m" + _sgr(self.theme.text, self.theme.bg))
        if right < self.w:
            for r in range(1, min(bottom, self.h) + 1):
                out += b"\x1b[%d;%dH\x1b[K" % (r, right + 1)
        if bottom < self.h:
            out += b"\x1b[%d;1H\x1b[J" % (bottom + 1)
        return bytes(out)

    def paint(self, w, h):
        t = self.theme
        now = time.time()
        c = Canvas(w, h, t.text, t.bg)
        self.hits = []
        c.box(0, 0, w, h, t.line, title=self.title, tfg=t.bright)
        # the tab strip, like the board's own
        tw = (w - 2) // len(self.TABS)
        for i, name in enumerate(self.TABS):
            bx = 1 + i * tw
            bw = tw if i < len(self.TABS) - 1 else w - 1 - bx
            self.key_button(c, bx, 1, bw, name, lambda n=name: self.set_tab(n), "tab:" + name,
                            on=self.tab == name)
        top, bottom = 4, h - 2
        area_h = bottom - top
        # the last row: what just happened, fading
        age = now - self.toast_t
        fg = t.bright if age < 1.5 else t.text if age < 6 else t.dim
        c.put(2, h - 2, self.toast_text[:w - 4].ljust(w - 4), fg)
        c.put(1, h - 3, "-" * (w - 2), t.line)
        area_h -= 1
        page = getattr(self, "page_" + self.tab.lower())
        page(c, 2, top, w - 4, area_h)
        return c

    def scrolled(self, c, x, y, w, h, tab, draw):
        """Draw onto a tall canvas and show the part the wheel has scrolled to."""
        tall = Canvas(w, 400, self.theme.text, self.theme.bg)
        hits_before = len(self.hits)
        used = draw(tall, w)
        self.content_h[tab] = used
        top = max(0, min(self.scroll[tab], used - h))
        self.scroll[tab] = top
        c.blit(tall, x, y, top, h)
        moved = []
        for hx, hy, hw, hh, act, hid in self.hits[hits_before:]:
            hy2 = hy - top
            if hy2 + hh <= 0 or hy2 >= h:
                continue
            moved.append((hx + x, hy2 + y, hw, hh, act, hid))
        self.hits[hits_before:] = moved
        if used > h:                                          # where in it we are
            bar = max(1, h * h // used)
            pos = (h - bar) * top // max(1, used - h)
            for r in range(h):
                c.put(x + w, y + r, "|" if pos <= r < pos + bar else ":", self.theme.line if pos <= r < pos + bar else self.theme.dim)

    def key_button(self, c, x, y, w, label, act, hid, hint="", on=False, danger=False, disabled=False, rows=3):
        """A key the board would draw: a ruled box, a solid field, the word across it.
        It lights under the mouse and flashes when pressed."""
        t = self.theme
        now = time.time()
        pressed = self.flash.get(hid, 0) > now
        hover = self.hover == hid and not disabled
        if disabled:
            field, fg, edge = t.bg, t.dim, t.dim
        elif pressed:
            field, fg, edge = t.bright, t.bg, t.bright
        elif danger:
            field, fg, edge = t.bad, t.bg, t.bad
        elif on:
            field, fg, edge = t.on, t.bg, t.on
        elif hover:
            field, fg, edge = mix(t.accent, t.bg, 2), t.bright, t.accent
        else:
            field, fg, edge = t.key, t.text, t.line
        if rows >= 3:
            c.box(x, y, w, rows, edge)
            c.fill(x + 1, y + 1, w - 2, rows - 2, fg, field)
            mid = y + rows // 2
        else:
            c.fill(x, y, w, 1, fg, field)
            mid = y
        text = label[:max(0, w - 2 - (len(hint) + 1 if hint else 0))]
        c.put(x + (w - len(text)) // 2 if not hint else x + 1 + (w - 2 - len(hint) - len(text)) // 2, mid, text, fg, field)
        if hint and w > len(text) + len(hint) + 3:
            c.put(x + w - 1 - len(hint) - (1 if rows >= 3 else 0), mid, hint, t.dim if not (pressed or on or danger) else fg, field)
        if not disabled:
            self.hits.append((x, y, w, rows, act, hid))

    def grid(self, c, x, y, w, items, per_row=None):
        """Keys in rows: as many across as fit at a useful width."""
        per = per_row or (3 if w >= 54 else 2 if w >= 26 else 1)
        kw = (w - (per - 1)) // per
        for i, item in enumerate(items):
            col, row = i % per, i // per
            self.key_button(c, x + col * (kw + 1), y + row * 3, kw, *item)
        return y + ((len(items) + per - 1) // per) * 3

    def heading(self, c, x, y, w, text):
        c.put(x, y, ("-- %s " % text).ljust(w, "-"), self.theme.line)
        c.put(x + 3, y, text, self.theme.accent)
        return y + 1

    # ------------------------------------------------------------ CONTROL

    def page_control(self, c, x, y, w, h):
        self.scrolled(c, x, y, w, h, "CONTROL", self.draw_control)

    def draw_control(self, c, w):
        t = self.theme
        now = time.time()
        y = 0
        live = now - self.frames_at < 0.4
        c.put(0, y, ("●" if live else "○") + " " + self.link, t.on if live else t.dim)
        c.put(len(self.link) + 2, y, (" " + self.version)[:max(0, w - len(self.link) - 2)], t.text)
        y += 1
        ups, bps = self.rate
        top = max(self.spark) or 1
        spark = "".join(SPARK[min(8, int(v * 8 / top + 0.5))] for v in self.spark)
        info = "%dx%d  %2.0f fps %5.1f KB/s " % (self.scr.cols, self.scr.rows, ups, bps / 1000)
        c.put(0, y, info, t.text)
        c.put(len(info), y, spark[:max(0, w - len(info))], t.accent)
        y += 1
        sound = "?" if self.volume is None else "MUTED" if self.muted else "%d%%" % self.volume
        screen = "?" if self.screen_off is None else "off" if self.screen_off else "%s%%" % (self.bright if self.bright is not None else "?")
        c.put(0, y, "screen %s   sound %s   daylight %s" % (screen, sound, {True: "on", False: "off"}.get(self.daylight, "?")), t.text)
        y += 1
        if self.rec:
            blink = int(now * 2) % 2 == 0
            c.put(0, y, ("● " if blink else "  ") + "REC %ds  %s" % (now - self.rec.t0, self.rec.path.name), t.bad)
            y += 1
        y = self.progress(c, 0, y, w)
        y += 1
        y = self.heading(c, 0, y, w, "CAPTURE")
        y = self.grid(c, 0, y, w, [
            ("SNAPSHOT", self.snapshot, "snap", "^P"),
            ("DEVICE PNG", self.device_png, "dpng", "", False, False, self._device is None),
            ("STOP REC" if self.rec else "RECORD", self.record, "rec", "^R", self.rec is not None and int(now * 2) % 2 == 0),
        ])
        y = self.heading(c, 0, y + 1, w, "DEVICE")
        dev = self._device is None
        y = self.grid(c, 0, y, w, [
            ("SCREEN ON" if self.screen_off else "SCREEN OFF", self.toggle_screen, "scr", "", bool(self.screen_off), False, dev),
            ("UNMUTE" if self.muted else "MUTE", self.toggle_mute, "mute", "", bool(self.muted), False, dev),
            ("VOL -", lambda: self.volume_step(-5), "vol-", "", False, False, dev),
            ("VOL +", lambda: self.volume_step(5), "vol+", "", False, False, dev),
            ("BRIGHT -", lambda: self.brightness(-10), "br-", "", False, False, dev),
            ("BRIGHT +", lambda: self.brightness(10), "br+", "", False, False, dev),
            ("THEME", lambda: self.key(b"\x1b[20~"), "theme", "F9"),
            ("DAYLIGHT", self.toggle_daylight, "day", "", bool(self.daylight), False, dev),
            ("ROTATE", lambda: self.send("display rotate"), "rot", "", False, False, dev),
            ("REDRAW", self.redraw, "redraw", "^L"),
        ])
        y = self.heading(c, 0, y + 1, w, "YOUR BUTTONS")
        mine = [(b["label"].upper()[:20], lambda b=b: self.custom(b), "my:%d" % i) for i, b in enumerate(self.buttons)]
        mine.append(("EDIT" if self.buttons else "ADD BUTTONS", self.edit_buttons, "my:edit"))
        mine.append(("RELOAD", self.reload_buttons, "my:reload"))
        y = self.grid(c, 0, y, w, mine)
        if self.app:
            y = self.heading(c, 0, y + 1, w, "NOW: " + self.app)
            for line in wrap(self.doc.get("purpose", "asking the board..."), w)[:4]:
                c.put(0, y, line, t.text)
                y += 1
            if self.doc.get("keys"):
                for line in wrap("keys: " + self.doc["keys"], w)[:2]:
                    c.put(0, y, line, t.dim)
                    y += 1
            c.put(0, y, "^G for the whole guide", t.dim)
            y += 1
        y += 1
        self.key_button(c, 0, y, w, "QUIT", self.quit, "quit", "^]")
        return y + 3

    def progress(self, c, x, y, w):
        t = self.theme
        job = self.xfer or self.upload
        if not job:
            return y
        spin = SPIN[int(time.time() * 10) % len(SPIN)]
        total, done = job.get("total") or 0, job.get("done", 0)
        label = "%s %s %s" % (spin, job["what"], job["name"])
        c.put(x, y, label[:w], t.accent)
        if total:
            bw = max(4, w - 12)
            fill = bw * min(done, total) // total
            c.put(x, y + 1, "[" + "#" * fill + "." * (bw - fill) + "] %3d%%" % (100 * done // total), t.on)
        return y + 2

    # ------------------------------------------------------------ CONSOLE

    def page_console(self, c, x, y, w, h):
        t = self.theme
        focused = self.focus == "console"
        box_h = 3
        comp_h = min(len(self.comp), 6) if self.comp else 0
        out_h = h - box_h - comp_h - 1
        # the output, newest at the bottom; the wheel goes back
        rows = []
        for text, kind in self.lines:
            colour = {"cmd": t.accent, "err": t.bad, "dim": t.dim}.get(kind, t.text)
            for part in wrap(text, w - 1) or [""]:
                rows.append((part, colour))
        back = max(0, min(self.scroll["CONSOLE"], len(rows) - out_h))
        self.scroll["CONSOLE"] = back
        shown = rows[max(0, len(rows) - out_h - back):len(rows) - back]
        for i, (text, colour) in enumerate(shown):
            c.put(x, y + out_h - len(shown) + i, text, colour)
        if back:
            c.put(x + w - 12, y, " %d more v " % back, t.bg, t.accent)
        # suggestions
        cy = y + out_h
        if comp_h:
            first = max(0, min(self.comp_i - comp_h + 1, len(self.comp) - comp_h)) if self.comp_i >= 0 else 0
            for i, (word, note) in enumerate(self.comp[first:first + comp_h]):
                sel = first + i == self.comp_i
                fg, bg = (t.bg, t.accent) if sel else (t.bright, t.key)
                c.fill(x, cy + i, w, 1, fg, bg)
                c.put(x + 1, cy + i, word, fg, bg)
                if len(word) + 4 < w:
                    c.put(x + len(word) + 3, cy + i, note[:w - len(word) - 4], t.bg if sel else t.dim, bg)
                self.hits.append((x, cy + i, w, 1, lambda k=first + i: self.take_completion(k), "comp:%d" % (first + i)))
            cy += comp_h
        # the line being typed
        iy = cy + 1
        edge = t.on if focused else t.line
        c.box(x, iy, w, 3, edge, title="CONSOLE" if focused else "CONSOLE - click or ^T", tfg=edge)
        shown_from = max(0, self.cur - (w - 6))
        text = self.inp[shown_from:shown_from + w - 5]
        c.put(x + 1, iy + 1, "> ", t.accent)
        c.put(x + 3, iy + 1, text, t.bright)
        if focused and int(time.time() * 2) % 2 == 0:
            ch = (self.inp[self.cur:self.cur + 1] or " ")
            c.put(x + 3 + self.cur - shown_from, iy + 1, ch, t.bg, t.bright)
        self.hits.append((x, iy, w, 3, self.focus_console, "input"))
        if self.inp and focused:
            label = " SAVE AS BUTTON "
            c.put(x + w - len(label) - 1, iy + 2, label, t.bg, t.accent)
            self.hits.append((x + w - len(label) - 1, iy + 2, len(label), 1, self.save_as_button, "savebtn"))

    # ------------------------------------------------------------ FILES

    def page_files(self, c, x, y, w, h):
        t = self.theme
        kw = (w - 3) // 4
        self.key_button(c, x, y, kw, "UP", self.dir_up, "f:up", "BS")
        self.key_button(c, x + kw + 1, y, kw, "REFRESH", lambda: self.list_dir(self.dir), "f:ref", "R")
        self.key_button(c, x + 2 * (kw + 1), y, kw, "NEW DIR", lambda: self.ask("mkdir", "new folder name"), "f:mk", "N")
        self.key_button(c, x + 3 * (kw + 1), y, w - 3 * (kw + 1), "PUT", lambda: self.ask("put", "file on this computer"), "f:put", "P")
        y += 3
        c.put(x, y, self.dir[-w:], t.accent)
        if self.listing:
            c.put(x + w - 2, y, SPIN[int(time.time() * 10) % len(SPIN)], t.accent)
        y += 1
        acts_h = 3 + (2 if (self.xfer or self.upload) else 0) + (3 if self.prompt else 0)
        list_h = max(3, h - 4 - acts_h)               # under the keys and the path, over the actions
        entries = self.entries
        if self.sel >= len(entries):
            self.sel = max(0, len(entries) - 1)
        top = self.scroll["FILES"]
        if self.sel < top:
            top = self.sel
        if self.sel >= top + list_h:
            top = self.sel - list_h + 1
        top = max(0, min(top, max(0, len(entries) - list_h)))
        self.scroll["FILES"] = top
        for i, (name, size) in enumerate(entries[top:top + list_h]):
            k = top + i
            sel = k == self.sel
            hov = self.hover == "file:%d" % k
            fg, bg = (t.bg, t.on) if sel else (t.bright, mix(t.accent, t.bg, 1)) if hov else (t.text, t.bg)
            c.fill(x, y + i, w, 1, fg, bg)
            label = ("> %s/" % name) if size is None else ("  " + name)
            c.put(x, y + i, label[:w - 9], fg if size is not None or sel else t.accent if not hov else fg, bg)
            c.put(x + w - 8, y + i, size_text(size).rjust(8), fg if sel else t.dim, bg)
            self.hits.append((x, y + i, w, 1, lambda k=k: self.click_file(k), "file:%d" % k))
        if not entries:
            c.put(x, y, "(empty)" if not self.listing else "reading the card...", t.dim)
        y += list_h
        if self.prompt:
            kind, label = self.prompt
            c.box(x, y, w, 3, t.on, title=label, tfg=t.on)
            shown_from = max(0, self.cur - (w - 6))
            c.put(x + 1, y + 1, "> " + self.inp[shown_from:shown_from + w - 5], t.bright)
            if int(time.time() * 2) % 2 == 0:
                c.put(x + 3 + self.cur - shown_from, y + 1, (self.inp[self.cur:self.cur + 1] or " "), t.bg, t.bright)
            y += 3
        y = self.progress(c, x, y, w)
        cur = entries[self.sel] if entries else None
        is_file = cur is not None and cur[1] is not None
        confirming = time.time() < self.confirm
        kw = (w - 2) // 3
        self.key_button(c, x, y, kw, "GET", self.get_file, "f:get", "G", False, False, not is_file)
        self.key_button(c, x + kw + 1, y, kw, "OPEN", self.open_file, "f:open", "Enter", False, False, cur is None)
        self.key_button(c, x + 2 * (kw + 1), y, w - 2 * (kw + 1), "SURE?" if confirming else "DELETE",
                        self.delete_file, "f:rm", "Del", False, confirming, cur is None)

    # ------------------------------------------------------------ HELP

    def page_help(self, c, x, y, w, h):
        self.scrolled(c, x, y, w, h, "HELP", self.draw_help)

    def draw_help(self, c, w):
        t = self.theme
        y = 0
        c.put(0, y, self.app or "(no app yet)", t.bright)
        y += 2
        if self.doc:
            for label, key in (("", "purpose"), ("KEYS", "keys"), ("KEEPS", "keeps"), ("GPS", "gps")):
                if not self.doc.get(key):
                    continue
                if label:
                    c.put(0, y, label, t.accent)
                    y += 1
                for line in wrap(self.doc[key], w):
                    c.put(0, y, line, t.text)
                    y += 1
                y += 1
        if self.guide_path:
            y = self.heading(c, 0, y, w, "GUIDE " + self.guide_path.name)
            self.key_button(c, 0, y, min(w, 24), "OPEN GUIDE", lambda: open_with_system(self.guide_path), "h:open")
            y += 4
            for text, kind in self.guide:
                colour = {"h": t.accent, "code": t.on, "dim": t.dim}.get(kind, t.text)
                for line in wrap(text, w) if kind != "code" else [text[:w]]:
                    c.put(0, y, line, colour)
                    y += 1
                    if y > 390:
                        return y
        y = self.heading(c, 0, y + 1, w, "LSTERM")
        for line in ("Ctrl-]  leave        Ctrl-L  repaint",
                     "Ctrl-T  console      Ctrl-F  files",
                     "Ctrl-G  this help    Ctrl-K  back to the board",
                     "Ctrl-P  snapshot     Ctrl-R  record",
                     "A click on the board gives it the keyboard."):
            c.put(0, y, line[:w], t.dim)
            y += 1
        return y

    def load_guide(self):
        """docs/apps/<APP>.md for the open app, by its name or its id."""
        self.guide, self.guide_path = [], None
        want = {re.sub(r"[^A-Z0-9]", "", s.upper()) for s in (self.app, self.doc.get("id", "")) if s}
        try:
            files = list(DOCS.glob("*.md"))
        except OSError:
            files = []
        for f in files:
            if re.sub(r"[^A-Z0-9]", "", f.stem.upper()) in want:
                self.guide_path = f
                break
        if not self.guide_path:
            return
        fence = False
        for raw in self.guide_path.read_text(encoding="utf-8", errors="replace").splitlines():
            if raw.startswith("```"):
                fence = not fence
                continue
            if fence:
                self.guide.append(("  " + raw, "code"))
            elif raw.startswith("#"):
                self.guide.append(("", ""))
                self.guide.append((raw.lstrip("# ").upper(), "h"))
            elif raw.strip().startswith(("- ", "* ")):
                self.guide.append(("  " + raw.strip()[2:].replace("**", "").replace("`", ""), ""))
            elif raw.strip():
                self.guide.append((raw.strip().replace("**", "").replace("`", ""), ""))
            elif self.guide and self.guide[-1] != ("", ""):
                self.guide.append(("", ""))

    # ------------------------------------------------------------ the app on screen

    def follow_app(self):
        title = "".join(ch for ch, f, g in self.scr.cells[0]) if self.scr.cells else ""
        m = re.search(r"LAKESHARK\s+(\S+(?: \S+)?)", title)
        name = m.group(1).strip() if m else ""
        name = re.split(r"\s{2,}|---", name)[0].strip()
        if name and name != self.app:
            self.app, self.doc = name, {}
            self.load_guide()
            self.scroll["HELP"] = 0
            self.send("tui doc", quiet=True)

    # ------------------------------------------------------------ input

    def set_tab(self, name):
        self.tab = name
        self.focus = {"CONSOLE": "console", "FILES": "files"}.get(name, "board")
        if name == "FILES" and not self.entries and not self.listing:
            self.list_dir(self.dir)

    def focus_console(self):
        self.tab, self.focus = "CONSOLE", "console"

    def press(self, hid):
        self.flash[hid] = time.time() + 0.18

    def route(self, keys):
        """Keys from the terminal: the frame takes its own and the focused
        tab's; what is left goes to the board."""
        board = bytearray()
        for tok in _TOKEN.findall(keys):
            m = _MOUSE.fullmatch(tok)
            if m:
                board += self.mouse(int(m.group(1)), int(m.group(2)), int(m.group(3)), m.group(4) == b"m")
                continue
            if tok == b"\x1d":
                self.quitting = True
                continue
            if tok == b"\x10":
                self.snapshot()
                continue
            if tok == b"\x12":
                self.record()
                continue
            if tok == b"\x14":
                self.focus_console() if self.focus != "console" else self.set_tab("CONTROL")
                continue
            if tok == b"\x06":
                self.set_tab("FILES")
                continue
            if tok == b"\x07":
                self.set_tab("HELP")
                continue
            if tok == b"\x0b":
                self.set_tab("CONTROL")
                continue
            if tok == b"\x0c":
                self.need_clear = True
                board += tok
                continue
            if self.focus == "console" or self.prompt:
                self.edit(tok)
            elif self.focus == "files":
                self.files_key(tok)
            elif tok == b"\x03":
                self.quitting = True
            else:
                board += tok
        return bytes(board)

    def mouse(self, b, x, y, up):
        gx, gy = x - self.scr.ox + 1, y - self.scr.oy + 1
        on_board = 1 <= gx <= self.scr.cols and 1 <= gy <= self.scr.rows
        if b & 32:                                       # motion: what the mouse is over
            self.hover = None
            if not on_board and self.panel:
                for hx, hy, hw, hh, act, hid in self.hits:
                    if hx <= x - self.panel[0] < hx + hw and hy <= y - self.panel[1] < hy + hh:
                        self.hover = hid
                        break
            return b""
        if on_board:
            if not up and b == 0 and self.focus != "board":
                self.focus = "board"
                if self.tab == "CONSOLE" or self.prompt:
                    self.prompt = None
            return b"\x1b[<%d;%d;%d%s" % (b, gx, gy, b"m" if up else b"M")
        if not self.panel:
            return b""
        px, py = x - self.panel[0], y - self.panel[1]
        if b in (64, 65):
            step = -3 if b == 64 else 3
            if self.tab == "CONSOLE":
                self.scroll["CONSOLE"] = max(0, self.scroll["CONSOLE"] - step)
            elif self.tab == "FILES":
                self.sel = max(0, min(len(self.entries) - 1, self.sel + step))
            else:
                self.scroll[self.tab] = max(0, self.scroll[self.tab] + step)
            return b""
        if up or b != 0:
            return b""
        for hx, hy, hw, hh, act, hid in reversed(self.hits):
            if hx <= px < hx + hw and hy <= py < hy + hh:
                self.press(hid)
                act()
                break
        return b""

    # ---- the line editor (CONSOLE, and FILES' questions)

    def edit(self, tok):
        k = KEYS.get(tok)
        if k == "enter":
            self.submit()
        elif k == "esc":
            if self.comp:
                self.comp, self.comp_i = [], -1
            elif self.prompt:
                self.prompt, self.inp, self.cur = None, "", 0
            else:
                self.focus = "board"
                self.set_tab("CONTROL")
        elif k == "tab":
            self.complete()
            return
        elif k == "bs":
            if self.cur:
                self.inp = self.inp[:self.cur - 1] + self.inp[self.cur:]
                self.cur -= 1
        elif k == "del":
            self.inp = self.inp[:self.cur] + self.inp[self.cur + 1:]
        elif k == "left":
            self.cur = max(0, self.cur - 1)
        elif k == "right":
            self.cur = min(len(self.inp), self.cur + 1)
        elif k == "home":
            self.cur = 0
        elif k == "end":
            self.cur = len(self.inp)
        elif k in ("kill", "ctrl-c"):
            self.inp, self.cur = "", 0
        elif k == "word":
            i = self.inp.rstrip()[:self.cur].rfind(" ") + 1
            self.inp, self.cur = self.inp[:i] + self.inp[self.cur:], i
        elif k in ("up", "down") and not self.prompt:
            if self.comp:
                self.comp_i = (self.comp_i + (1 if k == "down" else -1)) % len(self.comp)
                return
            if not self.hist:
                return
            self.hidx = (len(self.hist) if self.hidx is None else self.hidx) + (-1 if k == "up" else 1)
            self.hidx = max(0, min(len(self.hist), self.hidx))
            self.inp = self.hist[self.hidx] if self.hidx < len(self.hist) else ""
            self.cur = len(self.inp)
        elif k is None and tok[:1] != b"\x1b" and tok >= b" ":
            ch = tok.decode("utf-8", "replace")
            self.inp = self.inp[:self.cur] + ch + self.inp[self.cur:]
            self.cur += len(ch)
        else:
            return
        self.suggest()

    def submit(self):
        line = self.inp.strip()
        if self.comp and self.comp_i >= 0:
            self.take_completion(self.comp_i)
            return
        self.inp, self.cur, self.comp, self.comp_i, self.hidx = "", 0, [], -1, None
        if self.prompt:
            kind, _ = self.prompt
            self.prompt = None
            if line and kind == "mkdir":
                self.send("sd mkdir %s/%s" % (self.dir, line), quiet=True)
                self.say("making %s" % line)
            elif line and kind == "put":
                self.put_file(line)
            return
        if not line:
            return
        if not self.hist or self.hist[-1] != line:
            self.hist.append(line)
        self.scroll["CONSOLE"] = 0
        self.send(line)

    # ---- completion

    def words_for(self, words):
        """What may come next after these words."""
        if not words:
            return [(n, self.cmds[n][0] or self.cmds[n][1]) for n in sorted(self.cmds)]
        name = words[0]
        if name == "call" and len(words) == 1:
            return self.actions
        if name in ("sd",) and len(words) >= 2 and words[1] in ("ls", "cat", "get", "rm"):
            return [((self.dir + "/" + n) + ("/" if s is None else ""), size_text(s)) for n, s in self.entries]
        if len(words) == 1 and name in self.cmds:
            hint, text = self.cmds[name]
            subs = []
            for word in re.findall(r"'%s ([a-z0-9_.\-]+)" % re.escape(name), text):
                if word not in subs:
                    subs.append(word)
            for group in re.findall(r"\[([^\]]*)\]", hint):
                for word in re.split(r"[| ]+", group):
                    if re.fullmatch(r"[a-z][a-z0-9_.\-]*", word) and word not in subs:
                        subs.append(word)
            if not subs:
                for word in re.split(r"[|:,]\s*", text):
                    w = word.strip().split(" ")[0]
                    if re.fullmatch(r"[a-z][a-z0-9_\-]*", w) and w not in subs and len(subs) < 20:
                        subs.append(w)
            return [(s, "") for s in subs]
        return []

    def local_paths(self, text):
        import glob
        base = os.path.expanduser(text or "")
        out = []
        for p in sorted(glob.glob(base + "*"))[:40]:
            out.append((p + (os.sep if os.path.isdir(p) else ""), "folder" if os.path.isdir(p) else size_text(os.path.getsize(p))))
        return out

    def candidates(self):
        before = self.inp[:self.cur]
        if self.prompt:
            return (self.local_paths(before) if self.prompt[0] == "put" else []), before
        words = before.split(" ")
        part = words[-1]
        return [(w, n) for w, n in self.words_for([w for w in words[:-1] if w]) if w.startswith(part)], part

    def suggest(self):
        if not self.inp[:self.cur].strip() and not self.prompt:
            self.comp, self.comp_i = [], -1
            return
        found, part = self.candidates()
        self.comp = found[:30] if found and not (len(found) == 1 and found[0][0] == part) else []
        self.comp_i = -1

    def complete(self):
        found, part = self.candidates()
        if not found:
            self.say("nothing to complete")
            return
        if len(found) == 1:
            self.insert_word(found[0][0], part)
            return
        common = os.path.commonprefix([w for w, n in found])
        if len(common) > len(part):
            self.insert_word(common, part, final=False)
        else:
            self.comp = found[:30]
            self.comp_i = (self.comp_i + 1) % len(self.comp)

    def take_completion(self, k):
        if 0 <= k < len(self.comp):
            found, part = self.candidates()
            self.insert_word(self.comp[k][0], part)

    def insert_word(self, word, part, final=True):
        start = self.cur - len(part)
        tail = "" if not final or word.endswith(("/", os.sep)) else " "
        self.inp = self.inp[:start] + word + tail + self.inp[self.cur:]
        self.cur = start + len(word) + len(tail)
        self.comp, self.comp_i = [], -1
        if not final or tail == "":
            self.suggest()

    # ------------------------------------------------------------ files

    def list_dir(self, path):
        self.listing = {"dir": path, "items": []}
        self.send("sd ls %s" % path, quiet=True)

    def dir_up(self):
        if self.dir.count("/") > 1:
            self.list_dir(self.dir.rsplit("/", 1)[0])

    def click_file(self, k):
        now = time.time()
        double = self.last_click[1] == k and now - self.last_click[0] < 0.45
        self.last_click = (now, k)
        self.sel = k
        self.focus = "files"
        if double:
            self.open_file()

    def files_key(self, tok):
        k = KEYS.get(tok)
        if k in ("up", "down"):
            self.sel = max(0, min(len(self.entries) - 1, self.sel + (1 if k == "down" else -1)))
        elif k in ("pgup", "pgdn"):
            self.sel = max(0, min(len(self.entries) - 1, self.sel + (10 if k == "pgdn" else -10)))
        elif k == "enter":
            self.open_file()
        elif k == "bs":
            self.dir_up()
        elif k == "del":
            self.delete_file()
        elif k == "esc":
            self.set_tab("CONTROL")
        elif tok in (b"g", b"G"):
            self.get_file()
        elif tok in (b"o", b"O"):
            self.open_file()
        elif tok in (b"n", b"N"):
            self.ask("mkdir", "new folder name")
        elif tok in (b"p", b"P"):
            self.ask("put", "file on this computer")
        elif tok in (b"r", b"R"):
            self.list_dir(self.dir)

    def selected(self):
        if not self.entries or self.sel >= len(self.entries):
            return None
        name, size = self.entries[self.sel]
        return "%s/%s" % (self.dir, name), size

    def open_file(self):
        cur = self.selected()
        if not cur:
            return
        path, size = cur
        if size is None:
            self.list_dir(path)
        else:
            self.get_file(then_open=True)

    def get_file(self, then_open=False):
        cur = self.selected()
        if not cur or cur[1] is None:
            return
        if self.xfer:
            self.say("one transfer at a time")
            return
        path, size = cur
        self.xfer = {"kind": "sd", "what": "GET", "name": path.rsplit("/", 1)[1], "path": path, "total": size,
                     "done": 0, "parts": [], "open": then_open, "t0": time.time()}
        self.send("sd get %s" % path, quiet=True)

    def delete_file(self):
        cur = self.selected()
        if not cur:
            return
        if time.time() > self.confirm:
            self.confirm = time.time() + 3
            self.say("DELETE again within 3 s to remove %s" % cur[0].rsplit("/", 1)[1])
            return
        self.confirm = 0
        self.send("sd rm %s" % cur[0], quiet=True)

    def ask(self, kind, label):
        self.prompt = (kind, label)
        self.inp, self.cur = ("" if kind == "mkdir" else os.path.expanduser("~") + os.sep), 0
        self.cur = len(self.inp)
        self.tab, self.focus = "FILES", "files"
        self.suggest()

    def put_file(self, local):
        local = os.path.expanduser(local)
        if not os.path.isfile(local):
            self.say("no such file: %s" % local)
            return
        rel = self.dir[len("/sdcard"):].strip("/")
        if rel.count("/") >= 1:
            self.say("PUT goes to the card's top or one folder down; this is deeper")
            return
        name = os.path.basename(local)
        remote = (rel + "/" if rel else "") + name
        data = base64.b64encode(open(local, "rb").read()).decode()
        self.upload = {"what": "PUT", "name": name, "remote": remote, "chunks": [data[i:i + 180] for i in range(0, len(data), 180)],
                       "next": 0, "total": len(data), "done": 0, "t0": time.time()}
        self.send("sd put %s" % remote, quiet=True)        # truncates; then a chunk per reply

    def upload_step(self):
        up = self.upload
        if up["next"] >= len(up["chunks"]):
            self.say("PUT %s: %s in %.1f s" % (up["name"], size_text(up["total"] * 3 // 4), time.time() - up["t0"]))
            self.upload = None
            self.list_dir(self.dir)
            return
        chunk = up["chunks"][up["next"]]
        up["next"] += 1
        up["done"] += len(chunk)
        self.send("sd put %s %s" % (up["remote"], chunk), quiet=True)

    # ------------------------------------------------------------ the board's replies

    def board_said(self, line):
        """A line of console text from between the board's frames."""
        if line.startswith("~") and self.xfer:
            self.xfer["parts"].append(line[1:])
            self.xfer["done"] += len(line[1:]) * 3 // 4
            return
        m = re.match(r"(?:\x1b\[[0-9;]*m)?lakeshark> ?(.*)", line)
        if m:                                           # the prompt, and the line it was given
            cmd = m.group(1).strip()
            self.block = cmd
            quiet = False
            if cmd.startswith("term "):
                quiet = True
            elif cmd in self.quiet:
                while self.quiet and self.quiet[0] != cmd:
                    self.quiet.popleft()
                self.quiet.popleft()
                quiet = True
            self.block_quiet = quiet
            if cmd and not quiet:
                self.lines.append(("> " + cmd, "cmd"))
            return
        self.understand(line)
        if not self.block_quiet and line.strip():
            bad = re.search(r"Unrecognized command|failed|refusing|usage|cannot|Invalid", line)
            self.lines.append((line.rstrip(), "err" if bad else ""))

    def understand(self, line):
        block = self.block
        m = re.match(r"LakeShark\S* (\S+)\s+board=(\S+)", line)
        if m:
            self.version = "%s %s" % (m.group(1), m.group(2))
        m = re.search(r"\bscreen=(on|off)\b", line)
        if m:
            self.screen_off = m.group(1) == "off"
        m = re.search(r"\bbrightness=(\d+)%", line)
        if m:
            self.bright = int(m.group(1))
        m = re.match(r"tui: daylight (on|off)", line)
        if m:
            self.daylight = m.group(1) == "on"
        m = re.search(r"\bv=(\d+)\b", line)
        if m and ("mu=" in line or line.startswith("+OK")):
            self.volume = int(m.group(1))
        m = re.search(r"\bmu=(\d)", line)
        if m:
            self.muted = m.group(1) == "1"
        m = re.match(r"doc: (\S+) (.*)", line)
        if m:
            self.doc = {"id": m.group(1), "name": m.group(2)}
            if not self.guide_path:
                self.load_guide()
            return
        m = re.match(r"doc (purpose|keys|keeps|gps): (.*)", line)
        if m:
            self.doc[m.group(1)] = m.group(2)
            return
        if block == "help":
            if line and not line[0].isspace():
                parts = line.split(None, 1)
                self.cmds[parts[0]] = [parts[1].strip() if len(parts) > 1 else "", ""]
                self._help_last = parts[0]
            elif line.strip() and getattr(self, "_help_last", None) in self.cmds:
                self.cmds[self._help_last][1] += (" " if self.cmds[self._help_last][1] else "") + line.strip()
        if block == "call":
            m = re.match(r"\s+([a-z0-9_.]+)\s+\(([^)]*)\)\s+\S+\s+(.*)", line)
            if m:
                self.actions.append((m.group(1), m.group(3).strip()))
        if block.startswith("sd ls") and self.listing is not None:
            m = re.match(r"  (.+?)\s+(<dir>|\d+|\?)\s*$", line)
            if m:
                name = m.group(1).strip()
                if name not in (".", ".."):
                    self.listing["items"].append((name, None if m.group(2) == "<dir>" else int(m.group(2)) if m.group(2).isdigit() else 0))
            elif line.startswith("sd: ") and " entries in " in line:
                items = sorted(self.listing["items"], key=lambda e: (e[1] is not None, e[0].lower()))
                self.dir, self.entries, self.sel, self.listing = self.listing["dir"], items, 0, None
                self.scroll["FILES"] = 0
            elif line.startswith("sd: cannot open") or line.startswith("sd: not mounted"):
                self.say(line[4:])
                self.listing = None
        if (line.startswith("sd: rm ") or line.startswith("sd: mkdir ")):
            self.say(line[4:])
            self.list_dir(self.dir)
        if self.upload and line.startswith("sd: ") and ("truncated" in line or "bytes ->" in line):
            self.upload_step()
        elif self.upload and line.startswith("sd: ") and ("bad base64" in line or "cannot" in line or "failed" in line or "refusing" in line):
            self.say("PUT failed: " + line[4:])
            self.upload = None
        if self.xfer and (line.startswith("tui: png end") or line.startswith("sd: get end")):
            self.finish_xfer(line)
        elif self.xfer and line.startswith("sd: cannot open"):
            self.say(line[4:])
            self.xfer = None

    def finish_xfer(self, line):
        job = self.xfer
        self.xfer = None
        try:
            data = b"".join(base64.b64decode(p) for p in job["parts"])
            m = re.search(r"crc ([0-9a-f]{8})", line)
            if job["kind"] == "sd" and m and int(m.group(1), 16) != zlib.crc32(data):
                self.say("GET %s: the CRC does not match, not saved" % job["name"])
                return
            folder = DOWNLOADS if job["kind"] == "sd" else lsframe.CAPTURES
            folder.mkdir(parents=True, exist_ok=True)
            path = folder / (job["name"] if job["kind"] == "sd" else job["name"] + "-device.png")
            path.write_bytes(data)
            took = time.time() - job["t0"]
            self.say("saved %s (%s, %.1f s)" % (path.name, size_text(len(data)), took))
            if job.get("open"):
                open_with_system(path)
        except Exception as e:
            self.say("transfer failed: %s" % e)

    # ------------------------------------------------------------ actions

    def snapshot(self):
        path = lsframe.save_snapshot(self.scr)
        self.say("saved %s" % path.name)

    def record(self):
        if self.rec:
            path = self.rec.close()
            self.rec = None
            self.say("saved %s; making GIF" % path.name)
            threading.Thread(target=lsframe.cast_to_gif, args=(path, 5, self.say), daemon=True).start()
        else:
            self.rec = lsframe.Recorder(self.scr)
            self.say("recording to %s" % lsframe.CAPTURES)

    def device_png(self):
        if self.xfer:
            self.say("one transfer at a time")
            return
        name = "lakeshark-%s" % lsframe.stamp()
        self.xfer = {"kind": "png", "what": "DEVICE PNG", "name": name, "total": 0, "done": 0, "parts": [], "t0": time.time()}
        self.send("tui png %s" % name, quiet=True)

    def redraw(self):
        self.key(b"\x0c")
        self.need_clear = True

    def toggle_screen(self):
        self.send("display %s" % ("on" if self.screen_off else "off"), quiet=True)
        self.screen_off = not self.screen_off
        self.say("the board's screen is %s" % ("off; a touch on it wakes it" if self.screen_off else "on"))

    def toggle_mute(self):
        self.send("fl MUTE %d" % (0 if self.muted else 1), quiet=True)

    def volume_step(self, step):
        self.send("fl VOL %+d" % step, quiet=True)

    def toggle_daylight(self):
        self.send("tui daylight %s" % ("off" if self.daylight else "on"), quiet=True)

    def brightness(self, step):
        level = max(5, min(100, (self.bright or 50) + step))
        self.send("display brightness %d" % level, quiet=True)

    def custom(self, b):
        if b.get("keys"):
            k = b["keys"]
            self.key(NAMED_KEYS.get(k.upper(), k.encode("utf-8").decode("unicode_escape").encode("latin1")))
            self.say("%s: keys sent" % b["label"])
        sends = b.get("send")
        if sends:
            for line in [sends] if isinstance(sends, str) else sends:
                self.send(line)
            self.say("%s: sent - CONSOLE shows the reply" % b["label"])

    def edit_buttons(self):
        if not CONFIG.exists():
            CONFIG.write_text(json.dumps(EXAMPLE, indent=2) + "\n", encoding="utf-8")
        self.say("opened %s; RELOAD when saved" % CONFIG)
        open_with_system(CONFIG)

    def reload_buttons(self):
        self.buttons = load_buttons()
        self.say("%d button%s from %s" % (len(self.buttons), "" if len(self.buttons) == 1 else "s", CONFIG.name))

    def save_as_button(self):
        line = self.inp.strip()
        if not line:
            return
        self.buttons.append({"label": line.split(" ")[0].upper() + (" " + line.split(" ")[1].upper() if " " in line else ""), "send": line})
        save_buttons(self.buttons)
        self.say("saved '%s' as a button on CONTROL" % line)

    def quit(self):
        self.quitting = True

    def tick(self, rate=None):
        """About ten times a second: the open app, the clock-driven bits, and
        whatever changed, painted."""
        if rate is not None:
            self.rate = rate
            self.spark.append(rate[1])
        self.follow_app()
        self.poll()
        if self.upload and time.time() - self.upload["t0"] > 300:
            self.say("PUT gave up")
            self.upload = None
        return self.render()
