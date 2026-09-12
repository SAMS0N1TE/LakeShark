/* See ls_glyph.h. Three by five strokes, and the scaling that puts them on
   a key. */
#include "ls_glyph.h"

#include "ls_tui_ui.h"

static const char *const DIGIT[10][LS_GLYPH_ROWS] = {
    { "XXX", "X X", "X X", "X X", "XXX" },   /* 0 */
    { "  X", "  X", "  X", "  X", "  X" },   /* 1 */
    { "XXX", "  X", "XXX", "X  ", "XXX" },   /* 2 */
    { "XXX", "  X", "XXX", "  X", "XXX" },   /* 3 */
    { "X X", "X X", "XXX", "  X", "  X" },   /* 4 */
    { "XXX", "X  ", "XXX", "  X", "XXX" },   /* 5 */
    { "XXX", "X  ", "XXX", "X X", "XXX" },   /* 6 */
    { "XXX", "  X", "  X", "  X", "  X" },   /* 7 */
    { "XXX", "X X", "XXX", "X X", "XXX" },   /* 8 */
    { "XXX", "X X", "XXX", "  X", "XXX" },   /* 9 */
};

static const char *const ALPHA[26][LS_GLYPH_ROWS] = {
    { " X ", "X X", "XXX", "X X", "X X" },   /* A */
    { "XX ", "X X", "XX ", "X X", "XX " },   /* B */
    { "XXX", "X  ", "X  ", "X  ", "XXX" },   /* C */
    { "XX ", "X X", "X X", "X X", "XX " },   /* D */
    { "XXX", "X  ", "XX ", "X  ", "XXX" },   /* E */
    { "XXX", "X  ", "XX ", "X  ", "X  " },   /* F */
    { "XXX", "X  ", "X X", "X X", "XXX" },   /* G */
    { "X X", "X X", "XXX", "X X", "X X" },   /* H */
    { "XXX", " X ", " X ", " X ", "XXX" },   /* I */
    { "  X", "  X", "  X", "X X", "XXX" },   /* J */
    { "X X", "X X", "XX ", "X X", "X X" },   /* K */
    { "X  ", "X  ", "X  ", "X  ", "XXX" },   /* L */
    { "XXX", "XXX", "X X", "X X", "X X" },   /* M */
    { "XX ", "X X", "X X", "X X", "X X" },   /* N */
    { "XXX", "X X", "X X", "X X", "XXX" },   /* O */
    { "XXX", "X X", "XXX", "X  ", "X  " },   /* P */
    { "XXX", "X X", "X X", "XXX", "  X" },   /* Q */
    { "XXX", "X X", "XX ", "X X", "X X" },   /* R */
    { "XXX", "X  ", "XXX", "  X", "XXX" },   /* S */
    { "XXX", " X ", " X ", " X ", " X " },   /* T */
    { "X X", "X X", "X X", "X X", "XXX" },   /* U */
    { "X X", "X X", "X X", "X X", " X " },   /* V */
    { "X X", "X X", "X X", "XXX", "XXX" },   /* W */
    { "X X", "X X", " X ", "X X", "X X" },   /* X */
    { "X X", "X X", " X ", " X ", " X " },   /* Y */
    { "XXX", "  X", " X ", "X  ", "XXX" },   /* Z */
};

static const char *const *figure(char c)
{
    if (c >= '0' && c <= '9') return DIGIT[c - '0'];
    if (c >= 'a' && c <= 'z') return ALPHA[c - 'a'];
    if (c >= 'A' && c <= 'Z') return ALPHA[c - 'A'];
    return NULL;
}

bool ls_glyph_has(char c) { return figure(c) != NULL; }

void ls_glyph_draw(tui_surface *sf, tui_rect a, char c, uint8_t at)
{
    if (a.w <= 0 || a.h <= 0) return;

    if (c == '.') {
        const int x = a.x + (a.w - 2) / 2, y = a.y + a.h - 2;
        tui_put_char(sf, a, x, y, LS_TUI_SHADE_FULL, at);
        tui_put_char(sf, a, x + 1, y, LS_TUI_SHADE_FULL, at);
        return;
    }
    const char *const *fig = figure(c);
    if (!fig) return;

    int sx = a.w / (LS_GLYPH_COLS + 2);
    if (sx < 1) sx = 1;
    if (sx > 3) sx = 3;
    int sy = a.h / (LS_GLYPH_ROWS + 1);
    if (sy < 1) sy = 1;
    if (sy > 2) sy = 2;

    const int fw = LS_GLYPH_COLS * sx, fh = LS_GLYPH_ROWS * sy;
    const int ox = a.x + (a.w - fw) / 2, oy = a.y + (a.h - fh) / 2;
    for (int r = 0; r < fh; r++)
        for (int c2 = 0; c2 < fw; c2++)
            if (fig[r / sy][c2 / sx] == 'X')
                tui_put_char(sf, a, ox + c2, oy + r, LS_TUI_SHADE_FULL, at);
}
