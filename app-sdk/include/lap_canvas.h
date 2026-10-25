/* lap_canvas.h: drawing in dots, the way COMPASS draws.
 *
 * Header only; include it after lakeshark_app.h. A canvas is a grid of
 * dots, two across and three down per cell, each one of the sixteen theme
 * colours. Draw into it with the lc_* calls, then lap_canvas_show() hands
 * it to lap_dots, which keeps the two commonest colours in every cell: an
 * ordered dither reads as shading, edges stay sharp, and the picture
 * follows the board's colour theme.
 *
 *     static lap_canvas_t cv;
 *     enter: lap_canvas_init(&cv, lap_cols(), lap_rows())   (or on first draw)
 *     draw:  lc_clear(&cv, LC_BLACK);
 *            lc_fill_circle(&cv, 40, 60, 20, LC_RED);
 *            lc_shade(&cv, x, y, 0.6f, LC_RAMP_METAL);
 *            lap_canvas_show(&cv, 0, 0);
 *
 * Coordinates are dots; y grows downward. Everything clips.
 */
#ifndef LAP_CANVAS_H
#define LAP_CANVAS_H

#include "lakeshark_app.h"
#include <math.h>
#include <string.h>

enum {
    LC_BLACK = 0, LC_RED, LC_GREEN, LC_YELLOW, LC_BLUE, LC_MAGENTA, LC_CYAN, LC_WHITE,
    LC_GREY, LC_BR_RED, LC_BR_GREEN, LC_BR_YELLOW, LC_BR_BLUE, LC_BR_MAGENTA, LC_BR_CYAN, LC_BR_WHITE,
    LC_CLEAR = LAP_DOT_CLEAR
};

typedef struct {
    uint8_t *d;
    int w, h;                   /* in dots: cells*2 by cells*3 */
    int cw, ch;                 /* in cells */
} lap_canvas_t;

/* Sizes (or resizes) for cells_w x cells_h cells. False when out of memory. */
static inline bool lap_canvas_init(lap_canvas_t *c, int cells_w, int cells_h)
{
    if (cells_w <= 0 || cells_h <= 0) return false;
    if (c->d && c->cw == cells_w && c->ch == cells_h) return true;
    free(c->d);
    c->cw = cells_w; c->ch = cells_h; c->w = cells_w * 2; c->h = cells_h * 3;
    c->d = (uint8_t *)malloc((size_t)c->w * c->h);
    return c->d != NULL;
}
static inline void lap_canvas_free(lap_canvas_t *c) { free(c->d); memset(c, 0, sizeof(*c)); }
static inline void lap_canvas_show(const lap_canvas_t *c, int cell_x, int cell_y)
{
    if (c->d) lap_dots(cell_x, cell_y, c->cw, c->ch, c->d);
}

/* ---- dots ------------------------------------------------------------- */

static inline void lc_clear(lap_canvas_t *c, uint8_t col) { if (c->d) memset(c->d, col, (size_t)c->w * c->h); }
static inline void lc_pset(lap_canvas_t *c, int x, int y, uint8_t col)
{
    if ((unsigned)x < (unsigned)c->w && (unsigned)y < (unsigned)c->h) c->d[y * c->w + x] = col;
}
static inline uint8_t lc_get(const lap_canvas_t *c, int x, int y)
{
    return (unsigned)x < (unsigned)c->w && (unsigned)y < (unsigned)c->h ? c->d[y * c->w + x] : LC_CLEAR;
}

/* ---- shading ------------------------------------------------------------ */

/* The 4x4 ordered-dither threshold COMPASS uses, 0..1. */
static inline float lc_bayer(int x, int y)
{
    static const uint8_t M[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
    return (M[y & 3][x & 3] + 0.5f) / 16.0f;
}
/* Brightness b (0..1) along a ramp of n inks, dark to light, dithered. */
static inline uint8_t lc_ramp(float b, int x, int y, const uint8_t *inks, int n)
{
    if (b <= 0) return inks[0];
    if (b >= 1) return inks[n - 1];
    const float f = b * (n - 1);
    const int lo = (int)f;
    return inks[lo + (f - lo > lc_bayer(x, y))];
}
static const uint8_t LC_RAMP_METAL[4] = { LC_BLUE, LC_GREY, LC_WHITE, LC_BR_WHITE };     /* COMPASS's bezel */
static const uint8_t LC_RAMP_GREY[4]  = { LC_BLACK, LC_GREY, LC_WHITE, LC_BR_WHITE };
static const uint8_t LC_RAMP_GREEN[4] = { LC_BLACK, LC_GREEN, LC_BR_GREEN, LC_BR_WHITE };
static const uint8_t LC_RAMP_FIRE[4]  = { LC_RED, LC_BR_RED, LC_BR_YELLOW, LC_BR_WHITE };
static const uint8_t LC_RAMP_WOOD[4]  = { LC_BLACK, LC_RED, LC_YELLOW, LC_BR_YELLOW };
static const uint8_t LC_RAMP_SEA[4]   = { LC_BLACK, LC_BLUE, LC_BR_BLUE, LC_BR_CYAN };
#define LC_RAMP_N 4
static inline void lc_shade(lap_canvas_t *c, int x, int y, float b, const uint8_t *ramp)
{
    lc_pset(c, x, y, lc_ramp(b, x, y, ramp, LC_RAMP_N));
}
/* Two inks mixed: t 0 is all a, 1 all b. */
static inline uint8_t lc_mix(uint8_t a, uint8_t b, float t, int x, int y) { return t > lc_bayer(x, y) ? b : a; }

/* ---- shapes -------------------------------------------------------------- */

static inline void lc_fill_rect(lap_canvas_t *c, int x, int y, int w, int h, uint8_t col)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y, x1 = x + w > c->w ? c->w : x + w, y1 = y + h > c->h ? c->h : y + h;
    for (int j = y0; j < y1; j++) memset(c->d + j * c->w + x0, col, x1 > x0 ? (size_t)(x1 - x0) : 0);
}
static inline void lc_rect(lap_canvas_t *c, int x, int y, int w, int h, uint8_t col)
{
    lc_fill_rect(c, x, y, w, 1, col); lc_fill_rect(c, x, y + h - 1, w, 1, col);
    lc_fill_rect(c, x, y, 1, h, col); lc_fill_rect(c, x + w - 1, y, 1, h, col);
}
static inline void lc_line(lap_canvas_t *c, int x0, int y0, int x1, int y1, uint8_t col)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, e = dx + dy;
    for (int guard = 0; guard < 4096; guard++) {
        lc_pset(c, x0, y0, col);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * e;
        if (e2 >= dy) { e += dy; x0 += sx; }
        if (e2 <= dx) { e += dx; y0 += sy; }
    }
}
static inline void lc_fill_circle(lap_canvas_t *c, int cx, int cy, int r, uint8_t col)
{
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++)
            if (x * x + y * y <= r * r + r) lc_pset(c, cx + x, cy + y, col);
}
static inline void lc_circle(lap_canvas_t *c, int cx, int cy, int r, uint8_t col)
{
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++) {
            int d = x * x + y * y;
            if (d <= r * r + r && d >= (r - 1) * (r - 1) + (r - 1)) lc_pset(c, cx + x, cy + y, col);
        }
}
/* A ball lit from the upper left: shaded along `ramp`. */
static inline void lc_ball(lap_canvas_t *c, int cx, int cy, int r, const uint8_t *ramp)
{
    if (r <= 0) return;
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++) {
            const float d2 = (float)(x * x + y * y) / (float)(r * r);
            if (d2 > 1.0f) continue;
            const float nz = sqrtf(1 - d2), nx = (float)x / r, ny = (float)y / r;
            float b = 0.15f + 0.85f * (-0.45f * nx - 0.4f * ny + 0.8f * nz);
            lc_shade(c, cx + x, cy + y, b, ramp);
        }
}

/* ---- sprites ------------------------------------------------------------- */

/* ASCII art, one string per row. A character found in `keys` takes the
   colour at the same place in `inks`; anything else ('.', ' ') is
   transparent. scale repeats each art pixel; flip mirrors left to right. */
static inline void lc_sprite(lap_canvas_t *c, int x, int y, const char *const *rows, int nrows,
                             const char *keys, const uint8_t *inks, int scale, bool flip)
{
    for (int r = 0; r < nrows; r++) {
        const char *row = rows[r];
        const int len = (int)strlen(row);
        for (int i = 0; i < len; i++) {
            const char *k = strchr(keys, row[flip ? len - 1 - i : i]);
            if (!k || !row[flip ? len - 1 - i : i]) continue;
            const uint8_t col = inks[k - keys];
            for (int sy = 0; sy < scale; sy++)
                for (int sx = 0; sx < scale; sx++) lc_pset(c, x + i * scale + sx, y + r * scale + sy, col);
        }
    }
}

/* ---- text in dots -------------------------------------------------------- */

/* 3 x 5 letters, bit 14 top-left, row by row. Digits, A-Z and a few marks. */
static inline uint16_t lc_glyph(char ch)
{
    static const uint16_t D[10] = { 0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7249, 0x7BEF, 0x7BCF };
    static const uint16_t A[26] = { 0x2BED, 0x6BAE, 0x7927, 0x6B6E, 0x79E7, 0x79E4, 0x796F, 0x5BED, 0x7497, 0x126A,
                                    0x5BAD, 0x4927, 0x5FED, 0x6B6D, 0x7B6F, 0x6BA4, 0x2B7B, 0x6BAD, 0x388E, 0x7492,
                                    0x5B6F, 0x5B6A, 0x5BFD, 0x5AAD, 0x5A92, 0x72A7 };
    if (ch >= '0' && ch <= '9') return D[ch - '0'];
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
    if (ch >= 'A' && ch <= 'Z') return A[ch - 'A'];
    switch (ch) {
    case '+': return 0x05D0; case '-': return 0x01C0; case '!': return 0x2482; case '?': return 0x6282;
    case '.': return 0x0002; case ':': return 0x0410; case '/': return 0x12A4; case '%': return 0x52A5;
    case '<': return 0x1511; case '>': return 0x4454; case '\'': return 0x2400; case '#': return 0x5F7D;
    case '*': return 0x0AA8; case '=': return 0x0E38; case '_': return 0x0007; case ',': return 0x0014;
    default: return 0;
    }
}
/* Width in dots of s at `scale`, one dot between letters. */
static inline int lc_text_w(const char *s, int scale) { int n = (int)strlen(s); return n ? (n * 4 - 1) * scale : 0; }
static inline void lc_text(lap_canvas_t *c, int x, int y, const char *s, int scale, uint8_t col)
{
    for (; *s; s++, x += 4 * scale) {
        const uint16_t g = lc_glyph(*s);
        for (int r = 0; r < 5; r++)
            for (int k = 0; k < 3; k++)
                if (g >> (14 - (r * 3 + k)) & 1)
                    for (int sy = 0; sy < scale; sy++)
                        for (int sx = 0; sx < scale; sx++) lc_pset(c, x + k * scale + sx, y + r * scale + sy, col);
    }
}
/* The same with a one-dot outline, readable over anything. */
static inline void lc_text_outlined(lap_canvas_t *c, int x, int y, const char *s, int scale, uint8_t col, uint8_t edge)
{
    for (int oy = -1; oy <= 1; oy++)
        for (int ox = -1; ox <= 1; ox++)
            if (ox || oy) lc_text(c, x + ox, y + oy, s, scale, edge);
    lc_text(c, x, y, s, scale, col);
}

/* Text whose every letter pixel is k whole cells (2k x 3k dots), placed on
   the cell grid. The board smooths stair-steps inside a cell into
   diagonals, which suits curves and spoils letters; letters made of whole
   cells are never smoothed. x, y are dots and are snapped down to a cell. */
static inline int lc_block_w(const char *s, int k) { int n = (int)strlen(s); return n ? (n * 4 - 1) * 2 * k : 0; }
static inline void lc_text_block(lap_canvas_t *c, int x, int y, const char *s, int k, uint8_t col)
{
    x -= ((x % 2) + 2) % 2; y -= ((y % 3) + 3) % 3;
    for (; *s; s++, x += 8 * k) {
        const uint16_t g = lc_glyph(*s);
        for (int r = 0; r < 5; r++)
            for (int q = 0; q < 3; q++)
                if (g >> (14 - (r * 3 + q)) & 1) lc_fill_rect(c, x + q * 2 * k, y + r * 3 * k, 2 * k, 3 * k, col);
    }
}

/* ---- small maths ---------------------------------------------------------- */

static inline float lc_clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline float lc_frand(void) { return (lap_random() & 0xFFFFFF) / 16777216.0f; }   /* 0..1 */

#endif /* LAP_CANVAS_H */
