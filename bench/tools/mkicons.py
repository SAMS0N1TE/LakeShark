"""Cut the home-screen icon art, and emit the C table for ls_icons.c.

LS-1051  Two problems, one cause.

The art was twelve by twelve, packed two-by-two into quadrant glyphs, so an
icon was six cells by six. A cell on this panel is 10 by 17 pixels, so a
"square" icon rendered 60 by 102 - stretched to one and seven tenths its
width. Every one of them was drawn on graph paper and displayed on a rack,
which is most of why several of them stopped reading as the thing they were
meant to be.

Twenty by twelve fixes it: ten cells by six is 100 by 102 pixels, square to
within a pixel. That is a different canvas than the old art was drawn for,
so the art is redrawn rather than stretched - and redrawn from shapes in a
square space rather than by hand, so the next time the cell size changes this
is one number rather than thirteen re-drawings.

Run it and paste, or use --write to replace the table in place:

    python bench/tools/mkicons.py --write
"""

import argparse
import math
import os
import re

# The art grid. Chosen so COLS*2 * cell_w == ROWS*2 * cell_h at 10x17.
AW, AH = 20, 12

# Everything below draws into a unit square, x and y in 0..1, and is sampled
# into the grid at the end. Drawing in the square is the whole point: the
# aspect correction happens once, here, instead of in every shape.


class Canvas:
    def __init__(self, w=AW, h=AH, ss=4):
        self.w, self.h, self.ss = w, h, ss
        # Supersampled coverage, so a diagonal is decided by area rather than
        # by whether its centre line happened to cross a sample point.
        self.acc = [[0] * (w * ss) for _ in range(h * ss)]

    def _px(self, sx, sy):
        if 0 <= sy < self.h * self.ss and 0 <= sx < self.w * self.ss:
            self.acc[sy][sx] = 1

    def _u2s(self, x, y):
        return x * self.w * self.ss, y * self.h * self.ss

    def disc(self, cx, cy, r, thick=None):
        """A filled disc, or a ring when `thick` is given (both in unit x)."""
        sw, sh = self.w * self.ss, self.h * self.ss
        for sy in range(sh):
            for sx in range(sw):
                # Distance measured in unit-x so a circle is round on screen.
                dx = (sx + 0.5) / sw - cx
                dy = ((sy + 0.5) / sh - cy) * (self.h / self.w) * (self.w / self.h)
                d = math.hypot(dx, dy)
                if thick is None:
                    if d <= r:
                        self._px(sx, sy)
                elif r - thick <= d <= r:
                    self._px(sx, sy)

    def rect(self, x0, y0, x1, y1):
        sx0, sy0 = self._u2s(x0, y0)
        sx1, sy1 = self._u2s(x1, y1)
        for sy in range(int(sy0), int(math.ceil(sy1))):
            for sx in range(int(sx0), int(math.ceil(sx1))):
                self._px(sx, sy)

    def line(self, x0, y0, x1, y1, thick=0.05):
        """A thick line. `thick` is a radius in unit x."""
        sw, sh = self.w * self.ss, self.h * self.ss
        steps = int(max(abs(x1 - x0) * sw, abs(y1 - y0) * sh) * 2) + 2
        for i in range(steps + 1):
            t = i / steps
            self.disc(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, thick)

    def poly(self, pts):
        """A filled polygon, by even-odd scanline."""
        sw, sh = self.w * self.ss, self.h * self.ss
        P = [(x * sw, y * sh) for x, y in pts]
        for sy in range(sh):
            yc = sy + 0.5
            xs = []
            for i in range(len(P)):
                x0, y0 = P[i]
                x1, y1 = P[(i + 1) % len(P)]
                if (y0 <= yc < y1) or (y1 <= yc < y0):
                    xs.append(x0 + (yc - y0) * (x1 - x0) / (y1 - y0))
            xs.sort()
            for i in range(0, len(xs) - 1, 2):
                for sx in range(int(xs[i]), int(math.ceil(xs[i + 1]))):
                    self._px(sx, sy)

    def arc(self, cx, cy, r, a0, a1, thick=0.045):
        steps = 48
        for i in range(steps + 1):
            a = math.radians(a0 + (a1 - a0) * i / steps)
            self.disc(cx + math.cos(a) * r, cy + math.sin(a) * r, thick)

    def rows(self):
        """Coverage down to the art grid: a cell is ink when most of it is."""
        out = []
        for y in range(self.h):
            row = []
            for x in range(self.w):
                hit = 0
                for sy in range(self.ss):
                    for sx in range(self.ss):
                        hit += self.acc[y * self.ss + sy][x * self.ss + sx]
                row.append('#' if hit * 2 >= self.ss * self.ss else '.')
            out.append("".join(row))
        return out


# ------------------------------------------------------------------ icons --

def i_tower():
    """A mast with a beam either side. Infrastructure, which is what a
    trunked system is - and distinct from the FM icon, which is a signal."""
    c = Canvas()
    c.poly([(0.46, 0.30), (0.54, 0.30), (0.60, 1.0), (0.40, 1.0)])   # mast
    c.rect(0.28, 0.94, 0.72, 1.0)                                     # base
    # Two arcs each side, opening outward from the feed - the broadcast
    # symbol, which is what says "this is a transmitter" and not "this is a
    # pylon". Angles measured the usual way: 0 is to the right.
    for r in (0.20, 0.34):
        c.arc(0.5, 0.26, r, -55, 55, 0.032)
        c.arc(0.5, 0.26, r, 125, 235, 0.032)
    c.disc(0.5, 0.26, 0.05)                                           # feed
    return c


def i_wave():
    """A sine. Two full cycles so it reads as a wave and not as a hill."""
    c = Canvas()
    n = 96
    for i in range(n + 1):
        t = i / n
        x = 0.04 + t * 0.92
        y = 0.5 - math.sin(t * math.pi * 4) * 0.30
        c.disc(x, y, 0.045)
    return c


def i_plane():
    """An airliner from above, nose up. Swept wings so it is not a cross."""
    c = Canvas()
    c.poly([(0.50, 0.02), (0.56, 0.16), (0.56, 0.60), (0.50, 0.72),
            (0.44, 0.60), (0.44, 0.16)])                     # fuselage
    c.poly([(0.06, 0.56), (0.44, 0.34), (0.56, 0.34), (0.94, 0.56),
            (0.94, 0.66), (0.56, 0.52), (0.44, 0.52), (0.06, 0.66)])  # wings
    c.poly([(0.26, 0.94), (0.44, 0.78), (0.56, 0.78), (0.74, 0.94),
            (0.74, 1.0), (0.26, 1.0)])                        # tailplane
    return c


def i_record():
    """A filled disc. The one symbol nobody has to be taught."""
    c = Canvas()
    c.disc(0.5, 0.5, 0.40)
    return c


def i_falls():
    """Bands of broken density falling: what the instrument looks like when
    it is working, which is the only honest thing to put on its tile."""
    c = Canvas()
    seed = 12345
    # Carriers first: solid columns straight down, which is what a signal
    # looks like in a waterfall and what makes this read as one rather than
    # as noise. The scatter goes around them.
    carriers = (3, 4, 10, 15, 16)
    for y in range(AH):
        for x in range(AW):
            seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
            near = min(abs(x - k) for k in carriers)
            if near == 0:
                ink = True
            elif near == 1:
                ink = (seed >> 16) % 100 < 55
            else:
                ink = (seed >> 16) % 100 < 22
            if ink:
                c.rect(x / AW, y / AH, (x + 1) / AW, (y + 1) / AH)
    return c


def i_map():
    """A map pin, and nothing else.

    Two richer versions came first - a folded sheet with a route punched out
    of it, then a sheet with a pin standing on it - and both turned to mush.
    Ten cells across with a two-pixel minimum stroke gives about five
    distinguishable features; a sheet, its folds, and a pin on top is more
    than that, so it read as neither. A pin alone is a silhouette, it is the
    map symbol everywhere, and it survives the resolution."""
    c = Canvas()
    c.disc(0.50, 0.36, 0.30)
    c.poly([(0.28, 0.50), (0.72, 0.50), (0.50, 0.98)])
    hole = Canvas()
    hole.disc(0.50, 0.35, 0.115)
    c.acc = [[a & ~b for a, b in zip(ra, rb)]
             for ra, rb in zip(c.acc, hole.acc)]
    return c


def i_files():
    """A folder with a tab. Not a document: this is a directory."""
    c = Canvas()
    c.rect(0.06, 0.16, 0.42, 0.28)
    c.rect(0.06, 0.26, 0.94, 0.92)
    hole = Canvas()
    hole.rect(0.14, 0.40, 0.86, 0.84)
    c.acc = [[a & ~b for a, b in zip(ra, rb)]
             for ra, rb in zip(c.acc, hole.acc)]
    return c


def i_gear():
    """A cog with eight teeth and a hole. Settings, universally."""
    c = Canvas()
    for k in range(8):
        a = math.radians(k * 45)
        c.disc(0.5 + math.cos(a) * 0.36, 0.5 + math.sin(a) * 0.36, 0.11)
    c.disc(0.5, 0.5, 0.30)
    hole = Canvas()
    hole.disc(0.5, 0.5, 0.13)
    c.acc = [[a & ~b for a, b in zip(ra, rb)]
             for ra, rb in zip(c.acc, hole.acc)]
    return c


def i_pager():
    """A pager: a body, a screen, and a stub aerial."""
    c = Canvas()
    c.rect(0.10, 0.20, 0.90, 0.92)
    c.rect(0.74, 0.06, 0.80, 0.22)
    screen = Canvas()
    screen.rect(0.20, 0.32, 0.80, 0.62)
    c.acc = [[a & ~b for a, b in zip(ra, rb)]
             for ra, rb in zip(c.acc, screen.acc)]
    return c


def i_chip():
    """An integrated circuit: a body with legs down each side.

    The gap between body and leg has to be two art columns or the coverage
    rounding closes it and the whole thing reads as one bar. That is the
    constraint this canvas imposes and it is why the body is narrow."""
    c = Canvas()
    c.rect(0.36, 0.12, 0.64, 0.88)
    for k in range(3):
        y = 0.20 + k * 0.26
        c.rect(0.06, y, 0.24, y + 0.14)
        c.rect(0.76, y, 0.94, y + 0.14)
    return c


def i_star():
    """A five-pointed star: an app that came off the card."""
    c = Canvas()
    pts = []
    for k in range(10):
        a = math.radians(-90 + k * 36)
        r = 0.46 if k % 2 == 0 else 0.19
        pts.append((0.5 + math.cos(a) * r, 0.5 + math.sin(a) * r))
    c.poly(pts)
    return c


def i_shark():
    """A fin and the water it is cutting."""
    c = Canvas()
    c.poly([(0.62, 0.06), (0.74, 0.66), (0.20, 0.66)])       # fin
    c.poly([(0.62, 0.06), (0.74, 0.66), (0.66, 0.66)])       # trailing edge
    for k in range(3):
        y = 0.76 + k * 0.10
        n = 28
        for i in range(n + 1):
            t = i / n
            c.disc(0.04 + t * 0.92,
                   y + math.sin(t * math.pi * 3 + k) * 0.035, 0.035)
    return c


def i_sat():
    """A satellite over a horizon: where you are, from above.

    GPS was borrowing the map pin, which is the MAP tile's icon - two tiles
    with the same picture is worse than an imperfect picture, because it says
    they do the same thing. A satellite and a ground arc says position
    fixing, and nothing else on the directory looks like it."""
    c = Canvas()
    c.rect(0.42, 0.06, 0.58, 0.34)                 # body
    for s in (-1, 1):
        c.poly([(0.5 + s * 0.16, 0.10), (0.5 + s * 0.44, 0.04),
                (0.5 + s * 0.44, 0.24), (0.5 + s * 0.16, 0.30)])  # panels
    c.line(0.5, 0.34, 0.5, 0.46, 0.022)            # the downlink
    c.arc(0.5, 1.30, 0.72, 235, 305, 0.045)        # the horizon
    return c


def i_power():
    """The power symbol: a broken ring with a bar through the gap.

    RADIOS was borrowing the P25 tower, which says "a trunked system" and
    not "everything with an antenna, and how to switch it off". This is what
    a switch looks like anywhere."""
    c = Canvas()
    c.arc(0.5, 0.56, 0.34, -62, 242, 0.055)
    c.rect(0.455, 0.08, 0.545, 0.52)
    return c


def i_mesh():
    """Four nodes and the links between them, which is what a mesh is.

    The one icon reported as reading correctly, so the design is kept and
    only the canvas changes - the same corners, the same diagonals, the same
    hub, drawn square instead of stretched."""
    c = Canvas()
    # In off the edge. At the corners the nodes ran into the tile border and
    # what survived read as an X with blobs on it rather than as four nodes
    # joined to a hub - the links became the whole figure.
    corners = [(0.16, 0.16), (0.84, 0.16), (0.16, 0.84), (0.84, 0.84)]
    # Small nodes and long links: the LINK is the thing worth drawing, and
    # fat nodes swallowed it - four blobs and a bar, which is not a topology.
    for x, y in corners:
        c.line(x, y, 0.5, 0.5, 0.026)
    for x, y in corners:
        c.disc(x, y, 0.105)
    c.disc(0.5, 0.5, 0.125)
    return c


ICONS = [
    ("LS_ICON_TOWER",  i_tower,
     "A mast with a beam either side. Infrastructure, which is what a\n"
     "   trunked system is, and distinct from the FM tile's signal."),
    ("LS_ICON_WAVE",   i_wave,
     "Two full cycles of a sine: a wave rather than a hill."),
    ("LS_ICON_PLANE",  i_plane,
     "An airliner from above. Swept wings, so it is not a cross."),
    ("LS_ICON_RECORD", i_record,
     "A filled disc - the one symbol nobody has to be taught."),
    ("LS_ICON_FALLS",  i_falls,
     "Bands of broken density falling, which is what the instrument looks\n"
     "   like when it is working."),
    ("LS_ICON_MAP",    i_map,
     "A folded sheet with a route punched through it."),
    ("LS_ICON_FILES",  i_files,
     "A folder with a tab. Not a document: this is a directory."),
    ("LS_ICON_GEAR",   i_gear,
     "A cog with eight teeth."),
    ("LS_ICON_PAGER",  i_pager,
     "A body, a screen and a stub aerial."),
    ("LS_ICON_CHIP",   i_chip,
     "An integrated circuit with legs, and the pin-one dot."),
    ("LS_ICON_STAR",   i_star,
     "A five-pointed star: an app that came off the card."),
    ("LS_ICON_SHARK",  i_shark,
     "A fin, and the water it is cutting."),
    ("LS_ICON_SAT",    i_sat,
     "A satellite over a horizon: position fixing, and nothing else in the\n"
     "   directory looks like it."),
    ("LS_ICON_POWER",  i_power,
     "The power symbol - a broken ring with a bar through the gap."),
    ("LS_ICON_MESH",   i_mesh,
     "Four nodes and the links between them. A mesh is a topology, and\n"
     "   what is worth drawing is that the corners are joined."),
]


def emit():
    out = []
    out.append("[LS_ICON_NONE] = {")
    blank = '"' + "." * AW + '"'
    for i in range(0, AH, 2):
        out.append("    " + ", ".join([blank] * 2) + ",")
    out[-1] = out[-1][:-1] + " },"
    out.append("")
    for name, fn, why in ICONS:
        out.append("/* %s */" % why)
        out.append("[%s] = {" % name)
        rows = fn().rows()
        for i in range(0, AH, 2):
            pair = ", ".join('"%s"' % r for r in rows[i:i + 2])
            out.append("    " + pair + ",")
        out[-1] = out[-1][:-1] + " },"
        out.append("")
    return "\n".join(out).rstrip() + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--preview", action="store_true")
    args = ap.parse_args()

    if args.preview:
        for name, fn, _ in ICONS:
            print(name)
            for r in fn().rows():
                print("  " + r.replace(".", " ").replace("#", "#"))
            print()
        return

    table = emit()
    if not args.write:
        print(table)
        return

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    path = os.path.join(root, "..", "components", "apps", "tui", "ls_icons.c")
    path = os.path.normpath(path)
    src = open(path, encoding="utf-8", newline="").read()
    nl = "\r\n" if "\r\n" in src else "\n"

    start = src.index("static const char *const ART")
    open_brace = src.index("{", start)
    # The table ends at the line "};" that closes it.
    end = src.index(nl + "};" + nl, open_brace) + len(nl) + 2

    head = ("static const char *const ART[LS_ICON__COUNT][%d] = {" % AH)
    body = table.replace("\n", nl)
    src = src[:start] + head + nl + body + nl + "};" + src[end:]
    open(path, "w", encoding="utf-8", newline="").write(src)
    print("rewrote", path)


if __name__ == "__main__":
    main()
