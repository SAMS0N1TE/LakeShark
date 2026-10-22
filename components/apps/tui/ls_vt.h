/* The TUI grid as terminal text, and terminal input as TUI keys and taps.

   Pure C with no device dependencies: the board serves its screen with it
   (ls_rterm.c, over the console UART or TCP) and the simulator runs the whole
   UI in a desktop terminal with it (lssim --term). Both ends therefore draw
   the same cells the same way.

   Every cell becomes one terminal cell with a 24-bit foreground and
   background. The TUI's own glyphs map to Unicode block elements: quadrants
   to U+2596..259F, sextants to U+1FB00..1FB3B (Cascadia Mono has them; pass
   quadrants_only for a font without), shades to a blended background, image
   cells (the map picture, waterfalls) to upper half blocks coloured from the
   picture, two pixels a cell. */

#ifndef LS_VT_H
#define LS_VT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tuilib/tui_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One terminal cell as drawn: codepoint and 0xRRGGBB colours. */
typedef struct {
    uint32_t cp;
    uint32_t fg, bg;
} ls_vt_cell;

/* Everything one frame needs, copied out of the TUI by the caller. */
typedef struct {
    int cols, rows;
    const tui_cell *cells;          /* cols * rows */
    const uint16_t *palette;        /* 16 RGB565, indexed by the attribute nibbles */
    const uint8_t *glass;           /* one bit a cell, or NULL */
    tui_rect image;                 /* cells the picture covers; w == 0 for none */
    const uint32_t *img_top;        /* cols * rows, 0xRRGGBB of each cell's upper half */
    const uint32_t *img_bot;        /* and lower half; valid inside `image` only */
} ls_vt_frame;

/* Resolve a frame to terminal cells (cols * rows of them). */
void ls_vt_resolve(const ls_vt_frame *f, bool quadrants_only, ls_vt_cell *out);

/* Average the picture behind each image cell into img_top / img_bot.
   `src` is w x h RGB565 drawn into `cells`; a ring picture (a waterfall)
   starts at column x0, row y0 of src. Cells outside the rect are not touched. */
void ls_vt_sample_image(tui_rect cells, int cols, const uint16_t *src, int w, int h,
                          int x0, int y0, uint32_t *img_top, uint32_t *img_bot);

typedef void (*ls_vt_out_fn)(void *ctx, const char *bytes, size_t n);

/* What one terminal shows, so a frame sends only the cells that changed. */
typedef struct {
    int cols, rows;
    ls_vt_cell *shown;            /* cols * rows, owned by the caller */
    bool full;                      /* repaint every cell next time */
    uint32_t fg, bg;                /* the terminal's colours after the last frame; a full repaint forgets them */
} ls_vt_screen;

/* Set up a screen over caller storage; the first render repaints all of it. */
void ls_vt_screen_init(ls_vt_screen *s, int cols, int rows, ls_vt_cell *shown);

/* Write the cells that differ from what the terminal shows, inside one
   synchronized update. Returns the bytes written. */
size_t ls_vt_render(ls_vt_screen *s, const ls_vt_cell *cells, ls_vt_out_fn out, void *ctx);

/* The terminal set up for the UI: alternate screen, no cursor, no wrap,
   mouse reporting in SGR form, a title. Leave undoes all of it. */
size_t ls_vt_enter(ls_vt_out_fn out, void *ctx, int cols, int rows);
size_t ls_vt_leave(ls_vt_out_fn out, void *ctx);

/* --- input ------------------------------------------------------------ */

enum { LS_VT_EV_KEY = 1, LS_VT_EV_TAP, LS_VT_EV_QUIT, LS_VT_EV_REDRAW };

typedef struct {
    int type;
    int key;                        /* ls_tk_t for LS_VT_EV_KEY */
    char ch;                        /* the character for LS_TK_CHAR */
    int col, row;                   /* LS_VT_EV_TAP, 0-based cells */
} ls_vt_event;

typedef struct {
    uint8_t buf[32];
    int n;
    uint8_t last;                   /* the byte before, so CR LF is one Enter */
} ls_vt_input;

/* Feed raw terminal bytes; up to `max` events come out. An escape sequence
   cut between two reads is kept for the next call. Ctrl-] quits, Ctrl-L
   asks for a full repaint. A mouse press is a tap, the wheel is up/down. */
int ls_vt_input_feed(ls_vt_input *in, const uint8_t *bytes, size_t n,
                       ls_vt_event *ev, int max);

/* After a quiet spell: a lone ESC still waiting is the Escape key. */
int ls_vt_input_idle(ls_vt_input *in, ls_vt_event *ev, int max);

#ifdef __cplusplus
}
#endif

#endif
