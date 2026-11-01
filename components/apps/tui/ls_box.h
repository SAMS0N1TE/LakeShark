/* Line and shape glyphs as procedural cells, the way braille is: a cell's
   int16 `ch` holds the Unicode codepoint and both blitters draw it as lines at
   any cell size. No font change.

   - Box drawing U+2500..U+257F: light, heavy, double, dashed, rounded, half
     lines and the three diagonals.
   - Nine shapes: up, down, left, right triangles, check, bullet, close cross,
     square (maximize) and two squares (restore).

   tui_put_char is byte-sized, so these go in with ls_box_put. The terminal
   text path passes the codepoint through (ls_box_vt), a dump prints an ASCII
   stand-in (ls_box_ascii) and the web page draws them from the atlas after
   braille (ls_box_atlas). */
#ifndef LS_BOX_GLYPHS_H
#define LS_BOX_GLYPHS_H

#include <stdbool.h>
#include <stdint.h>
#include "tuilib/tui_core.h"

enum {
    LS_BOX_H      = 0x2500, LS_BOX_V      = 0x2502,
    LS_BOX_DR     = 0x250C, LS_BOX_DL     = 0x2510,   /* top left, top right */
    LS_BOX_UR     = 0x2514, LS_BOX_UL     = 0x2518,   /* bottom left, bottom right */
    LS_BOX_VR     = 0x251C, LS_BOX_VL     = 0x2524,   /* tees: right, left */
    LS_BOX_DH     = 0x252C, LS_BOX_UH     = 0x2534,   /* tees: down, up */
    LS_BOX_VH     = 0x253C,
    LS_BOX_DASH_H = 0x2504, LS_BOX_DASH_V = 0x2506,
    LS_BOX_DBL_H  = 0x2550, LS_BOX_DBL_V  = 0x2551,
    LS_BOX_DBL_DR = 0x2554, LS_BOX_DBL_DL = 0x2557,
    LS_BOX_DBL_UR = 0x255A, LS_BOX_DBL_UL = 0x255D,
    LS_BOX_DBL_VR = 0x2560, LS_BOX_DBL_VL = 0x2563,
    LS_BOX_DBL_DH = 0x2566, LS_BOX_DBL_UH = 0x2569, LS_BOX_DBL_VH = 0x256C,
    LS_BOX_ARC_DR = 0x256D, LS_BOX_ARC_DL = 0x256E,
    LS_BOX_ARC_UL = 0x256F, LS_BOX_ARC_UR = 0x2570,
    LS_BOX_DIAG_R = 0x2571, LS_BOX_DIAG_L = 0x2572, LS_BOX_DIAG_X = 0x2573,
    LS_BOX_HALF_L = 0x2574, LS_BOX_HALF_U = 0x2575,
    LS_BOX_HALF_R = 0x2576, LS_BOX_HALF_D = 0x2577,
    /* Shapes. The cross is U+2715 because U+00D7 is a byte the sextants own;
       the terminal path sends it as U+00D7. */
    LS_SHAPE_UP = 0x25B2, LS_SHAPE_DOWN = 0x25BC,
    LS_SHAPE_LEFT = 0x25C0, LS_SHAPE_RIGHT = 0x25B6,
    LS_SHAPE_CHECK = 0x2713, LS_SHAPE_DOT = 0x2022, LS_SHAPE_TIMES = 0x2715,
    LS_SHAPE_SQUARE = 0x25A1, LS_SHAPE_RESTORE = 0x29C9,
    /* Edge-aligned lines for frames whose border cells keep the ground of
       what lies outside: the line sits on the cell's edge that faces the
       inside, so no face colour shows past it. A frame's left border is
       EDGE_R (its line on the cell's right edge), its right border EDGE_L,
       its top EDGE_B, its bottom EDGE_T. Corners of such a frame are a dot
       where the two lines meet; EDGE_xy are two edges of one cell, for a
       line that turns along a cell's edges (taskbar key caps). The
       codepoints are the eighth blocks and quadrants they look like; the
       desk's cells own them (the quadrant bytes are 0x80-0x8F). */
    LS_EDGE_L = 0x258F, LS_EDGE_R = 0x2595, LS_EDGE_T = 0x2594, LS_EDGE_B = 0x2581,
    LS_EDGE_DOT_TL = 0x2598, LS_EDGE_DOT_TR = 0x259D,
    LS_EDGE_DOT_BL = 0x2596, LS_EDGE_DOT_BR = 0x2597,
    LS_EDGE_TL = 0x259B, LS_EDGE_TR = 0x259C, LS_EDGE_BL = 0x2599, LS_EDGE_BR = 0x259F,
};

#define LS_BOX_SHAPE_N 21
static const uint16_t LS_BOX_SHAPES[LS_BOX_SHAPE_N] = {
    LS_SHAPE_UP, LS_SHAPE_DOWN, LS_SHAPE_LEFT, LS_SHAPE_RIGHT, LS_SHAPE_CHECK,
    LS_SHAPE_DOT, LS_SHAPE_TIMES, LS_SHAPE_SQUARE, LS_SHAPE_RESTORE,
    LS_EDGE_L, LS_EDGE_R, LS_EDGE_T, LS_EDGE_B,
    LS_EDGE_DOT_TL, LS_EDGE_DOT_TR, LS_EDGE_DOT_BL, LS_EDGE_DOT_BR,
    LS_EDGE_TL, LS_EDGE_TR, LS_EDGE_BL, LS_EDGE_BR,
};
/* Atlas layout (lssim atlas, web/screen.js): 0-255 bytes, 256-511 braille,
   512-639 U+2500..257F, 640.. the shapes in the order above. */
#define LS_BOX_ATLAS_FIRST 512
#define LS_BOX_ATLAS_GLYPHS (640 + 32)

static inline int ls_box_shape_index(int cp)
{
    for (int i = 0; i < LS_BOX_SHAPE_N; i++) if (LS_BOX_SHAPES[i] == cp) return i;
    return -1;
}
static inline bool ls_box_is(int cp)
{
    return (cp >= 0x2500 && cp <= 0x257F) || ls_box_shape_index(cp) >= 0;
}
static inline int ls_box_atlas(int cp)
{
    if (cp >= 0x2500 && cp <= 0x257F) return LS_BOX_ATLAS_FIRST + cp - 0x2500;
    const int s = ls_box_shape_index(cp);
    return s >= 0 ? 640 + s : -1;
}

/* Into the grid, like ls_braille_put. */
static inline void ls_box_put(tui_surface *sf, tui_rect clip, int x, int y, int cp, uint8_t at)
{
    clip = tui_rect_intersect(clip, tui_surface_rect(sf));
    if (tui_rect_contains(clip, x, y)) sf->back[y * sf->w + x] = (tui_cell){(int16_t)cp, at};
}

/* Arms of U+2500..257F as 0xURDL nibbles: 0 none, 1 light, 2 heavy, 3 double,
   4/5 three dashes light/heavy, 6/7 four dashes, 8/9 two dashes. Arcs and
   diagonals are drawn by name. */
static const uint16_t LS_BOX_ARMS[128] = {
    0x0101, 0x0202, 0x1010, 0x2020, 0x0404, 0x0505, 0x4040, 0x5050,   /* 2500 */
    0x0606, 0x0707, 0x6060, 0x7070, 0x0110, 0x0210, 0x0120, 0x0220,   /* 2508 */
    0x0011, 0x0012, 0x0021, 0x0022, 0x1100, 0x1200, 0x2100, 0x2200,   /* 2510 */
    0x1001, 0x1002, 0x2001, 0x2002, 0x1110, 0x1210, 0x2110, 0x1120,   /* 2518 */
    0x2120, 0x2210, 0x1220, 0x2220, 0x1011, 0x1012, 0x2011, 0x1021,   /* 2520 */
    0x2021, 0x2012, 0x1022, 0x2022, 0x0111, 0x0112, 0x0211, 0x0212,   /* 2528 */
    0x0121, 0x0122, 0x0221, 0x0222, 0x1101, 0x1102, 0x1201, 0x1202,   /* 2530 */
    0x2101, 0x2102, 0x2201, 0x2202, 0x1111, 0x1112, 0x1211, 0x1212,   /* 2538 */
    0x2111, 0x1121, 0x2121, 0x2112, 0x2211, 0x1122, 0x1221, 0x2212,   /* 2540 */
    0x1222, 0x2122, 0x2221, 0x2222, 0x0808, 0x0909, 0x8080, 0x9090,   /* 2548 */
    0x0303, 0x3030, 0x0310, 0x0130, 0x0330, 0x0013, 0x0031, 0x0033,   /* 2550 */
    0x1300, 0x3100, 0x3300, 0x1003, 0x3001, 0x3003, 0x1310, 0x3130,   /* 2558 */
    0x3330, 0x1013, 0x3031, 0x3033, 0x0313, 0x0131, 0x0333, 0x1303,   /* 2560 */
    0x3101, 0x3303, 0x1313, 0x3131, 0x3333, 0x0110, 0x0011, 0x1001,   /* 2568 */
    0x1100, 0x0000, 0x0000, 0x0000, 0x0001, 0x1000, 0x0100, 0x0010,   /* 2570 */
    0x0002, 0x2000, 0x0200, 0x0020, 0x0201, 0x1020, 0x0102, 0x2010,   /* 2578 */
};

typedef struct { uint32_t r[32]; int w, h; } ls_box_mask_t;

static inline void ls_box_px(ls_box_mask_t *m, int x, int y)
{
    if (x >= 0 && y >= 0 && x < m->w && y < m->h) m->r[y] |= 1u << x;
}
static inline void ls_box_hl(ls_box_mask_t *m, int y, int x0, int x1)
{
    for (int x = x0; x <= x1; x++) ls_box_px(m, x, y);
}
static inline void ls_box_vl(ls_box_mask_t *m, int x, int y0, int y1)
{
    for (int y = y0; y <= y1; y++) ls_box_px(m, x, y);
}
static inline void ls_box_seg(ls_box_mask_t *m, int x0, int y0, int x1, int y1, int thick)
{
    const int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    const int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        for (int t = 0; t < thick; t++) ls_box_px(m, x0 + t, y0);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* Dashes along [a, b] in n pieces, each a bit over half its piece. */
static inline void ls_box_dash(ls_box_mask_t *m, bool horiz, int at, int len, int n, int thick)
{
    for (int k = 0; k < n; k++) {
        const int s = k * len / n, e = (k + 1) * len / n;
        const int on = (e - s) * 6 / 10 > 0 ? (e - s) * 6 / 10 : 1;
        for (int t = 0; t < thick; t++) {
            if (horiz) ls_box_hl(m, at + t, s, s + on - 1);
            else       ls_box_vl(m, at + t, s, s + on - 1);
        }
    }
}

/* The cell's ink as a bit mask, w x h (both <= 32). False for a codepoint
   that is not one of these. */
static inline bool ls_box_mask(int cp, int w, int h, ls_box_mask_t *m)
{
    if (w > 32) w = 32;
    if (h > 32) h = 32;
    for (int i = 0; i < 32; i++) m->r[i] = 0;
    m->w = w; m->h = h;
    const int cx = w / 2, cy = h / 2;
    const int shape = ls_box_shape_index(cp);
    if (shape >= 0) {
        switch (cp) {
        case LS_SHAPE_UP: case LS_SHAPE_DOWN: {
            const int yt = h * 3 / 10, yb = h * 7 / 10, hw = w * 4 / 10;
            for (int y = yt; y <= yb; y++) {
                const int half = ((y - yt + 1) * hw + (yb - yt) / 2) / (yb - yt + 1);
                const int yy = cp == LS_SHAPE_UP ? y : yt + yb - y;
                if (half > 0) ls_box_hl(m, yy, cx - half, cx + half - 1);
                else ls_box_px(m, cx, yy);
            }
            return true;
        }
        case LS_SHAPE_LEFT: case LS_SHAPE_RIGHT: {
            const int xl = w * 2 / 10, xr = w * 8 / 10 - 1, hh = h * 3 / 10;
            for (int x = xl; x <= xr; x++) {
                const int half = ((xr - x + 1) * hh + (xr - xl) / 2) / (xr - xl + 1);
                const int xx = cp == LS_SHAPE_RIGHT ? x : xl + xr - x;
                ls_box_vl(m, xx, cy - half, cy + half);
            }
            return true;
        }
        case LS_SHAPE_DOT: {
            const int r2 = (w * 6 / 10) * (w * 6 / 10);
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    const int X = 2 * x + 1 - w, Y = 2 * y + 1 - h;
                    if (X * X + Y * Y <= r2) ls_box_px(m, x, y);
                }
            return true;
        }
        case LS_SHAPE_TIMES: {
            const int s = (w < h ? w : h) * 6 / 10, x0 = (w - s) / 2, y0 = cy - s / 2;
            ls_box_seg(m, x0, y0, x0 + s - 1, y0 + s - 1, w >= 9 ? 2 : 1);
            ls_box_seg(m, x0 + s - 1, y0, x0, y0 + s - 1, w >= 9 ? 2 : 1);
            return true;
        }
        case LS_SHAPE_SQUARE: {
            const int s = w * 6 / 10, x0 = (w - s) / 2, y0 = cy - s / 2;
            ls_box_hl(m, y0, x0, x0 + s - 1); ls_box_hl(m, y0 + 1, x0, x0 + s - 1);
            ls_box_hl(m, y0 + s - 1, x0, x0 + s - 1);
            ls_box_vl(m, x0, y0, y0 + s - 1); ls_box_vl(m, x0 + s - 1, y0, y0 + s - 1);
            return true;
        }
        case LS_SHAPE_RESTORE: {
            const int s = w / 2, x0 = (w - s) / 2 + 1, y0 = cy - s / 2 - 1;
            /* the back sheet: its top and right edges */
            ls_box_hl(m, y0, x0, x0 + s - 1); ls_box_vl(m, x0 + s - 1, y0, y0 + s - 1);
            const int fx = x0 - 2, fy = y0 + 2;
            ls_box_hl(m, fy, fx, fx + s - 1); ls_box_hl(m, fy + 1, fx, fx + s - 1);
            ls_box_hl(m, fy + s - 1, fx, fx + s - 1);
            ls_box_vl(m, fx, fy, fy + s - 1); ls_box_vl(m, fx + s - 1, fy, fy + s - 1);
            return true;
        }
        case LS_EDGE_L: case LS_EDGE_R: case LS_EDGE_T: case LS_EDGE_B:
        case LS_EDGE_TL: case LS_EDGE_TR: case LS_EDGE_BL: case LS_EDGE_BR: {
            const bool l = cp == LS_EDGE_L || cp == LS_EDGE_TL || cp == LS_EDGE_BL;
            const bool r = cp == LS_EDGE_R || cp == LS_EDGE_TR || cp == LS_EDGE_BR;
            const bool t = cp == LS_EDGE_T || cp == LS_EDGE_TL || cp == LS_EDGE_TR;
            const bool b = cp == LS_EDGE_B || cp == LS_EDGE_BL || cp == LS_EDGE_BR;
            if (l) ls_box_vl(m, 0, 0, h - 1);
            if (r) ls_box_vl(m, w - 1, 0, h - 1);
            if (t) ls_box_hl(m, 0, 0, w - 1);
            if (b) ls_box_hl(m, h - 1, 0, w - 1);
            return true;
        }
        case LS_EDGE_DOT_TL: ls_box_px(m, 0, 0); return true;
        case LS_EDGE_DOT_TR: ls_box_px(m, w - 1, 0); return true;
        case LS_EDGE_DOT_BL: ls_box_px(m, 0, h - 1); return true;
        case LS_EDGE_DOT_BR: ls_box_px(m, w - 1, h - 1); return true;
        case LS_SHAPE_CHECK: {
            const int ax = w * 15 / 100, ay = h * 55 / 100, bx = w * 40 / 100, by = h * 72 / 100;
            const int ex = w * 85 / 100, ey = h * 28 / 100;
            ls_box_seg(m, ax, ay, bx, by, 2);
            ls_box_seg(m, bx, by, ex, ey, 2);
            return true;
        }
        default: return false;
        }
    }
    if (cp < 0x2500 || cp > 0x257F) return false;
    if (cp == LS_BOX_DIAG_R || cp == LS_BOX_DIAG_L || cp == LS_BOX_DIAG_X) {
        for (int y = 0; y < h; y++) {
            if (cp != LS_BOX_DIAG_L) ls_box_px(m, (h - 1 - y) * w / h, y);
            if (cp != LS_BOX_DIAG_R) ls_box_px(m, y * w / h, y);
        }
        return true;
    }
    if (cp >= LS_BOX_ARC_DR && cp <= LS_BOX_ARC_UR) {
        const bool up = cp == LS_BOX_ARC_UL || cp == LS_BOX_ARC_UR;
        const bool right = cp == LS_BOX_ARC_DR || cp == LS_BOX_ARC_UR;
        if (right) ls_box_hl(m, cy, cx + 1, w - 1); else ls_box_hl(m, cy, 0, cx - 1);
        if (up) ls_box_vl(m, cx, 0, cy - 1); else ls_box_vl(m, cx, cy + 1, h - 1);
        return true;
    }
    const uint16_t a = LS_BOX_ARMS[cp - 0x2500];
    const int U = a >> 12 & 15, R = a >> 8 & 15, D = a >> 4 & 15, L = a & 15;
    /* Dashed: one straight line in pieces. */
    const int dash = U >= 4 ? U : R >= 4 ? R : 0;
    if (dash) {
        const int n = dash <= 5 ? 3 : dash <= 7 ? 4 : 2, thick = (dash & 1) ? 2 : 1;
        if (U >= 4) ls_box_dash(m, false, cx, h, n, thick);
        else        ls_box_dash(m, true, cy, w, n, thick);
        return true;
    }
    /* Light and heavy arms through the middle. */
    const int arm[4] = {U, R, D, L};
    for (int k = 0; k < 4; k++) {
        if (arm[k] != 1 && arm[k] != 2) continue;
        const int t = arm[k];
        for (int i = 0; i < t; i++) {
            if (k == 0) ls_box_vl(m, cx + i, 0, cy + t - 1);
            if (k == 2) ls_box_vl(m, cx + i, cy, h - 1);
            if (k == 1) ls_box_hl(m, cy + i, cx, w - 1);
            if (k == 3) ls_box_hl(m, cy + i, 0, cx + t - 1);
        }
    }
    /* Double arms: per quadrant, a corner where both of its arms are
       double, else a straight side line long enough to meet its partner. */
    const int d = w >= 13 ? 2 : 1;
    const bool dU = U == 3, dR = R == 3, dD = D == 3, dL = L == 3;
    if (dU || dR || dD || dL) {
        static const int8_t Q[4][2] = {{1, -1}, {1, 1}, {-1, 1}, {-1, -1}};
        for (int q = 0; q < 4; q++) {
            const int qx = Q[q][0], qy = Q[q][1];
            const bool V = qy < 0 ? dU : dD, Hh = qx > 0 ? dR : dL;
            const int px = cx + qx * d, py = cy + qy * d;
            if (V && Hh) {
                if (qy < 0) ls_box_vl(m, px, 0, py); else ls_box_vl(m, px, py, h - 1);
                if (qx > 0) ls_box_hl(m, py, px, w - 1); else ls_box_hl(m, py, 0, px);
            } else if (V) {
                if (qy < 0) ls_box_vl(m, px, 0, cy + d); else ls_box_vl(m, px, cy - d, h - 1);
            } else if (Hh) {
                if (qx > 0) ls_box_hl(m, py, cx - d, w - 1); else ls_box_hl(m, py, 0, cx + d);
            }
        }
    }
    return true;
}

/* What a terminal shows for it: the codepoint itself, but the cross as
   U+00D7, the check as U+221A and restore as U+25A3, which every font that
   draws box lines has. */
static inline uint32_t ls_box_vt(int cp)
{
    if (cp == LS_SHAPE_TIMES) return 0x00D7;
    if (cp == LS_SHAPE_CHECK) return 0x221A;
    if (cp == LS_SHAPE_RESTORE) return 0x25A3;
    /* Frame corners are one pixel: a terminal cell shows nothing. Two-edge
       turns are the legacy-computing eighth corners (U+1FB7C..1FB7F). */
    if (cp == LS_EDGE_DOT_TL || cp == LS_EDGE_DOT_TR || cp == LS_EDGE_DOT_BL || cp == LS_EDGE_DOT_BR) return 0x20;
    if (cp == LS_EDGE_BL) return 0x1FB7C;
    if (cp == LS_EDGE_TL) return 0x1FB7D;
    if (cp == LS_EDGE_TR) return 0x1FB7E;
    if (cp == LS_EDGE_BR) return 0x1FB7F;
    return (uint32_t)cp;
}

/* A printable stand-in for dumps. */
static inline char ls_box_ascii(int cp)
{
    switch (cp) {
    case LS_SHAPE_UP: return '^';
    case LS_SHAPE_DOWN: return 'v';
    case LS_SHAPE_LEFT: return '<';
    case LS_SHAPE_RIGHT: return '>';
    case LS_SHAPE_CHECK: return 'v';
    case LS_SHAPE_DOT: return 'o';
    case LS_SHAPE_TIMES: return 'x';
    case LS_SHAPE_SQUARE: return 'O';
    case LS_SHAPE_RESTORE: return '&';
    case LS_EDGE_L: case LS_EDGE_R: return '|';
    case LS_EDGE_T: case LS_EDGE_B: return '-';
    case LS_EDGE_DOT_TL: case LS_EDGE_DOT_TR: case LS_EDGE_DOT_BL: case LS_EDGE_DOT_BR:
    case LS_EDGE_TL: case LS_EDGE_TR: case LS_EDGE_BL: case LS_EDGE_BR: return '+';
    case LS_BOX_DIAG_R: return '/';
    case LS_BOX_DIAG_L: return '\\';
    case LS_BOX_DIAG_X: return 'X';
    default: break;
    }
    if (cp < 0x2500 || cp > 0x257F) return '?';
    if (cp >= LS_BOX_ARC_DR && cp <= LS_BOX_ARC_UR) return '+';
    const uint16_t a = LS_BOX_ARMS[cp - 0x2500];
    const int U = a >> 12 & 15, R = a >> 8 & 15, D = a >> 4 & 15, L = a & 15;
    const bool v = U || D, hz = R || L;
    if (U >= 4 || R >= 4) return U >= 4 ? ':' : '-';
    if (v && hz) return '+';
    if (hz) return (R == 3 || L == 3) ? '=' : '-';
    if (v) return '|';
    return ' ';
}

#endif
