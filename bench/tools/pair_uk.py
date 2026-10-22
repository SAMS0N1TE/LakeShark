"""English and Ukrainian renders of one view side by side, for a review.

  python bench/tools/pair_uk.py VIEW SHAPE   ->  bench/build-uk/pair-VIEW-SHAPE.png
"""
import sys
from pathlib import Path

from PIL import Image

OUT = Path(__file__).resolve().parents[2] / "bench/build-uk"
view, shape = sys.argv[1], sys.argv[2]
en = Image.open(OUT / f"{view}-en-{shape}.png")
uk = Image.open(OUT / f"{view}-uk-{shape}.png")
gap = 12
if en.width > en.height:   # landscape: one above the other
    pic = Image.new("RGB", (en.width, en.height * 2 + gap), (255, 0, 255))
    pic.paste(en, (0, 0))
    pic.paste(uk, (0, en.height + gap))
else:
    pic = Image.new("RGB", (en.width * 2 + gap, en.height), (255, 0, 255))
    pic.paste(en, (0, 0))
    pic.paste(uk, (en.width + gap, 0))
dst = OUT / f"pair-{view}-{shape}.png"
pic.save(dst)
print(dst)
