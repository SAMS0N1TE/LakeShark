"""The web screen's home-screen icons: a fin over the water on black, in the
board's cyan. Needs Pillow; the PNGs are kept beside it, so the build does not.

    python icons.py
"""
import os

from PIL import Image, ImageDraw

CYAN = (70, 214, 224)
DEEP = (16, 74, 92)


def icon(size):
    k = 4                                          # drawn large, then scaled down for smooth edges
    n = size * k
    im = Image.new('RGB', (n, n), (0, 0, 0))
    d = ImageDraw.Draw(im)
    u = n / 100
    water = 64 * u
    # The fin: a curved back edge, a straight front one.
    fin = [(30 * u, water)]
    for i in range(21):
        t = i / 20
        fin.append((30 * u + t * t * 26 * u + t * 6 * u, water - t * 44 * u + t * t * 4 * u))
    fin += [(68 * u, water - 6 * u), (74 * u, water)]
    d.polygon(fin, fill=CYAN)
    # The water: three bands, the glass's own blocky look.
    for i, y in enumerate((water, water + 9 * u, water + 18 * u)):
        d.rectangle([14 * u + i * 6 * u, y, 86 * u - i * 6 * u, y + 4.5 * u], fill=CYAN if i == 0 else DEEP)
    return im.resize((size, size), Image.LANCZOS)


if __name__ == '__main__':
    here = os.path.dirname(os.path.abspath(__file__))
    for s in (180, 192, 512):
        icon(s).save(os.path.join(here, f'icon-{s}.png'), optimize=True)
