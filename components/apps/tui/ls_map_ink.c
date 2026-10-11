/* See ls_map_ink.h. */
#include "ls_map_ink.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"

#include "ls_tui.h"

#define INK_COLS 160
#define INK_ROWS 100

/* PSRAM: three bytes a cell for the largest map pane, touched only by the
   UI task's draw. */
EXT_RAM_BSS_ATTR static uint8_t s_bits[INK_COLS * INK_ROWS];
EXT_RAM_BSS_ATTR static uint8_t s_attr[INK_COLS * INK_ROWS];
EXT_RAM_BSS_ATTR static uint8_t s_prio[INK_COLS * INK_ROWS];
static tui_rect s_rect;
static bool     s_on;
static uint8_t  s_tint;

void ls_ink_set_tint(uint8_t base) { s_tint = (uint8_t)(base & 7); }

static uint8_t tint_nibble(uint8_t n)
{
    const uint8_t c = n & 7, bright = n & 8;
    /* The ground stays the ground; the dim grey of furniture becomes the
       dim tint; every colour takes the tint at its own brightness. */
    if (c == 0) return bright ? s_tint : 0;
    return (uint8_t)(s_tint | bright);
}

uint8_t ls_ink_tinted(uint8_t attr)
{
    if (!s_tint) return attr;
    return TUI_ATTR(tint_nibble(TUI_ATTR_FG(attr)), tint_nibble(TUI_ATTR_BG(attr)));
}

bool ls_ink_begin(tui_rect cells)
{
    s_on = cells.w > 0 && cells.h > 0 && cells.w <= INK_COLS && cells.h <= INK_ROWS;
    s_rect = cells;
    if (!s_on) return false;
    for (int r = 0; r < cells.h; r++) {
        memset(s_bits + r * INK_COLS, 0, (size_t)cells.w);
        memset(s_prio + r * INK_COLS, 0, (size_t)cells.w);
    }
    return true;
}

void ls_ink_dot(int x, int y, uint8_t attr, uint8_t prio)
{
    if (!s_on || x < 0 || y < 0 || x >= s_rect.w * 2 || y >= s_rect.h * 3) return;
    const int col = x / 2, row = y / 3;
    const int i = row * INK_COLS + col;
    s_bits[i] |= (uint8_t)(1u << ((y % 3) * 2 + (x % 2)));
    if (prio >= s_prio[i]) { s_prio[i] = prio ? prio : 1; s_attr[i] = attr; }
}

static bool on_dash(int dash, int k)
{
    if (!dash) return true;
    if (dash > 0) return (k / dash) % 2 == 0;
    return k % (-dash) == 0;
}

void ls_ink_line(int x0, int y0, int x1, int y1, uint8_t attr, uint8_t prio, int dash)
{
    if (!s_on) return;
    /* A line wholly off one side of the canvas draws nothing; long ones
       from far off the pane are clipped to a sane length first. */
    const int w = s_rect.w * 2, h = s_rect.h * 3;
    if ((x0 < 0 && x1 < 0) || (y0 < 0 && y1 < 0) || (x0 >= w && x1 >= w) || (y0 >= h && y1 >= h)) return;
    int dx = abs(x1 - x0), dy = -abs(y1 - y0);
    if (dx > 4 * w || -dy > 4 * h) return;
    const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, k = 0;
    for (;;) {
        if (on_dash(dash, k)) ls_ink_dot(x0, y0, attr, prio);
        k++;
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void ls_ink_ellipse(int cx, int cy, int rx, int ry, uint8_t attr, uint8_t prio, int dash)
{
    if (!s_on || rx < 1 || ry < 1) return;
    /* Enough steps that neighbouring dots touch, capped for a ring far
       larger than the pane. */
    int steps = (int)(2.0 * M_PI * (rx > ry ? rx : ry));
    if (steps < 12) steps = 12;
    if (steps > 1200) steps = 1200;
    /* A unit vector turned a step at a time: two multiplies and adds per
       dot instead of a sine and a cosine. Single precision, which is all
       this part has an FPU for. */
    const float c = cosf(6.2831853f / steps), s = sinf(6.2831853f / steps);
    float ux = 0.0f, uy = 1.0f;
    const int w = s_rect.w * 2, h = s_rect.h * 3;
    int px = 0x7FFFFFFF, py = 0, k = 0;
    for (int i = 0; i < steps; i++) {
        const int x = cx + (int)lrintf(rx * ux);
        const int y = cy - (int)lrintf(ry * uy);
        const float nx = ux * c + uy * s;
        uy = uy * c - ux * s;
        ux = nx;
        if (x == px && y == py) continue;
        px = x; py = y;
        if (on_dash(dash, k) && x >= 0 && y >= 0 && x < w && y < h) ls_ink_dot(x, y, attr, prio);
        k++;
    }
}

bool ls_ink_cell_used(int col, int row)
{
    if (!s_on || col < 0 || row < 0 || col >= s_rect.w || row >= s_rect.h) return false;
    return s_bits[row * INK_COLS + col] != 0;
}

int ls_ink_cell_prio(int col, int row)
{
    if (!s_on || col < 0 || row < 0 || col >= s_rect.w || row >= s_rect.h) return 0;
    return s_bits[row * INK_COLS + col] ? s_prio[row * INK_COLS + col] : 0;
}

void ls_ink_flush(tui_surface *sf, tui_rect skip)
{
    if (!s_on) return;
    for (int r = 0; r < s_rect.h; r++) {
        const int y = s_rect.y + r;
        for (int c = 0; c < s_rect.w; c++) {
            const uint8_t b = s_bits[r * INK_COLS + c];
            if (!b) continue;
            const int x = s_rect.x + c;
            if (skip.h > 0 && x >= skip.x && x < skip.x + skip.w &&
                y >= skip.y && y < skip.y + skip.h) continue;
            uint16_t glyph=LS_TUI_SEXT(b);
            if(s_prio[r*INK_COLS+c]<=3) {
                static const uint8_t dots[6]={1,8,2,16,4,32};
                glyph=0x2800;for(int k=0;k<6;k++) if(b&(1u<<k)) glyph|=dots[k];
            }
            ls_tui_put_glass(sf, s_rect, x, y, glyph, ls_ink_tinted(s_attr[r * INK_COLS + c]));
        }
    }
}

void ls_ink_text(tui_surface *sf, tui_rect clip, int x, int y, const char *s, int n, uint8_t attr)
{
    attr = ls_ink_tinted(attr);
    /* `n` is bytes of UTF-8; each character takes one cell, folded to ASCII. */
    const char *p = s, *end = s + n;
    for (int i = 0; p < end && *p; i++) {
        /* A space is see-through: the cell is left to the picture, which
           also clears anything the canvas put there. */
        ls_tui_put_glass(sf, clip, x + i, y, tui_utf8_fold(&p), attr);
    }
}
