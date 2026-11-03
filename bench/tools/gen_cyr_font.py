"""Cyrillic glyph pages for the TUI fonts, from the same faces as the Latin tables.

The Latin tables came from two renderers, and each Cyrillic page uses the one
its Latin table came from, so the letters sit side by side without a seam:

  ls_font_mono_14 / _16   DejaVu Sans Mono, lv_font_conv defaults: FreeType
                          light autohinting, 8-bit coverage >> 4.
  ls_font_mono_24         DejaVu Sans Mono Bold through Pillow, exactly as
                          tools/gen_cell_font.py draws it.

Before writing anything the script re-renders the Latin table of each face
the same way and stops unless every glyph matches the shipped table bit for
bit. That is the proof that the renderer, size, hinting and baseline are the
ones the existing tables used.

A Cyrillic letter whose glyph is identical to a Latin one in all three faces
(A and the Cyrillic A, for example) is not stored at all: the string encoding
sends it to the Latin byte. The rest get a byte of their own from the bytes no
TUI glyph uses (see tui_cyr.h). Glyphs are clipped to their cell, the way
every Latin glyph already fits one.

Run from the repository root:

  python bench/tools/gen_cyr_font.py --freetype "C:/Program Files/FreeCAD 1.0/bin/freetype.dll"

Any 64-bit FreeType 2.x DLL works. Font licence: Bitstream Vera terms with
DejaVu changes in the public domain, components/apps/tui/DEJAVU-LICENSE.txt.
"""
import argparse
import ctypes as C
import os
import re
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

REPO = Path(__file__).resolve().parents[2]
TUI = REPO / "components/apps/tui"

# Ukrainian alphabet, both cases, in alphabetical order, then the four letters
# only Russian uses (cheap: eight glyphs a face).
UK_UPPER = "АБВГҐДЕЄЖЗИІЇЙКЛМНОПРСТУФХЦЧШЩЬЮЯ"
UK_LOWER = "абвгґдеєжзиіїйклмнопрстуфхцчшщьюя"
RU_EXTRA = "ЁЪЫЭёъыэ"
LETTERS = UK_UPPER + UK_LOWER + RU_EXTRA

# Bytes no TUI glyph uses and no string carries: control bytes other than
# tab, line feed, carriage return and escape, and the gaps between the
# graphics glyphs (0x80-0x93 quadrants and shades, 0xA0-0xA7 traces, 0xB0
# the picture cell, 0xC0-0xFF sextants). Order is the order slots are given.
FREE_BYTES = ([b for b in range(0x01, 0x20) if b not in (0x09, 0x0A, 0x0D, 0x1B)] +
              list(range(0x94, 0xA0)) + list(range(0xA8, 0xB0)) + list(range(0xB1, 0xC0)))

FT_LOAD_RENDER = 0x4
FT_LOAD_FORCE_AUTOHINT = 0x20
FT_LOAD_TARGET_LIGHT = 1 << 16


class FT_Bitmap(C.Structure):
    _fields_ = [("rows", C.c_uint), ("width", C.c_uint), ("pitch", C.c_int),
                ("buffer", C.POINTER(C.c_ubyte)), ("num_grays", C.c_ushort),
                ("pixel_mode", C.c_ubyte), ("palette_mode", C.c_ubyte), ("palette", C.c_void_p)]


class FT_Vector(C.Structure):
    _fields_ = [("x", C.c_long), ("y", C.c_long)]


class FT_Generic(C.Structure):
    _fields_ = [("data", C.c_void_p), ("finalizer", C.c_void_p)]


class FT_GlyphSlotRec(C.Structure):
    _fields_ = [("library", C.c_void_p), ("face", C.c_void_p), ("next", C.c_void_p),
                ("glyph_index", C.c_uint), ("generic", FT_Generic), ("metrics", C.c_long * 8),
                ("linearHoriAdvance", C.c_long), ("linearVertAdvance", C.c_long),
                ("advance", FT_Vector), ("format", C.c_ulong), ("bitmap", FT_Bitmap),
                ("bitmap_left", C.c_int), ("bitmap_top", C.c_int)]


class FT_FaceRec(C.Structure):
    _fields_ = [("num_faces", C.c_long), ("face_index", C.c_long), ("face_flags", C.c_long),
                ("style_flags", C.c_long), ("num_glyphs", C.c_long), ("family_name", C.c_char_p),
                ("style_name", C.c_char_p), ("num_fixed_sizes", C.c_int), ("available_sizes", C.c_void_p),
                ("num_charmaps", C.c_int), ("charmaps", C.c_void_p), ("generic", FT_Generic),
                ("bbox", C.c_long * 4), ("units_per_EM", C.c_ushort), ("ascender", C.c_short),
                ("descender", C.c_short), ("height", C.c_short), ("max_advance_width", C.c_short),
                ("max_advance_height", C.c_short), ("underline_position", C.c_short),
                ("underline_thickness", C.c_short), ("glyph", C.POINTER(FT_GlyphSlotRec))]


class FreeTypeFace:
    """lv_font_conv's default render: light autohinting, coverage >> 4."""

    def __init__(self, dll, ttf, px):
        os.add_dll_directory(os.path.dirname(os.path.abspath(dll)))
        self.ft = C.CDLL(dll)
        lib = C.c_void_p()
        if self.ft.FT_Init_FreeType(C.byref(lib)):
            raise SystemExit("FreeType did not start")
        self.face = C.POINTER(FT_FaceRec)()
        if self.ft.FT_New_Face(lib, str(ttf).encode(), 0, C.byref(self.face)):
            raise SystemExit(f"cannot open {ttf}")
        self.px = px
        self.ft.FT_Set_Pixel_Sizes(self.face, 0, px)

    def glyph(self, ch, cell):
        """(box_w, box_h, ofs_x, ofs_y, 4-bit pixels) or None for no ink."""
        return self.render(ch, self.px)

    # A capital whose mark rises out of the cell at 14 and 16 px, the capital
    # it is built on, and the small letter whose mark it borrows.
    MARKED = {"Й": ("И", "й"), "Ї": ("І", "ї"), "Ё": ("Е", "ё")}

    def fitted(self, ch, cell):
        """The glyph as it renders, except a marked capital that would not fit
        its cell: that one is the plain capital with its least telling rows
        taken out and the small letter's mark set above it, so the capital
        keeps its height and its strokes and the mark is not cut off."""
        cw, chh, base = cell
        g = self.render(ch, self.px)
        if not g or g[1] + g[3] <= chh - base or ch not in self.MARKED:
            return g
        cap, small = self.MARKED[ch]
        body = self.render(cap, self.px)
        mark = self.render(small, self.px)
        room = chh - base                       # rows above the baseline
        # the mark: the small letter's rows above its first blank row
        mw, mh, mx, my, mp = mark
        mrows = [mp[y * mw:(y + 1) * mw] for y in range(mh)]
        k = 0
        while k < len(mrows) and any(mrows[k]):
            k += 1
        mrows = mrows[:k]
        bw, bh, bx, by, bp = body
        brows = [bp[y * bw:(y + 1) * bw] for y in range(bh)]
        # The capital gives up one row, no more; the mark and a row of air
        # share what is left above it, the mark keeping its inkiest rows.
        avail = room - (bh - 1)
        gap = 1 if avail >= 2 else 0
        keep = max(1, avail - gap)
        while len(mrows) > keep:
            weakest = min(range(len(mrows)), key=lambda i: sum(mrows[i]))
            mrows.pop(weakest)
        def thin(rows, n):
            """Take n rows out where a row most repeats its neighbour."""
            rows = list(rows)
            for _ in range(n):
                if len(rows) <= 4:
                    break
                best = min(range(2, len(rows) - 2),
                           key=lambda i: sum(abs(a - b) for a, b in zip(rows[i], rows[i - 1])))
                rows.pop(best)
            return rows
        while len(mrows) > 2 and len(mrows) + gap + len(brows) > room:
            mrows = thin(mrows, 1) if len(mrows) > 4 else mrows[:1] + mrows[2:]
        brows = thin(brows, max(0, len(mrows) + gap + len(brows) - room))
        # one bitmap, the mark placed by its own left offset over the capital
        left = min(bx, mx)
        right = max(bx + bw, mx + mw)
        w = right - left
        rows = []
        for r in mrows:
            rows.append([0] * (mx - left) + r + [0] * (right - mx - mw))
        rows += [[0] * w for _ in range(gap)]
        for r in brows:
            rows.append([0] * (bx - left) + r + [0] * (right - bx - bw))
        return (w, len(rows), left, by, [v for r in rows for v in r])

    def render(self, ch, py):
        self.ft.FT_Set_Pixel_Sizes(self.face, self.px, py)
        gi = self.ft.FT_Get_Char_Index(self.face, C.c_ulong(ord(ch)))
        if not gi:
            raise SystemExit(f"U+{ord(ch):04X} is not in the face")
        self.ft.FT_Load_Glyph(self.face, gi, C.c_int(FT_LOAD_RENDER | FT_LOAD_FORCE_AUTOHINT |
                                                    FT_LOAD_TARGET_LIGHT))
        g = self.face.contents.glyph.contents
        bm = g.bitmap
        if not bm.width or not bm.rows:
            self.ft.FT_Set_Pixel_Sizes(self.face, 0, self.px)
            return None
        px = [bm.buffer[y * bm.pitch + x] >> 4 for y in range(bm.rows) for x in range(bm.width)]
        self.ft.FT_Set_Pixel_Sizes(self.face, 0, self.px)
        return (bm.width, bm.rows, g.bitmap_left, g.bitmap_top - bm.rows, px)


class PillowFace:
    """tools/gen_cell_font.py: the bold face drawn at its final size."""

    def __init__(self, ttf, px):
        self.font = ImageFont.truetype(str(ttf), px)

    def fitted(self, ch, cell):
        return self.glyph(ch, cell)

    def glyph(self, ch, cell):
        cw, chh, base = cell
        baseline = chh - base
        image = Image.new("L", (cw, chh))
        ImageDraw.Draw(image).text((0, baseline), ch, font=self.font, fill=255, anchor="ls")
        box = image.getbbox()
        if box is None:
            return None
        x, y, right, bottom = box
        w, h = right - x, bottom - y
        px = [(v * 15 + 127) // 255 for v in image.crop(box).tobytes()]
        return (w, h, x, baseline - bottom, px)


def clip(w, h, ox, oy, px, cell):
    """Crop a glyph to its cell; the Latin tables never draw outside one."""
    cw, chh, base = cell
    top = (chh - base) - h - oy          # cell row of the glyph's first row
    x0, x1 = max(0, -ox), min(w, cw - ox)
    y0, y1 = max(0, -top), min(h, chh - top)
    rows = [px[y * w + x0:y * w + x1] for y in range(y0, y1)]
    # trim rows and columns left blank by the crop
    while rows and not any(rows[0]):
        rows.pop(0); y0 += 1
    while rows and not any(rows[-1]):
        rows.pop(); y1 -= 1
    if not rows:
        return None
    nw = x1 - x0
    nh = len(rows)
    flat = [v for r in rows for v in r]
    return (nw, nh, ox + x0, oy + (h - y1), flat)


def parse_table(path):
    text = path.read_text()
    dsc = [tuple(int(v) for v in m.groups()) for m in re.finditer(
        r"\{\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+)\s*\}", text)][:95]
    body = text[text.index("uint8_t", text.index("};")):]
    data = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", body[:body.index("};")])]
    cell = tuple(int(re.search(rf"\.{k}\s*=\s*(\d+)", text).group(1))
                 for k in ("cell_w", "cell_h", "base_line"))
    glyphs = {}
    for i, (idx, w, h, ox, oy) in enumerate(dsc):
        if not w or not h:
            glyphs[chr(0x20 + i)] = None
            continue
        px = [(data[idx + (b >> 1)] & 0x0F) if b & 1 else (data[idx + (b >> 1)] >> 4)
              for b in range(w * h)]
        glyphs[chr(0x20 + i)] = (w, h, ox, oy, px)
    return glyphs, cell


def pack(px):
    if len(px) % 2:
        px = px + [0]
    return [(px[i] << 4) | px[i + 1] for i in range(0, len(px), 2)]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--freetype", required=True, help="path to a 64-bit freetype.dll")
    ap.add_argument("--regular", default="C:/Windows/Fonts/DejaVuSansMono.ttf")
    ap.add_argument("--bold", default=str(REPO / "integrations/cell-lab/fonts/DejaVuSansMono-Bold.ttf"))
    args = ap.parse_args()

    faces = [
        ("ls_font_mono_16", FreeTypeFace(args.freetype, args.regular, 16)),
        ("ls_font_mono_14", FreeTypeFace(args.freetype, args.regular, 14)),
        ("ls_font_mono_24", PillowFace(args.bold, 24)),
        # Small glass faces from bench/tools/gen_small_font.py, same renderer as 14/16.
        ("ls_font_mono_13", FreeTypeFace(args.freetype, args.regular, 13)),
        ("ls_font_mono_11", FreeTypeFace(args.freetype, args.regular, 11)),
    ]
    latin, cells = {}, {}
    for name, face in faces:
        table, cell = parse_table(TUI / f"{name}.c")
        cells[name] = cell
        bad = [c for c in table if c != " " and face.glyph(c, cell) != table[c]]
        if bad:
            raise SystemExit(f"{name}: the renderer does not reproduce {''.join(bad)!r}; refusing")
        latin[name] = table
        print(f"{name}: all 94 Latin glyphs reproduced bit for bit")

    rendered = {}
    for name, face in faces:
        rendered[name] = {}
        for c in LETTERS:
            g = face.fitted(c, cells[name])
            k = clip(*g, cells[name]) if g else None
            if k != g and (k is None or k[:2] != g[:2]):
                print(f"{name}: {c} clipped to its cell, {g[0]}x{g[1]} -> {k[0]}x{k[1]}")
            rendered[name][c] = k

    # Homoglyphs: identical to a Latin letter in every face.
    alias = {}
    for c in LETTERS:
        for a in "ABCEHIKMOPTXaceiopxy":
            if all(rendered[n][c] == latin[n][a] for n, _ in faces):
                alias[c] = a
                break
    own = [c for c in LETTERS if c not in alias]
    if len(own) > len(FREE_BYTES):
        raise SystemExit(f"{len(own)} letters need a byte, only {len(FREE_BYTES)} are free")
    slot_byte = FREE_BYTES[:len(own)]
    print(f"{len(alias)} letters share a Latin glyph: " +
          " ".join(f"{c}={a}" for c, a in alias.items()))
    print(f"{len(own)} letters get a byte of their own")

    # tuilib/tui_cyr.h: the string side.
    slot_of = [-1] * 256
    for i, b in enumerate(slot_byte):
        slot_of[b] = i
    pairs = sorted([(ord(c), alias[c]) for c in alias] +
                   [(ord(c), slot_byte[own.index(c)]) for c in own])
    h = ["/* Generated by bench/tools/gen_cyr_font.py. Do not edit.",
         "   Cyrillic in one byte a character, the TUI's cell encoding. A letter",
         "   drawn the same as a Latin one is that Latin byte; every other letter",
         "   has one of the bytes no TUI glyph uses. Included by tui_core.c only. */",
         "#ifndef TUI_CYR_H", "#define TUI_CYR_H", "",
         f"#define TUI_CYR_SLOTS {len(own)}", "",
         "/* Codepoint and cell byte, sorted by codepoint. */",
         "static const struct { uint16_t cp; uint8_t byte; } TUI_CYR_MAP[] = {"]
    for i in range(0, len(pairs), 6):
        h.append("    " + " ".join(
            f"{{0x{cp:04X},{(f'0x{b:02X}' if isinstance(b, int) else repr(b)):>4}}},"
            for cp, b in pairs[i:i + 6]))
    h += ["};", "", "/* The codepoint each slot draws, for dumps that print text. */",
          "static const uint16_t TUI_CYR_SLOT_CP[TUI_CYR_SLOTS] = {"]
    for i in range(0, len(own), 10):
        h.append("    " + " ".join(f"0x{ord(c):04X}," for c in own[i:i + 10]))
    h += ["};", "", "/* Slot of a cell byte, or -1. */",
          "static const int8_t TUI_CYR_SLOT_OF[256] = {"]
    for i in range(0, 256, 16):
        h.append("    " + " ".join(f"{v:2d}," for v in slot_of[i:i + 16]))
    h += ["};", "", "#endif /* TUI_CYR_H */", ""]
    (TUI / "tuilib/tui_cyr.h").write_text("\n".join(h), encoding="utf-8", newline="\n")

    # ls_font_cyr.c: one page a face, indexed by slot.
    out = ["/* Generated by bench/tools/gen_cyr_font.py. Do not edit.",
           "   Cyrillic glyph pages for the TUI faces, one glyph a slot (tui_cyr.h),",
           "   rendered the way each face's Latin table was. DejaVu Sans Mono;",
           "   licence: components/apps/tui/DEJAVU-LICENSE.txt. */",
           '#include "ls_font.h"', ""]
    total = 0
    for name, _ in faces:
        cw, chh, base = cells[name]
        dsc, data = [], []
        for c in own:
            g = rendered[name][c]
            if g is None:
                dsc.append((len(data), 0, 0, 0, 0))
                continue
            w, hh, ox, oy, px = g
            dsc.append((len(data), w, hh, ox, oy))
            data.extend(pack(px))
        total += len(data) + len(dsc) * 6
        tag = name.replace("ls_font_mono_", "")
        out.append(f"static const ls_font_glyph_t cyr_dsc_{tag}[{len(own)}] = {{")
        for c, d in zip(own, dsc):
            out.append(f"    {{ {d[0]:5d}, {d[1]:2d}, {d[2]:2d}, {d[3]:3d}, {d[4]:3d} }},  /* U+{ord(c):04X} */")
        out.append("};")
        out.append(f"static const uint8_t cyr_bmp_{tag}[{len(data)}] = {{")
        for i in range(0, len(data), 16):
            out.append("    " + ", ".join(f"0x{b:02X}" for b in data[i:i + 16]) + ",")
        out.append("};")
        out.append(f"static const ls_font_t ls_font_cyr_{tag} = {{")
        out.append(f"    .dsc = cyr_dsc_{tag}, .bitmap = cyr_bmp_{tag}, .bitmap_len = {len(data)},")
        out.append(f"    .first = 0, .count = {len(own)}, .cell_w = {cw}, .cell_h = {chh},")
        out.append(f'    .base_line = {base}, .name = "ls_font_cyr_{tag}",')
        out.append("};")
        out.append("")
    out += ["/* By cell size, which is unique to each face, so a build that links",
            "   only some of the faces still links. */",
            "const ls_font_t *ls_font_cyr_page(const ls_font_t *latin)",
            "{",
            "    static const ls_font_t *const PAGES[] = { " + ", ".join(f"&ls_font_cyr_{n.replace('ls_font_mono_', '')}" for n, _ in faces) + " };",
            "    if (!latin) return NULL;",
            "    for (unsigned i = 0; i < sizeof(PAGES) / sizeof(PAGES[0]); i++)",
            "        if (PAGES[i]->cell_w == latin->cell_w && PAGES[i]->cell_h == latin->cell_h &&",
            "            PAGES[i]->base_line == latin->base_line) return PAGES[i];",
            "    return NULL;",
            "}", ""]
    (TUI / "ls_font_cyr.c").write_text("\n".join(out), encoding="utf-8", newline="\n")
    print(f"ls_font_cyr.c: {total} bytes of glyph data and descriptors in {len(faces)} faces")

    # A proof sheet: every letter in every face, for a look.
    sheet = REPO / "bench/build-uk/cyr_sheet.png"
    sheet.parent.mkdir(parents=True, exist_ok=True)
    rows = []
    for name, _ in faces:
        cw, chh, base = cells[name]
        line = "ABCabc " + LETTERS
        img = Image.new("L", (cw * len(line), chh), 0)
        for k, c in enumerate(line):
            g = latin[name].get(c) if c in latin[name] else (
                latin[name][alias[c]] if c in alias else rendered[name][c])
            if not g:
                continue
            w, hh, ox, oy, px = g
            top = (chh - base) - hh - oy
            for y in range(hh):
                for x in range(w):
                    X, Y = k * cw + ox + x, top + y
                    if 0 <= X < img.width and 0 <= Y < chh:
                        img.putpixel((X, Y), px[y * w + x] * 17)
        rows.append(img.resize((img.width * 2, img.height * 2), Image.NEAREST))
    full = Image.new("L", (max(r.width for r in rows), sum(r.height + 6 for r in rows)), 40)
    y = 0
    for r in rows:
        full.paste(r, (0, y)); y += r.height + 6
    full.save(sheet)
    print(f"proof sheet: {sheet}")


if __name__ == "__main__":
    sys.exit(main())
