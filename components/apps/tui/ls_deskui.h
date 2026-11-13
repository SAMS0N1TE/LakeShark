/* Desk widgets: what an app laid out for a desk window is made of.

   On the desk an app has a mouse and a real keyboard, so it can have what a
   desktop program has: a menu bar with drop-downs, a menu under the right
   button, a table with a header to click and a scroll strip, a text field
   with a caret. These are those, drawn in cells in the house style (lines,
   black ground, a hue), each recording where it drew so a click is tested
   against the same rects. None of them owns any state but what the caller
   hands it, and none allocates.

   Only a build that has the desk compiles them (LS_TUI_HAS_COMPACT, see
   ls_tui_ui.h); a touch build carries none of it. First used by FREQS
   (screens/ext/scr_freqs_desk.c). */
#ifndef LS_DESKUI_H
#define LS_DESKUI_H

#include <stdbool.h>
#include <stdint.h>

#include "ls_tui_ui.h"

#if LS_TUI_HAS_COMPACT

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------ the right button -- */

/* The screen that is drawing claims the right button for this frame's
   layout: `fn` gets the cell (in the screen's own area) and answers true
   when it opened a menu of its own. A screen that does not claim, or
   answers false, gets the desk's window menu as before. The claim holds
   for the screen that made it and no other. */
void ls_dui_context_claim(bool (*fn)(int col, int row));
/* The desk asks; false when the current screen has no claim or declined. */
bool ls_dui_context_fire(int col, int row);

/* ------------------------------------------------------------ text field -- */

#define LS_DUI_TEXT 160
typedef struct {
    char text[LS_DUI_TEXT];
    uint8_t len, caret, max;        /* max: characters it may hold */
} ls_dui_edit_t;

void ls_dui_edit_set(ls_dui_edit_t *e, const char *text, int max);
/* Characters, Backspace, Delete, Left, Right, Home, End. True when the key
   was the field's; Enter, Esc, Tab and the rest are the caller's. */
bool ls_dui_edit_key(ls_dui_edit_t *e, ls_tk_t k, char ch);
/* "[text_   ]" in `w` cells; the text scrolls to keep the caret in view.
   `x_hit` (may be NULL) gets the column of the first text cell and the
   index of the character there, for ls_dui_edit_click. */
void ls_dui_edit_draw(tui_surface *sf, tui_rect clip, int x, int y, int w,
                      const ls_dui_edit_t *e, bool focus, uint8_t ink);
/* A click at column `col` of a field drawn at x with width w. */
void ls_dui_edit_click(ls_dui_edit_t *e, int x, int w, int col);

/* ------------------------------------------------------------- clipboard -- */

/* One line of text the desk's programs pass between them: an id copied in
   MESH is there to paste into a field of NOTES. It is the board's own, not
   the PC's; it lasts until the next copy or a restart. */
void ls_dui_clip_set(const char *text);
const char *ls_dui_clip_get(void);          /* "" when nothing was copied */
/* The clipboard typed in at the caret, as much as the field holds. True
   when anything went in. */
bool ls_dui_edit_paste(ls_dui_edit_t *e);

/* ------------------------------------------------------------- one frame -- */

/* A box with its title and, at the right of the top edge, a quieter note
   ("3 of 12"). `lit`: the pane the keyboard is in. Answers the inside. */
static inline tui_rect ls_dui_pane(tui_surface *sf, tui_rect r, const char *title, const char *note, uint8_t hue, bool lit)
{
    if (r.w < 2 || r.h < 2) return tui_rect_make(r.x, r.y, 0, 0);
    ls_panel_box(sf, r, title, lit ? (uint8_t)(hue | TUI_BRIGHT) : hue);
    if (note && *note) {
        int w = 0, t = 0;
        while (note[w]) w++;
        while (title && title[t]) t++;
        const int x = r.x + r.w - w - 2;
        if (x > r.x + t + 4) tui_put_str(sf, r, x, r.y, note, TUI_ATTR(hue, TUI_BLACK));
    }
    return tui_rect_make(r.x + 1, r.y + 1, r.w - 2, r.h - 2);
}

/* ----------------------------------------------------------------- menus -- */

enum { LS_DUI_OFF = 1, LS_DUI_CHECK = 2, LS_DUI_SEP = 4, LS_DUI_HEAD = 8 };
typedef struct {
    const char *label;      /* NULL with LS_DUI_SEP */
    const char *keys;       /* the shortcut, shown at the right; NULL for none */
    int16_t id;             /* what a choice answers, >= 0 */
    uint8_t flags;          /* LS_DUI_OFF greyed, LS_DUI_CHECK ticked, LS_DUI_SEP a rule,
                               LS_DUI_HEAD a caption that cannot be chosen */
    uint8_t hue;            /* 0: the menu's own */
} ls_dui_item_t;

typedef struct {
    const ls_dui_item_t *items;
    int n, sel, first;
    int x, y;               /* where it was asked to open */
    int bar;                /* the menu bar title it hangs from, -1 for none */
    bool open;
    uint8_t hue;
    tui_rect box;           /* where the last draw put it */
} ls_dui_popup_t;

/* What a key or a click did to an open popup. A choice is its id (>= 0). */
enum { LS_DUI_NONE = -1, LS_DUI_CLOSED = -2, LS_DUI_PREV = -3, LS_DUI_NEXT = -4 };

void ls_dui_popup_open(ls_dui_popup_t *p, const ls_dui_item_t *items, int n, int x, int y, int bar, uint8_t hue);
/* Drawn inside `area`, moved and cut to fit; a list too long scrolls. */
void ls_dui_popup_draw(tui_surface *sf, tui_rect area, ls_dui_popup_t *p);
/* Up, Down, Home, End, Enter, Esc, a letter (the next item that starts with
   it); Left and Right answer LS_DUI_PREV / LS_DUI_NEXT for a menu bar. */
int  ls_dui_popup_key(ls_dui_popup_t *p, ls_tk_t k, char ch);
/* A click: a choice, LS_DUI_NONE on a rule or a greyed row, LS_DUI_CLOSED
   outside the box. */
int  ls_dui_popup_tap(ls_dui_popup_t *p, int col, int row);

/* A menu bar on one row: " File  Edit  View ". `open` is the title whose
   menu is down (-1 none). Fills x[0..n] with where each title starts (x[n]
   is the end), for ls_dui_bar_hit and for hanging a popup. */
void ls_dui_bar_draw(tui_surface *sf, tui_rect row, const char *const *titles, int n, int open,
                     uint8_t hue, int16_t *x);
int  ls_dui_bar_hit(const int16_t *x, int n, int col);

/* ----------------------------------------------------------------- table -- */

#define LS_DUI_COLS 12
enum { LS_DUI_RIGHT = 1, LS_DUI_FLEX = 2 };
typedef struct {
    const char *title;
    uint8_t w, min;         /* the width it wants; the least a flexing one takes */
    uint8_t flags;          /* LS_DUI_RIGHT: numbers; LS_DUI_FLEX: takes the slack */
    uint8_t drop;           /* 0 never dropped; else the higher goes first when narrow */
} ls_dui_col_t;

typedef struct {
    tui_rect head, body, strip;         /* the header row, the rows, the scroll strip */
    int16_t x[LS_DUI_COLS];             /* per column id: where it starts, -1 when not shown */
    uint8_t w[LS_DUI_COLS];
    int first, rows;                    /* the window on the list */
} ls_dui_table_t;

/* Lay `n` columns out in `r` (one header row, then the rows, a two-cell
   scroll strip at the right). `shown` is a bit per column id the user wants;
   the ones that do not fit are dropped by their `drop` order. */
void ls_dui_table_layout(ls_dui_table_t *t, tui_rect r, const ls_dui_col_t *cols, int n, uint32_t shown);
/* The header: titles, the sorted one bright with its arrow. */
void ls_dui_table_head(tui_surface *sf, const ls_dui_table_t *t, const ls_dui_col_t *cols, int n,
                       int sort_col, bool desc, uint8_t hue);
/* One cell's text in its column on body row `r`, cut to the column. */
void ls_dui_table_cell(tui_surface *sf, const ls_dui_table_t *t, const ls_dui_col_t *cols, int col,
                       int r, const char *text, uint8_t attr);
/* Keep `sel` inside the window, and the window inside the list. */
void ls_dui_table_follow(ls_dui_table_t *t, int sel, int total);
void ls_dui_table_clamp(ls_dui_table_t *t, int total);
void ls_dui_table_strip(tui_surface *sf, const ls_dui_table_t *t, int total, uint8_t hue);
/* Where a click landed. */
enum { LS_DUI_T_NONE, LS_DUI_T_HEAD, LS_DUI_T_ROW, LS_DUI_T_STRIP };
/* HEAD: *what is the column id. ROW: the list index (may be past the end).
   STRIP: -1 for a page up, +1 for a page down. */
int  ls_dui_table_hit(const ls_dui_table_t *t, int n, int col, int row, int *what);

/* -------------------------------------------------------------- one key -- */

/* "[Label]" on one row, in `hue`; greyed when `off`, lit when `on`. Answers
   the cells it took and fills `hit` (may be NULL). */
int  ls_dui_key(tui_surface *sf, tui_rect clip, int x, int y, const char *label, bool on, bool off,
                uint8_t hue, tui_rect *hit);

/* ------------------------------------------------- a menu, built by rows -- */

/* A popup with the storage for its rows: an app keeps one (in PSRAM) and
   fills it each time a menu comes down, so a label can be made on the spot. */
#define LS_DUI_MENU_ITEMS 40
typedef struct {
    ls_dui_popup_t pop;
    ls_dui_item_t items[LS_DUI_MENU_ITEMS];
    char label[LS_DUI_MENU_ITEMS][44], keys[LS_DUI_MENU_ITEMS][12];
    int n;
} ls_dui_menu_t;

void ls_dui_menu_begin(ls_dui_menu_t *m);
/* A row; NULL when the menu is full. `flags` as ls_dui_item_t's. */
ls_dui_item_t *ls_dui_menu_item(ls_dui_menu_t *m, const char *label, const char *keys, int id, int flags);
void ls_dui_menu_rule(ls_dui_menu_t *m);
void ls_dui_menu_open(ls_dui_menu_t *m, int x, int y, int bar, uint8_t hue);

/* ------------------------------------------------------- keys on a toolbar -- */

/* Where each key of a toolbar was drawn and the command it stands for. */
#define LS_DUI_HITS 28
typedef struct { tui_rect r[LS_DUI_HITS]; int16_t cmd[LS_DUI_HITS]; int n; } ls_dui_hits_t;

/* One key at x on `row`, recorded in `hits` unless `off`. Answers the cells
   to step on by (its width and a gap), 0 when it did not fit. */
int ls_dui_tool(tui_surface *sf, tui_rect row, int x, const char *label, int cmd, bool on, bool off, uint8_t hue,
                ls_dui_hits_t *hits);
/* The command under a click, -1 for none. */
int ls_dui_hits_at(const ls_dui_hits_t *hits, int col, int row);

/* -------------------------------------------------------- selection bits -- */

static inline bool ls_dui_bit(const uint8_t *bits, int i) { return (bits[i >> 3] >> (i & 7)) & 1; }
static inline void ls_dui_bit_set(uint8_t *bits, int i, bool on)
{
    if (on) bits[i >> 3] |= (uint8_t)(1u << (i & 7));
    else bits[i >> 3] &= (uint8_t)~(1u << (i & 7));
}

#ifdef __cplusplus
}
#endif

#endif /* LS_TUI_HAS_COMPACT */
#endif /* LS_DESKUI_H */
