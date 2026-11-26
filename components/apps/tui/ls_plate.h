/* Plates: instrument panels for the AMOLED character grid, in the house
   style - black ground (black is off on the panel; never a grey fill),
   coloured ASCII frames with heavier '=' edges, a solid caption tab, ASCII
   meters, braille graph columns, and keys drawn like the raised ASCII keys.

   Rules that keep a plate readable at 52 columns:
   - one headline per plate (ls_calls_big_text, braille, 3 rows), everything
     else ordinary text; bright for values, plain for labels, dim for units;
   - a blank row between groups; a cell of air inside the frame;
   - colour means something (a job, a class, a state), never decoration;
   - blocks only as small accents (a lamp, a mark), never fills or bars.

   Header only, so a module (WARDRIVE, PARTY) needs no new firmware export. */
#ifndef LS_PLATE_H
#define LS_PLATE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "ls_tui.h"
#include "ls_tui_ui.h"
#include "ls_braille.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LS_PLATE_GROUND   TUI_BLACK      /* AMOLED: black stays black */

/* Text on a plate. */
static inline uint8_t ls_plate_at(uint8_t fg) { return TUI_ATTR(fg, LS_PLATE_GROUND); }
#define LS_PLATE_VALUE(h) ls_plate_at((uint8_t)((h) | TUI_BRIGHT))
#define LS_PLATE_LABEL    ls_plate_at(TUI_WHITE)
#define LS_PLATE_FAINT    LS_ATTR_FAINT

/* A heavier ASCII frame around r: '=' top and bottom, '|' sides, '+'
   corners, in the hue. */
static inline void ls_plate_frame(tui_surface *sf, tui_rect clip, tui_rect r, uint8_t hue)
{
    if (r.w < 2 || r.h < 2) return;
    const uint8_t e = TUI_ATTR(hue, TUI_BLACK), c = TUI_ATTR(hue | TUI_BRIGHT, TUI_BLACK);
    const int x1 = r.x + r.w - 1, y1 = r.y + r.h - 1;
    for (int x = r.x + 1; x < x1; x++) {
        tui_put_char(sf, clip, x, r.y, '=', e);
        tui_put_char(sf, clip, x, y1, '=', e);
    }
    for (int y = r.y + 1; y < y1; y++) {
        tui_put_char(sf, clip, r.x, y, '|', e);
        tui_put_char(sf, clip, x1, y, '|', e);
    }
    tui_put_char(sf, clip, r.x, r.y, '+', c);
    tui_put_char(sf, clip, x1, r.y, '+', c);
    tui_put_char(sf, clip, r.x, y1, '+', c);
    tui_put_char(sf, clip, x1, y1, '+', c);
}

/* A caption tab: " TEXT " in black on the hue, as ls_panel_box's. Returns its width. */
static inline int ls_plate_tab(tui_surface *sf, tui_rect clip, int x, int y, const char *text, uint8_t hue)
{
    char t[48];
    snprintf(t, sizeof(t), " %s ", text ? text : "");
    tui_put_str(sf, clip, x, y, t, TUI_ATTR(TUI_BLACK, hue | TUI_BRIGHT));
    return (int)strlen(t);
}

/* The whole plate: black inside, the frame, the caption at the left of the
   top edge and an optional chip at its right. Returns the inside, a cell of
   air in from the sides. */
static inline tui_rect ls_plate(tui_surface *sf, tui_rect clip, tui_rect r, uint8_t hue,
                                const char *title, const char *chip, uint8_t chip_hue)
{
    if (r.w < 6 || r.h < 4) return tui_rect_make(r.x, r.y, 0, 0);
    tui_fill(sf, tui_rect_make(r.x + 1, r.y + 1, r.w - 2, r.h - 2), ' ', TUI_ATTR(TUI_WHITE, TUI_BLACK));
    ls_plate_frame(sf, clip, r, hue);
    if (title && *title) ls_plate_tab(sf, clip, r.x + 2, r.y, title, hue);
    if (chip && *chip) {
        const int cw = (int)strlen(chip) + 2;
        if (r.w > cw + 4) ls_plate_tab(sf, clip, r.x + r.w - 2 - cw, r.y, chip, chip_hue);
    }
    return tui_rect_make(r.x + 2, r.y + 1, r.w - 4, r.h - 2);
}

/* An ASCII meter: '=' for the part lit in the bright hue, '.' for the rest
   in the plain hue. f is 0..1. */
static inline void ls_plate_meter(tui_surface *sf, tui_rect clip, int x, int y, int w, float f, uint8_t hue)
{
    if (w <= 0) return;
    if (!(f > 0)) f = 0;
    if (f > 1) f = 1;
    const int lit = (int)(f * (float)w + 0.5f);
    for (int i = 0; i < w; i++)
        tui_put_char(sf, clip, x + i, y, i < lit ? '=' : '.',
                     i < lit ? TUI_ATTR(hue | TUI_BRIGHT, TUI_BLACK) : TUI_ATTR(hue, TUI_BLACK));
}

/* One cell of a braille column graph: halves 0 (nothing), 1 (the lower two
   dot rows), 2 (all four). */
static inline void ls_plate_col(tui_surface *sf, tui_rect clip, int x, int y, int halves, uint8_t at)
{
    if (halves <= 0) return;
    ls_braille_put(sf, clip, x, y, (int16_t)(halves >= 2 ? 0x28FF : 0x28E4), at);
}

/* A key drawn like the raised ASCII keys: '+---+' edges, '|' sides, the
   label on its colour when on, in its colour when off, dull when it cannot
   be used. One row tall: "[ LABEL ]". */
static inline void ls_plate_key(tui_surface *sf, tui_rect clip, tui_rect b, const char *label, uint8_t hue,
                                bool on, bool can)
{
    if (b.w < 3 || b.h < 1) return;
    const uint8_t h = can ? hue : (uint8_t)TUI_BLUE;
    const uint8_t edge = TUI_ATTR(h, TUI_BLACK);
    const uint8_t at = on ? TUI_ATTR(TUI_BLACK, hue | TUI_BRIGHT)
                          : TUI_ATTR(can ? (hue | TUI_BRIGHT) : TUI_BLUE, TUI_BLACK);
    tui_fill(sf, b, ' ', TUI_ATTR(TUI_WHITE, TUI_BLACK));
    const int mid = b.y + (b.h - 1) / 2;
    if (b.h >= 3) {
        for (int x = b.x + 1; x < b.x + b.w - 1; x++) {
            tui_put_char(sf, clip, x, b.y, '-', edge);
            tui_put_char(sf, clip, x, b.y + b.h - 1, '-', edge);
        }
        for (int y = b.y; y < b.y + b.h; y++) {
            const bool corner = y == b.y || y == b.y + b.h - 1;
            tui_put_char(sf, clip, b.x, y, corner ? '+' : '|', edge);
            tui_put_char(sf, clip, b.x + b.w - 1, y, corner ? '+' : '|', edge);
        }
    } else {
        tui_put_char(sf, clip, b.x, mid, '[', edge);
        tui_put_char(sf, clip, b.x + b.w - 1, mid, ']', edge);
    }
    if (on && b.w > 2) tui_fill(sf, tui_rect_make(b.x + 1, mid, b.w - 2, 1), ' ', at);
    const int n = label ? (int)strlen(label) : 0;
    const int lx = b.x + (b.w - n) / 2 > b.x + 1 ? b.x + (b.w - n) / 2 : b.x + 1;
    if (label) tui_put_str(sf, clip, lx, mid, label, at);
}

/* A grid of those keys, `per` to a row, filling r; their rects into out. The
   bottom row's outer keys stay clear of the glass's rounded corners. For
   keys inside a page; an app's main key bar stays ls_btn_bar_*. */
static inline void ls_plate_grid(tui_surface *sf, tui_rect clip, tui_rect r, const ls_btn_t *b, int n, int per,
                                 const uint8_t *hues, tui_rect *out, bool corners)
{
    const int rows = (n + per - 1) / per, kh = r.h / rows;
    for (int i = 0; i < n; i++) {
        const int row = i / per, col = i % per;
        int x0 = r.x + r.w * col / per, x1 = r.x + r.w * (col + 1) / per;
        if (corners && row == rows - 1) { if (col == 0) x0 += 2; if (col == per - 1) x1 -= 2; }
        const int yh = row == rows - 1 ? r.y + r.h - (r.y + row * kh) : kh;
        const tui_rect k = tui_rect_make(x0, r.y + row * kh, x1 - x0 - (col < per - 1 ? 1 : 0), yh);
        char t[32];
        snprintf(t, sizeof(t), "%s%s%s", b[i].label, b[i].value ? " " : "", b[i].value ? b[i].value : "");
        if ((int)strlen(t) > k.w - 4) snprintf(t, sizeof(t), "%s", b[i].label);
        ls_plate_key(sf, clip, k, t, hues ? hues[i] : TUI_WHITE, b[i].on, !b[i].dim);
        if (out) out[i] = k;
    }
}

#ifdef __cplusplus
}
#endif
#endif
