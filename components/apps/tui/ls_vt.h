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

/* What one terminal shows, so a frame sends only the cells that changed.

   A terminal smaller than the grid (a phone: 52 x 70 does not fit) shows
   the part of it at x0, y0, and a bar on the row under that to move it:
   UP and DOWN, LEFT and RIGHT when it is narrower too, and where it is.
   Nothing is ever written outside the terminal's own window: a terminal
   that scrolls on a cursor sent past its bottom, rather than holding it
   there, would otherwise push the picture off the top. */
typedef struct {
    int cols, rows;                 /* the grid */
    ls_vt_cell *shown;            /* cols * rows, owned by the caller */
    bool full;                      /* repaint every cell next time */
    bool clear;                     /* erase the terminal first: what it shows changed shape */
    uint32_t fg, bg;                /* the terminal's colours after the last frame; a full repaint forgets them */
    int term_cols, term_rows;       /* the terminal's window; 0 when it has not said */
    int x0, y0;                     /* the grid cell in the terminal's top left */
    bool quadrants;                 /* sextants out as quadrants, for a font that may not have them */
    uint32_t bar_fg, bar_bg;        /* the bar as last drawn */
} ls_vt_screen;

/* Set up a screen over caller storage; the first render repaints all of it.
   The terminal is taken to be as big as the grid until ls_vt_screen_resize. */
void ls_vt_screen_init(ls_vt_screen *s, int cols, int rows, ls_vt_cell *shown);

/* The terminal's size, when it says (telnet NAWS, or a cursor report).
   True when that changes what it shows; the next render erases and repaints. */
bool ls_vt_screen_resize(ls_vt_screen *s, int term_cols, int term_rows);

/* The grid cells the terminal shows, w x h from x0, y0. Less than the grid
   when it is smaller, and then the bar is on terminal row h. */
void ls_vt_screen_view(const ls_vt_screen *s, int *w, int *h);

/* Move the view by whole cells, kept inside the grid. */
void ls_vt_screen_pan(ls_vt_screen *s, int dcols, int drows);

/* A tap at a terminal cell, 0-based. True with the grid cell under it; a
   tap on the bar moves the view instead, and like one off the grid is false. */
bool ls_vt_screen_tap(ls_vt_screen *s, int col, int row, int *gcol, int *grow);

/* Write the cells that differ from what the terminal shows, inside one
   synchronized update. Returns the bytes written. */
size_t ls_vt_render(ls_vt_screen *s, const ls_vt_cell *cells, ls_vt_out_fn out, void *ctx);

/* The terminal set up for the UI: alternate screen, no cursor, no wrap,
   mouse reporting in SGR form, a title. Leave undoes all of it. */
size_t ls_vt_enter(ls_vt_out_fn out, void *ctx, int cols, int rows);
size_t ls_vt_leave(ls_vt_out_fn out, void *ctx);

/* Ask the terminal its size: the cursor as far right and down as it goes,
   then where it is. Set ls_vt_input.want_size so the answer is an
   LS_VT_EV_SIZE and not a key. */
size_t ls_vt_ask_size(ls_vt_out_fn out, void *ctx);

/* Telnet's option talk taken out of what a client sends, keeping the window
   size it gives in a NAWS subnegotiation (RFC 1073). */
typedef struct {
    int state;
    uint8_t sb[8];
    int sb_n;
    int cols, rows;                 /* the client's window; 0 until it says */
    bool sized;                     /* a NAWS size has come */
} ls_vt_telnet;

/* IAC DO NAWS: the server asking a client for its window size. */
#define LS_VT_TELNET_DO_NAWS "\xff\xfd\x1f"

/* Strip in place; returns the bytes left. A sequence cut between two reads
   carries on in the next. */
size_t ls_vt_telnet_strip(ls_vt_telnet *t, uint8_t *b, size_t n);

/* --- input ------------------------------------------------------------ */

/* A left press is LS_VT_EV_TAP. A terminal in button-event mode (?1002)
   then sends LS_VT_EV_DRAG while the button moves held and LS_VT_EV_UP when
   it lets go, so a client that sends those can steer a drag, not just tap. */
enum { LS_VT_EV_KEY = 1, LS_VT_EV_TAP, LS_VT_EV_QUIT, LS_VT_EV_REDRAW, LS_VT_EV_SIZE,
       LS_VT_EV_DRAG, LS_VT_EV_UP };

typedef struct {
    int type;
    int key;                        /* ls_tk_t for LS_VT_EV_KEY */
    char ch;                        /* the character for LS_TK_CHAR */
    bool wheel;                     /* the key is the mouse wheel's */
    int col, row;                   /* LS_VT_EV_TAP, 0-based cells; LS_VT_EV_SIZE, the size */
} ls_vt_event;

typedef struct {
    uint8_t buf[32];
    int n;
    uint8_t last;                   /* the byte before, so CR LF is one Enter */
    bool want_size;                 /* a cursor report is due (ls_vt_ask_size), not F3 */
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
