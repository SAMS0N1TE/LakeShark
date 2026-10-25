"""The firmware's app icons: 20 x 18 colour dots each, lit and shaded the way
COMPASS and DICE draw. 'A' is the tile's colour bright and 'a' the same
colour dim, so a theme or a tile colour still recolours the accent;
everything else is a theme colour or clear. A cell shows its two commonest
colours, so keep each 2 x 3 block to two where it matters.

    python app_icons.py preview.png          # how they pack on the board
    python app_icons.py --emit ../ls_icons_px.inc

The .inc is generated: edit the drawings here, not the table there."""
import math, sys
from PIL import Image, ImageDraw

W, H = 20, 18
PAL = [0x0000, 0xB249, 0x4C6B, 0xB447, 0x3B74, 0x8AD4, 0x3C73, 0xAD97,
       0x3A09, 0xFB4D, 0x6F0F, 0xFE4A, 0x6DBF, 0xDC5F, 0x5F1C, 0xF7BF]
BAYER = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]
A, a = 'A', 'a'                      # the tile's colour, bright and dim
# Dots: '0'-'9' and 'b'-'f' are theme colours, 'g' is colour 10 (bright
# green: 'a' is taken by the accent), 'A'/'a' the accent, '.' clear.


def bayer(x, y): return (BAYER[y & 3][x & 3] + .5) / 16


def ramp(b, x, y, inks):
    if b <= 0: return inks[0]
    if b >= 1: return inks[-1]
    f = b * (len(inks) - 1); lo = int(f)
    return inks[lo + (1 if f - lo > bayer(x, y) else 0)]


class Ic:
    def __init__(self): self.d = [['.'] * W for _ in range(H)]
    def set(self, x, y, c):
        x, y = int(round(x)), int(round(y))
        if 0 <= x < W and 0 <= y < H and c is not None: self.d[y][x] = c
    def get(self, x, y): return self.d[y][x] if 0 <= x < W and 0 <= y < H else '.'
    def rect(self, x0, y0, w, h, c):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w): self.set(x, y, c)
    def frame(self, x0, y0, w, h, c):
        for x in range(x0, x0 + w): self.set(x, y0, c); self.set(x, y0 + h - 1, c)
        for y in range(y0, y0 + h): self.set(x0, y, c); self.set(x0 + w - 1, y, c)
    def line(self, x0, y0, x1, y1, c):
        n = int(math.ceil(max(abs(x1 - x0), abs(y1 - y0), 1)))
        for i in range(n + 1): self.set(x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n, c)
    def disc(self, cx, cy, r, c, ry=None):
        ry = ry or r
        for y in range(H):
            for x in range(W):
                if ((x + .5 - cx) / r) ** 2 + ((y + .5 - cy) / ry) ** 2 <= 1: self.set(x, y, c)
    def ring(self, cx, cy, r, c, t=1.0, ry=None, a0=0, a1=360):
        ry = ry or r
        for y in range(H):
            for x in range(W):
                dx, dy = (x + .5 - cx) / r, (y + .5 - cy) / ry
                d = math.hypot(dx, dy)
                ang = math.degrees(math.atan2(-dy, dx)) % 360
                if abs(d - 1) * min(r, ry) <= t / 2 and (a0 <= ang <= a1 if a0 <= a1 else (ang >= a0 or ang <= a1)):
                    self.set(x, y, c)
    def ball(self, cx, cy, r, inks, ry=None, hi=None):
        ry = ry or r
        for y in range(H):
            for x in range(W):
                nx, ny = (x + .5 - cx) / r, (y + .5 - cy) / ry
                d2 = nx * nx + ny * ny
                if d2 > 1: continue
                nz = math.sqrt(1 - d2)
                l = max(0, -.45 * nx - .35 * ny + .82 * nz)
                if hi and l > .97: self.set(x, y, hi); continue
                self.set(x, y, ramp(l * 1.1 - .05, x, y, inks))
    def poly(self, pts, c):
        for y in range(H):
            for x in range(W):
                px, py = x + .5, y + .5
                inside = False
                for i in range(len(pts)):
                    (x1, y1), (x2, y2) = pts[i], pts[(i + 1) % len(pts)]
                    if (y1 > py) != (y2 > py) and px < x1 + (py - y1) * (x2 - x1) / (y2 - y1): inside = not inside
                if inside: self.set(x, y, c)
    def rows(self): return [''.join(r) for r in self.d]


METAL = ['8', '7', 'f']            # dark grey, grey, white


def art(rows, keys=None):
    """An icon straight from 18 rows of 20 characters."""
    i = Ic()
    assert len(rows) == 18, len(rows)
    for y, r in enumerate(rows):
        assert len(r) == 20, (y, r, len(r))
        for x, c in enumerate(r):
            if c != '.': i.set(x, y, (keys or {}).get(c, c))
    return i


# ---- the icons ---------------------------------------------------------------

def shark():                       # HOME: a fin through the swell
    i = Ic()
    i.poly([(5, 13), (11, 1), (14, 5), (13.5, 13)], '7')
    i.poly([(11, 1), (12.6, 3), (12, 13), (9.5, 13)], 'f')
    for x in range(W):
        y = 12 + round(math.sin((x + 1) / 2.0) * .8)
        for yy in range(y, 18): i.set(x, yy, A if yy - y < 2 else a)
    for x in range(0, 20, 5): i.set(x + 2, 15, 'f')
    return i


def tower():                       # P25: the mast and its signal
    return art([
        "..A...............A.",
        ".A..A....ff....A..A.",
        ".A.A.....99.....A.A.",
        ".A..A....77....A..A.",
        "..A......77.......A.",
        "........7777........",
        "........7..7........",
        ".......77777........",
        ".......7....7.......",
        ".......7....7.......",
        "......77777777......",
        "......7......7......",
        ".....77......77.....",
        ".....777777777777...",
        "....77........77....",
        "....7..........7....",
        "...77..........77...",
        "...7............7...",
    ])


def wave():                        # FM: a clean carrier, two cycles
    i = Ic()
    for x10 in range(0, 200):
        x = x10 / 10
        y = 8.5 - 6 * math.sin((x - .5) / 19 * 4 * math.pi)
        i.set(x, y, A); i.set(x, y + 1, A)
    for x10 in range(0, 200):
        x = x10 / 10
        y = 8.5 - 6 * math.sin((x - .5) / 19 * 4 * math.pi)
        i.set(x, y + 2, a) if i.get(int(round(x)), int(round(y + 2))) == '.' else None
    return i


def plane():                       # ADSB: a jet from above, a ping behind
    i = Ic()
    i.ring(10, 9, 8.5, a, 1, ry=8.5)
    i.poly([(10, 1), (11.2, 4), (11.2, 8), (18, 11), (18, 12.5), (11.2, 11), (11, 15), (13.5, 17), (6.5, 17), (9, 15), (8.8, 11), (2, 12.5), (2, 11), (8.8, 8), (8.8, 4)], '7')
    i.line(10, 2, 10, 15, 'f'); i.line(11, 8, 17, 11, 'f')
    i.rect(9, 3, 2, 2, A)
    return i


def record():                      # REC: the record lamp, lit
    i = Ic()
    i.ring(10, 9, 8, '7', 1.4)
    i.ball(10, 9, 6, ['1', A], hi='f')
    return i


def falls():                       # FALLS: a spectrum over its waterfall
    # The frame is lit along its top and left, so the trace can touch it
    # without a third colour in the cell.
    return art([
        "ffffffffffffffffffff",
        "f0000000ff000000000f",
        "f000000f00f00000000f",
        "f000000f00f0000ff007",
        "f00000f0000f00f00f07",
        "ffffff000000ff000077",
        "f4444aaAAAAaa444aa47",
        "f44444aAffAa444aAa47",
        "f4444aaAAAAaa444aa47",
        "f44444aAAAAa44444447",
        "f4444aaAAAAaa4444447",
        "f44444aAAAAa444aA447",
        "f4444aaAAAAaa444a447",
        "f44444aAAAAa44444447",
        "f4444aaAAAAaa4444447",
        "77777777777777777777",
        "...777........777...",
        "....................",
    ])


def map_():                        # MAP: a folded sheet, a route and a pin
    i = Ic()
    i.poly([(1, 4), (7, 2), (13, 4), (19, 2), (19, 16), (13, 18), (7, 16), (1, 18)], '7')
    i.poly([(7, 2), (13, 4), (13, 18), (7, 16)], 'f')
    i.line(3, 15, 6, 11, A); i.line(6, 11, 10, 12, A); i.line(10, 12, 13, 8, A)
    i.disc(14.5, 6.5, 2.4, '9'); i.set(14.5, 6.5, 'f'); i.line(14.5, 8, 14.5, 11, '1')
    return i


def files():                       # FILES: a folder with a card in it
    i = Ic()
    i.rect(1, 3, 7, 2, a)
    i.rect(1, 5, 18, 12, a)
    i.rect(4, 2, 11, 7, 'f'); i.rect(5, 4, 9, 1, '8'); i.rect(5, 6, 6, 1, '8')
    i.rect(1, 8, 18, 9, A)
    i.line(1, 8, 18, 8, 'f')
    return i


def gear():                        # SET: a machined gear
    i = Ic()
    for k in range(8):
        t = k / 8 * 2 * math.pi
        cx, cy = 10 + math.cos(t) * 7, 9 + math.sin(t) * 7
        i.disc(cx, cy, 1.9, '7')
    i.ball(10, 9, 6.3, METAL)
    i.disc(10, 9, 2.6, '0')
    i.ring(10, 9, 2.6, A, 1)
    return i


def pager():                       # MIX-RF: a pager with words on its screen
    i = Ic()
    i.rect(2, 4, 16, 12, '8')
    i.rect(3, 5, 14, 10, '7')
    i.rect(4, 6, 12, 5, A)
    i.rect(5, 7, 7, 1, a); i.rect(5, 9, 9, 1, a)
    i.rect(4, 12, 3, 2, 'f'); i.rect(8, 12, 3, 2, 'f'); i.rect(12, 12, 4, 2, '9')
    i.line(15, 1, 15, 4, '7'); i.set(15, 1, A)
    return i


def chip():                        # DIAG: a chip and its heartbeat
    i = Ic()
    for x in range(4, 17, 3):
        i.rect(x, 1, 1, 3, '7'); i.rect(x, 14, 1, 3, '7')
    i.rect(2, 4, 16, 10, '8'); i.frame(2, 4, 16, 10, '7')
    pts = [(3, 9), (6, 9), (7, 6), (9, 12), (11, 7), (12, 9), (16, 9)]
    for p, q in zip(pts, pts[1:]): i.line(*p, *q, A)
    i.set(3, 5, 'f')
    return i


def star():                        # user apps: a star, lit from the top left
    i = Ic()
    pts = []
    for k in range(10):
        t = -math.pi / 2 + k * math.pi / 5
        r = 9 if k % 2 == 0 else 3.8
        pts.append((10 + math.cos(t) * r, 10 + math.sin(t) * r))
    i.poly(pts, A)
    c = (10, 10)
    for k in (0, 2, 4, 6, 8):          # the shadowed side of each point
        i.poly([c, pts[k], pts[(k + 1) % 10]], a)
    return i


def mesh():                        # MESH: radios linked in a mesh
    i = Ic()
    nodes = [(4, 3), (15, 2), (9.5, 9), (3, 15), (16, 14)]
    for p, q in [(0, 1), (0, 2), (1, 2), (1, 4), (2, 3), (2, 4), (3, 4), (0, 3)]:
        i.line(*nodes[p], *nodes[q], '7')
    for k, (x, y) in enumerate(nodes):
        r = 2.6 if k == 2 else 2.2
        i.disc(x, y, r, A)
        i.set(x - .6, y - .8, 'f')
    return i


def sat():                         # GPS: a position fix, the crosshair
    i = Ic()
    i.ring(10, 9, 6.2, A, 2.0)
    for (x0, y0, x1, y1) in [(10, 0, 10, 3), (10, 15, 10, 17), (1, 9, 3, 9), (17, 9, 19, 9)]:
        i.line(x0, y0, x1, y1, A)
        i.line(x0 - (1 if x0 == x1 else 0), y0 - (0 if x0 == x1 else 1),
               x1 - (1 if x0 == x1 else 0), y1 - (0 if x0 == x1 else 1), A)
    i.disc(9.5, 8.5, 2.3, 'f')
    return i


def power():                       # RADIOS: the power symbol
    return art([
        "....................",
        ".........AA.........",
        ".........AA.........",
        "....AA...AA...AA....",
        "...AA....AA....AA...",
        "..AA.....AA.....AA..",
        "..A......AA......A..",
        ".AA......AA......AA.",
        ".AA......AA......AA.",
        ".AA..............AA.",
        ".AA..............AA.",
        "..A..............A..",
        "..AA............AA..",
        "...AA..........AA...",
        "....AAA......AAA....",
        "......AAAAAAAA......",
        "....................",
        "....................",
    ])


BT = ["..##..",
      "..#.#.",
      "#.#..#",
      ".###.#",
      "..##..",
      ".###.#",
      "#.#..#",
      "..#.#.",
      "..##.."]


def wireless():                    # LINK: the Wi-Fi fan and the Bluetooth rune
    return art([
        "....................",
        "....................",
        "....................",
        "....AAAAAAA.........",
        "..AAAA...AAAA....c..",
        ".AAA.......AAA...cc.",
        ".AA.........AA.c.c.c",
        ".....AAAAA......ccc.",
        "...AAAA.AAAA.....c..",
        "...AA.....AA....ccc.",
        "...............c.c.c",
        "......AAA........cc.",
        ".....AA.AA.......c..",
        "....................",
        "......fff...........",
        "......fff...........",
        "....................",
        "....................",
    ])


def labs():                        # LORA LABS: a glass flask, bubbling
    # A lit glass wall on the left, shadowed on the right; the liquid in the
    # tile's colour, darker round its far side; bubbles rising; a lip on the
    # neck and the base's shadow on the bench. Two colours to every cell.
    return art([
        "....................",
        ".......fffff7.......",
        ".......f....7.......",
        ".......f.f..7.......",
        ".......f.f..7.......",
        ".......f.f..7.......",
        ".......f....7.......",
        "......f....f.7......",
        "......f......7......",
        ".....f....A...7.....",
        "....ffAAAAAAAA77....",
        "....ffAAAAAAAA77....",
        "...fAAAAAAAAfAaa7...",
        "..ffAAAAAAAfAAaa77..",
        "..ffAAAAfAAAAAaa77..",
        ".fffffffffff7777777.",
        ".fffffffffff7777777.",
        "....88888888888888..",
    ])


def journal():                     # NOTES: a notebook and a pencil
    i = Ic()
    i.rect(2, 2, 13, 15, 'f'); i.rect(2, 2, 2, 15, A)
    for y in range(4, 16, 3): i.set(3, y, '0')
    for y in (6, 9, 12): i.line(6, y, 13, y, '7')
    i.line(18, 3, 11, 15, '3'); i.line(19, 4, 12, 16, 'b'); i.line(18.5, 3.5, 11.5, 15.5, 'b')
    i.set(11, 16, '0'); i.set(18, 2, '9'); i.set(19, 3, '9')
    return i


def compass():                     # COMPASS: a steel bezel, a red north
    i = Ic()
    i.disc(10, 9, 8.6, '7')
    i.ball(10, 9, 8.6, ['8', '7', 'f'])
    i.disc(10, 9, 6.2, '0')
    for k in range(4):
        t = k * math.pi / 2
        i.set(10 + math.cos(t) * 5.2, 9 + math.sin(t) * 5.2, '7')
    i.poly([(10, 2.6), (12, 9), (8, 9)], '9')
    i.poly([(10, 15.4), (12, 9), (8, 9)], 'f')
    i.disc(10, 9, 1, A)
    return i


def music():                       # MUSIC: a cassette
    i = Ic()
    i.rect(1, 3, 18, 13, '8'); i.frame(1, 3, 18, 13, '7')
    i.rect(3, 5, 14, 6, A); i.rect(3, 9, 14, 2, a)
    i.disc(6.5, 8, 2, 'f'); i.disc(13.5, 8, 2, 'f'); i.set(6.5, 8, '0'); i.set(13.5, 8, '0')
    i.rect(8, 7, 4, 2, '0')
    i.poly([(5, 15), (6.5, 12), (13.5, 12), (15, 15)], '7')
    return i


def experiment():                  # EXPERIMENTS: a wave under the glass
    i = Ic()
    i.line(13, 12, 18, 17, '7'); i.line(14, 12, 19, 17, '8'); i.line(13, 13, 18, 18, '8')
    i.disc(8, 7.5, 6.6, '7')
    i.disc(8, 7.5, 5.2, '4')
    for x10 in range(30, 140):
        x = x10 / 10
        y = 7.5 - 2.6 * math.sin((x - 3) / 10 * 2 * math.pi)
        if (x - 8) ** 2 + (y - 7.5) ** 2 < 22: i.set(x, y, A)
    i.set(5, 4, 'f'); i.set(4, 5, 'f')
    return i


def terminal():                    # TERMINAL: a screen with a prompt
    i = Ic()
    i.rect(1, 1, 18, 13, '7'); i.rect(2, 2, 16, 11, '0')
    i.line(4, 4, 6, 6, A); i.line(6, 6, 4, 8, A)
    i.rect(8, 8, 5, 1, A); i.rect(14, 4, 2, 5, a)
    i.rect(8, 14, 4, 2, '8'); i.rect(5, 16, 10, 2, '7')
    return i


def update():                      # UPDATE: an arrow into a tray
    i = Ic()
    i.rect(8, 1, 4, 7, A)
    i.poly([(3, 7), (17, 7), (10, 13.5)], A)
    i.poly([(8, 1), (10, 1), (10, 12), (5, 7.5), (8, 7.5)], 'f')
    i.rect(1, 12, 2, 5, '7'); i.rect(17, 12, 2, 5, '7'); i.rect(1, 15, 18, 2, '7')
    i.line(1, 15, 18, 15, 'f')
    return i


def tiles():                       # TILES: a map cut into tiles, one of them chosen
    i = Ic()
    for y in range(18):
        for x in range(20):
            coast = 13 - .9 * y + 2.2 * math.sin(y / 2.3) + 1.2 * math.sin(x / 3.1)
            land = x < coast + 6 or (x - 16) ** 2 + (y - 3) ** 2 < 5
            i.set(x, y, ('g' if (x + y) % 7 == 0 else '2') if land else '4')
    for x in (6, 13): i.line(x, 0, x, 17, '0')
    for y in (5, 11): i.line(0, y, 19, y, '0')
    i.frame(6, 5, 8, 7, 'f')
    return i


def drone():                       # DRONES: a quadcopter from above
    i = Ic()
    for cx, cy in [(4, 4), (16, 4), (4, 14), (16, 14)]:
        i.disc(cx, cy, 3.6, a, ry=3); i.ring(cx, cy, 3.6, A, 1, ry=3)
        i.set(cx, cy, 'f')
    i.line(4, 4, 16, 14, '7'); i.line(16, 4, 4, 14, '7')
    i.ball(10, 9, 3.2, METAL, ry=2.8)
    i.set(10, 7, A)
    return i


def sweep():                       # SWEEP: a radar scope, its beam, a contact
    i = Ic()
    i.disc(10, 9, 8.6, '0')
    for y in range(18):
        for x in range(20):
            dx, dy = x + .5 - 10, y + .5 - 9
            r = math.hypot(dx, dy)
            ang = math.degrees(math.atan2(dy, dx))
            if r < 8 and -95 < ang < -40: i.set(x, y, a)
    i.ring(10, 9, 8.6, '7', 1.3)
    i.ring(10, 9, 4.6, '8', 1)
    i.line(10, 9, 15.6, 3, A); i.line(10, 8, 15, 2.6, A)
    i.disc(5, 13, 1.5, A)
    i.set(10, 9, 'f')
    return i


def detector():                    # DETECTOR: a watching eye
    # Lit from the upper left: a crease and a lit lid that shades the ball
    # under it, an iris dark above and bright below (light through the
    # cornea), a glint, and the white rolling off to grey at the right.
    # Every cell holds two colours, so the board shows exactly this.
    return art([
        "....................",
        "....................",
        "....................",
        "......88888888......",
        "......8ffffff8......",
        ".....ff888888ff.....",
        "..8ff877aaaa778ff8..",
        "..f88f7affaaa7f88f..",
        ".78fffaaffaaaafff87.",
        "f8ffffaa0000aa777787",
        "8ffffaAA0000AAa77778",
        "8fffffaA0000Aa777788",
        ".8ffffaaAAAAaa77788.",
        "....fffaAAAAa77788..",
        "....88ffaaaa778888..",
        "......88888888......",
        "....................",
        "....................",
    ])


def cell():                        # CELL WATCH: a handset and its bars
    i = Ic()
    i.rect(2, 1, 9, 17, '8'); i.frame(2, 1, 9, 17, '7')
    i.rect(3, 3, 7, 10, '4'); i.rect(4, 4, 5, 3, a)
    i.rect(5, 15, 3, 1, '7')
    for k, h in enumerate((3, 6, 9, 12)):
        i.rect(12 + k * 2, 16 - h, 1, h, A if k < 3 else a)
    return i


def subghz():                      # SUB-GHZ: a key fob sending
    return art([
        "....................",
        "..........A.........",
        "......f....A...A....",
        "......7.....A...A...",
        "......7.....A....A..",
        "......7....A....A...",
        "...7777777A....A....",
        "..78888888c.........",
        "..78AAA88c..........",
        "..78AAA88c..........",
        "..788888c7..........",
        "..78aaa8c7..........",
        "..78aaa8c7..........",
        "..7888888c..........",
        "..7888888c..........",
        "...7777777..........",
        "....................",
        "....................",
    ], {'c': '7'})


ICONS = [  # (enum name, function, sample hue for the preview)
    ('SHARK', shark, 6), ('TOWER', tower, 2), ('WAVE', wave, 3), ('PLANE', plane, 5),
    ('RECORD', record, 1), ('FALLS', falls, 4), ('MAP', map_, 2), ('FILES', files, 3),
    ('GEAR', gear, 4), ('PAGER', pager, 6), ('CHIP', chip, 7), ('STAR', star, 3),
    ('MESH', mesh, 6), ('SAT', sat, 3), ('POWER', power, 1), ('WIRELESS', wireless, 6),
    ('LABS', labs, 6), ('JOURNAL', journal, 2), ('COMPASS', compass, 3), ('MUSIC', music, 5),
    ('EXPERIMENT', experiment, 5), ('TERMINAL', terminal, 2), ('UPDATE', update, 6), ('TILES', tiles, 6),
    ('DRONE', drone, 6), ('SWEEP', sweep, 2), ('DETECTOR', detector, 3), ('CELL', cell, 6), ('SUBGHZ', subghz, 2),
]


def colour(ch, hue):
    if ch == 'A': return hue | 8
    if ch == 'a': return hue
    if ch == 'g': return 10
    return int(ch, 16)


def cells_over(ic):
    bad = 0
    for r in range(6):
        for c in range(10):
            s = {ic.d[3 * r + k // 2][2 * c + (k & 1)] for k in range(6)}
            bad += len(s) > 2
    return bad


def preview(path, selected=False):
    def rgb(v): return (((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31)
    cw, ch, cols = 10, 17, 6
    tw, th = 10 * cw + 24, 6 * ch + 34
    rows = (len(ICONS) + cols - 1) // cols
    img = Image.new('RGB', (cols * tw, rows * th), (0, 0, 0))
    dr = ImageDraw.Draw(img)
    for n, (name, fn, hue) in enumerate(ICONS):
        ic = fn()
        ox, oy = (n % cols) * tw + 12, (n // cols) * th + 8
        dr.rectangle([ox - 6, oy - 4, ox + 10 * cw + 5, oy + 6 * ch + 4], outline=rgb(PAL[hue]))
        for r in range(6):
            for c in range(10):
                cell = [ic.d[3 * r + k // 2][2 * c + (k & 1)] for k in range(6)]
                cols6 = [0 if v == '.' else colour(v, hue) for v in cell]
                cnt = [cols6.count(v) for v in cols6]
                ink = max(range(6), key=lambda k: (cnt[k], cols6[k]))
                pap = [k for k in range(6) if cols6[k] != cols6[ink]]
                pap = max(pap, key=lambda k: cnt[k]) if pap else None
                fg, bg = cols6[ink], (cols6[pap] if pap is not None else 0)
                for k in range(6):
                    v = fg if cols6[k] == fg else bg
                    x0 = ox + c * cw + (k & 1) * cw // 2; y0 = oy + r * ch + (k // 2) * ch // 3
                    dr.rectangle([x0, y0, x0 + cw // 2 - 1, y0 + ch // 3], fill=rgb(PAL[v]))
        dr.text((ox, oy + 6 * ch + 8), name, fill=(200, 200, 200))
    img = img.resize((img.width * 3 // 2, img.height * 3 // 2), Image.NEAREST)
    img.save(path)



ACCENT_BRIGHT, ACCENT_DIM, CLEAR = 0x10, 0x11, 0xFF


def code(ch):
    if ch == '.': return CLEAR
    if ch == 'A': return ACCENT_BRIGHT
    if ch == 'a': return ACCENT_DIM
    if ch == 'g': return 10
    return int(ch, 16)


def emit(path):
    """Each icon as up to 16 colours and a nibble per dot: 196 bytes."""
    out = ['/* Generated by icons/app_icons.py --emit: edit the drawings there. */',
           '/* Per icon: a palette (0-15 theme colours, 0x10/0x11 the tile colour bright/dim,',
           '   0xFF clear) and 20 x 18 dots, two to a byte, the first in the low nibble. */',
           'static const ls_icon_packed_t PX[LS_ICON__COUNT] = {']
    for name, fn, hue in ICONS:
        rows = fn().rows()
        dots = [code(c) for r in rows for c in r]
        pal = []
        for d in dots:
            if d not in pal: pal.append(d)
        assert len(pal) <= 16, (name, len(pal))
        idx = [pal.index(d) for d in dots]
        packed = [idx[k] | idx[k + 1] << 4 for k in range(0, len(idx), 2)]
        pal += [CLEAR] * (16 - len(pal))
        out.append(f'    [LS_ICON_{name}] = {{')
        out.append('        {' + ', '.join(f'0x{p:02x}' for p in pal) + '},')
        out.append('        {')
        for k in range(0, len(packed), 20):
            out.append('            ' + ', '.join(f'0x{b:02x}' for b in packed[k:k + 20]) + ',')
        out.append('        } },')
    out.append('};')
    with open(path, 'w', newline='') as f:
        f.write(''.join(line + chr(10) for line in out))


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--emit':
        emit(sys.argv[2])
    else:
        preview(sys.argv[1] if len(sys.argv) > 1 else 'preview.png')
        for name, fn, hue in ICONS:
            bad = cells_over(fn())
            if bad > 12: print(name, 'cells with 3+ colours:', bad)
