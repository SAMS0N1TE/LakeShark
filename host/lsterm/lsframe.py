"""lsterm's copy of the board's screen, and what it makes of it.

The board draws its grid from row 1, column 1 with absolute cursor moves.
Screen keeps a copy of that grid (for snapshots and recordings) and moves
the board's output to wherever the frame put the grid. The frame and its
panel are lspanel.py.

Captures go to ~/LakeShark captures:
  SNAPSHOT     the screen as lsterm has it: .png, .ans (24-bit ANSI), .txt
  RECORD       an asciinema .cast of the board's stream, and an animated .gif
               made from it when the recording stops
"""
import base64
import json
import os
import re
import threading
import time
from pathlib import Path

CAPTURES = Path.home() / "LakeShark captures"

QUAD = " ▘▝▀▖▌▞▛▗▚▐▜▄▙▟█"         # 2x2 bits: 1 top left, 2 top right, 4 bottom left, 8 bottom right

_CSI = re.compile(rb"\x1b\[([0-9;?<]*)([ -/]*)([@-~])")
_CSI_OPEN = re.compile(rb"\x1b\[[0-9;?<]*[ -/]*")
_OSC = re.compile(rb"\x1b\][^\x07]*\x07")
_MOUSE = re.compile(rb"\x1b\[<(\d+);(\d+);(\d+)([Mm])")


def _quadrant_of_sextant(cp):
    i = cp - 0x1FB00
    bits = i + 1
    bits += bits >= 21
    bits += bits >= 42
    return QUAD[(1 if bits & 5 else 0) | (2 if bits & 10 else 0) | (4 if bits & 16 else 0) | (8 if bits & 32 else 0)]


class Screen:
    """The board's grid as the board drew it, and its output moved to (ox, oy)."""

    def __init__(self, cols, rows):
        self.ox = self.oy = 1                    # where the grid's top left lands, 1-based
        self.clip_w = self.clip_h = 10 ** 6      # terminal cells past which nothing is drawn
        self.fold = True                         # sextants out as quadrants
        self.on_size = None                      # called with (cols, rows) when the board says
        self.resize(cols, rows)
        self.pending = b""

    def resize(self, cols, rows):
        self.cols, self.rows = cols, rows
        self.cells = [[(" ", 0xC0C0C0, 0)] * cols for _ in range(rows)]
        self.r = self.c = 0
        self.fg, self.bg = 0xC0C0C0, 0

    def feed(self, data):
        """Board bytes in; bytes for this terminal out."""
        b = self.pending + data
        self.pending = b""
        out = bytearray()
        i, n = 0, len(b)
        while i < n:
            x = b[i]
            if x == 0x1B:
                kind = b[i + 1:i + 2]
                if kind == b"[":
                    m = _CSI.match(b, i)
                    if m:
                        out += self._csi(m.group(1).decode(), m.group(3).decode())
                        i = m.end()
                        continue
                    if _CSI_OPEN.fullmatch(b, i):          # cut off at the end of this read
                        self.pending = b[i:]
                        break
                elif kind == b"]":
                    m = _OSC.match(b, i)
                    if m:
                        i = m.end()
                        continue
                    if b.find(b"\x07", i) < 0:
                        self.pending = b[i:]
                        break
                elif not kind:
                    self.pending = b[i:]
                    break
                i += 1
                continue
            if x < 0x20:
                i += 1
                continue
            k = 1 if x < 0x80 else 2 if x < 0xE0 else 3 if x < 0xF0 else 4
            if i + k > n:
                self.pending = b[i:]
                break
            ch = b[i:i + k].decode("utf-8", "replace")
            i += k
            if 0 <= self.r < self.rows and 0 <= self.c < self.cols:
                self.cells[self.r][self.c] = (ch, self.fg, self.bg)
                if self.c + self.ox - 1 < self.clip_w and self.r + self.oy - 1 < self.clip_h:
                    shown = _quadrant_of_sextant(ord(ch)) if self.fold and 0x1FB00 <= ord(ch) <= 0x1FB3B else ch
                    out += shown.encode()
            self.c += 1
        return bytes(out)

    def _csi(self, params, final):
        if final == "H" and not params.startswith(("?", "<")):
            p = params.split(";")
            self.r = int(p[0] or 1) - 1
            self.c = (int(p[1] or 1) if len(p) > 1 else 1) - 1
            return b"\x1b[%d;%dH" % (self.r + self.oy, self.c + self.ox)
        if final == "m":
            p = [int(v) for v in params.split(";") if v] or [0]
            j = 0
            while j < len(p):
                if p[j] in (38, 48) and j + 4 < len(p) and p[j + 1] == 2:
                    v = p[j + 2] << 16 | p[j + 3] << 8 | p[j + 4]
                    if p[j] == 38:
                        self.fg = v
                    else:
                        self.bg = v
                    j += 5
                elif p[j] == 0:
                    self.fg, self.bg = 0xC0C0C0, 0
                    j += 1
                else:
                    j += 1
            return b"\x1b[" + params.encode() + b"m"
        if final == "t" and params.startswith("8;"):
            rows, cols = (int(v) for v in params.split(";")[1:3])
            if (cols, rows) != (self.cols, self.rows):
                self.resize(cols, rows)
                if self.on_size:
                    self.on_size(cols, rows)
            return b""
        if final == "J":                          # the board clearing: its repaint follows
            self.cells = [[(" ", 0xC0C0C0, 0)] * self.cols for _ in range(self.rows)]
            return b""
        if params in ("?2026",) and final in "hl":
            return b"\x1b[?2026" + final.encode()
        return b""                                # modes, titles: the frame's own

    def ansi(self, ox=1, oy=1, fold=None):
        """The whole grid as terminal bytes: what a recording starts from."""
        fold = self.fold if fold is None else fold
        out = bytearray(b"\x1b[0m")
        fg = bg = None
        for r, row in enumerate(self.cells):
            out += b"\x1b[%d;%dH" % (r + oy, ox)
            for ch, f, g in row:
                if (f, g) != (fg, bg):
                    out += b"\x1b[38;2;%d;%d;%d;48;2;%d;%d;%dm" % (f >> 16, f >> 8 & 255, f & 255, g >> 16, g >> 8 & 255, g & 255)
                    fg, bg = f, g
                out += (_quadrant_of_sextant(ord(ch)) if fold and 0x1FB00 <= ord(ch) <= 0x1FB3B else ch).encode()
        return bytes(out + b"\x1b[0m")

    def repaint(self):
        """The grid where the frame put it, clipped as feed() clips: after a
        clear the screen comes back from this copy at once, not cell by cell
        as the board next changes them."""
        out = bytearray()
        fg = bg = None
        for r, row in enumerate(self.cells):
            if r + self.oy - 1 >= self.clip_h:
                break
            out += b"\x1b[%d;%dH" % (r + self.oy, self.ox)
            for c, (ch, f, g) in enumerate(row):
                if c + self.ox - 1 >= self.clip_w:
                    break
                if (f, g) != (fg, bg):
                    out += b"\x1b[38;2;%d;%d;%d;48;2;%d;%d;%dm" % (f >> 16, f >> 8 & 255, f & 255, g >> 16, g >> 8 & 255, g & 255)
                    fg, bg = f, g
                out += (_quadrant_of_sextant(ord(ch)) if self.fold and 0x1FB00 <= ord(ch) <= 0x1FB3B else ch).encode()
        return bytes(out)

    def text(self):
        return "\n".join("".join(c[0] for c in row).rstrip() for row in self.cells)


# ---------------------------------------------------------------- pictures

def _font(size):
    from PIL import ImageFont
    for name in ("CascadiaMono.ttf", "consola.ttf", "DejaVuSansMono.ttf", "Menlo.ttc"):
        for folder in ("C:/Windows/Fonts", os.path.expanduser("~/AppData/Local/Microsoft/Windows/Fonts"),
                       "/usr/share/fonts/truetype/dejavu", "/System/Library/Fonts"):
            path = os.path.join(folder, name)
            if os.path.exists(path):
                return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def render(screen, cw=10, ch=18, font=None):
    """The grid as a picture. Block, sextant, braille and trace cells are
    drawn as the board draws them; letters come from a monospace font."""
    from PIL import Image, ImageDraw
    font = font or _font(ch - 3)
    img = Image.new("RGB", (screen.cols * cw, screen.rows * ch))
    d = ImageDraw.Draw(img)
    rgb = lambda v: (v >> 16, v >> 8 & 255, v & 255)
    for r, row in enumerate(screen.cells):
        y = r * ch
        c = 0
        while c < screen.cols:                   # the ground, a run of one colour at a time
            g = row[c][2]
            e = c
            while e + 1 < screen.cols and row[e + 1][2] == g:
                e += 1
            d.rectangle([c * cw, y, (e + 1) * cw - 1, y + ch - 1], fill=rgb(g))
            c = e + 1
        for c, (t, f, g) in enumerate(row):
            if t == " ":
                continue
            x, o, fc = c * cw, ord(t), rgb(f)
            if o == 0x2588:
                d.rectangle([x, y, x + cw - 1, y + ch - 1], fill=fc)
            elif o == 0x2580 or o == 0x2584:
                top = o == 0x2580
                d.rectangle([x, y if top else y + ch // 2, x + cw - 1, y + ch // 2 - 1 if top else y + ch - 1], fill=fc)
            elif 0x2596 <= o <= 0x259F or o in (0x258C, 0x2590, 0x2598, 0x259D):
                bits = QUAD.index(t) if t in QUAD else 0
                for q in range(4):
                    if bits & (1 << q):
                        qx, qy = q % 2, q // 2
                        d.rectangle([x + qx * cw // 2, y + qy * ch // 2, x + (qx + 1) * cw // 2 - 1,
                                     y + (qy + 1) * ch // 2 - 1], fill=fc)
            elif 0x1FB00 <= o <= 0x1FB3B:
                i = o - 0x1FB00
                bits = i + 1
                bits += bits >= 21
                bits += bits >= 42
                for q in range(6):
                    if bits & (1 << q):
                        qx, qy = q % 2, q // 2
                        d.rectangle([x + qx * cw // 2, y + qy * ch // 3, x + (qx + 1) * cw // 2 - 1,
                                     y + (qy + 1) * ch // 3 - 1], fill=fc)
            elif 0x2800 <= o <= 0x28FF:
                for k, (bx, by) in enumerate(((0, 0), (0, 1), (0, 2), (1, 0), (1, 1), (1, 2), (0, 3), (1, 3))):
                    if o & (1 << k):
                        px, py = x + (bx * 2 + 1) * cw // 4, y + (by * 2 + 1) * ch // 8
                        d.rectangle([px - 1, py - 1, px, py], fill=fc)
            elif o in (0x2502, 0x2577):
                d.rectangle([x + cw // 2 - 1, y if o == 0x2502 else y + ch // 2, x + cw // 2, y + ch - 1], fill=fc)
            else:
                d.text((x, y + 1), t, font=font, fill=fc)
    return img


def stamp():
    return time.strftime("%Y%m%d-%H%M%S")


def save_snapshot(screen):
    CAPTURES.mkdir(parents=True, exist_ok=True)
    base = CAPTURES / ("lakeshark-%s" % stamp())
    (base.with_suffix(".txt")).write_text(screen.text() + "\n", encoding="utf-8")
    (base.with_suffix(".ans")).write_bytes(screen.ansi() + b"\n")
    try:
        render(screen).save(base.with_suffix(".png"))
        return base.with_suffix(".png")
    except ImportError:
        return base.with_suffix(".ans")


class Recorder:
    """An asciinema v2 recording of the board's own stream, at the grid's size."""

    def __init__(self, screen):
        CAPTURES.mkdir(parents=True, exist_ok=True)
        self.path = CAPTURES / ("lakeshark-%s.cast" % stamp())
        self.f = open(self.path, "w", encoding="utf-8")
        self.t0 = time.time()
        self.size = (screen.cols, screen.rows)
        self.f.write(json.dumps({"version": 2, "width": screen.cols, "height": screen.rows,
                                 "timestamp": int(self.t0), "title": "LakeShark",
                                 "env": {"TERM": "xterm-256color"}}) + "\n")
        self.out(b"\x1b[?25l\x1b[2J" + screen.ansi())

    def out(self, data):
        self.f.write(json.dumps([round(time.time() - self.t0, 4), "o", data.decode("utf-8", "replace")]) + "\n")

    def close(self):
        self.f.close()
        return self.path


def cast_to_gif(path, fps=5, done=None):
    """The recording replayed into a grid and drawn every 1/fps seconds."""
    try:
        from PIL import Image  # noqa: F401
        lines = path.read_text(encoding="utf-8").splitlines()
        head = json.loads(lines[0])
        scr = Screen(head["width"], head["height"])
        scr.fold = False
        font = _font(15)
        frames, next_t, last = [], 0.0, 0.0
        for line in lines[1:]:
            t, kind, data = json.loads(line)
            while t >= next_t:
                frames.append(render(scr, font=font).convert("P", palette=1, colors=255))
                next_t += 1.0 / fps
            scr.feed(data.encode("utf-8"))
            last = t
        frames.append(render(scr, font=font).convert("P", palette=1, colors=255))
        gif = path.with_suffix(".gif")
        frames[0].save(gif, save_all=True, append_images=frames[1:], duration=int(1000 / fps), loop=0)
        if done:
            done("GIF ready: %s (%.0f s)" % (gif.name, last))
    except Exception as e:                        # a recording is kept even when the GIF fails
        if done:
            done("GIF failed: %s" % e)
