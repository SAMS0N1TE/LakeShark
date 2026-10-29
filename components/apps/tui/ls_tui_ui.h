/* Big touch targets, drawn as cells. */

#ifndef LS_TUI_UI_H
#define LS_TUI_UI_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "ls_tui.h"
#include "ls_tui_screen.h"

/* ------------------------------------------------------------------ button */

typedef struct {
    const char *label;   /* short and upper case; it has to fit a thumb */
    const char *value;   /* current setting, drawn under the label      */
    char        key;     /* keyboard shortcut, 0 for none               */
    bool        on;      /* draw as engaged                             */
    bool        dim;     /* unavailable right now, still shown          */
    bool        badge;   /* something new inside: a dot on the corner   */
} ls_btn_t;

/* Two hit slots, because a screen can carry two bars.

   A screen with page buttons that also embeds the waterfall draws two bars in
   one frame, and a single hit table means the second one silently steals the
   first one's taps. Slot 0 is the screen's own; slot 1 belongs to the
   waterfall. Two is not a general mechanism and is not meant to become one -
   a third bar on one screen is a sign the screen wants splitting. */
#define LS_BTN_SLOT_SCREEN     0
#define LS_BTN_SLOT_WATERFALL  1

#define LS_BTN_SLOT_QUICK      2

/* Fill `bar` with n buttons, wrapping onto as many rows as its height allows.
   `focus` is the keyboard-focused index, -1 for none. */
void ls_btn_bar(tui_surface *sf, tui_rect bar, const ls_btn_t *btn, int n,
                int focus);
void ls_btn_bar_slot(tui_surface *sf, tui_rect bar, const ls_btn_t *btn, int n,
                     int focus, int slot);
/* Taller controls with shortcut badges; height adapts to the available pane. */
/* Whether a three-row bar of these buttons can put each label and its value
   on one line, or whether asking for three rows would cut a word in half.
   A landscape pane wants the short bar; a half-width one cannot have it. */
bool ls_btn_compact_fits(tui_rect bar, const ls_btn_t *btn, int n);

int ls_btn_raised_height(tui_rect area, int n);
/* Stretch each row to the usable width, insetting only its own corners. */
void ls_btn_bar_transport(tui_surface *sf,tui_rect bar,const ls_btn_t *btn,int n,int focus);
/* The same in a hit slot of the caller's choosing: one wide key over a bar. */
void ls_btn_bar_transport_slot(tui_surface *sf,tui_rect bar,const ls_btn_t *btn,int n,int focus,int slot);
void ls_btn_bar_raised(tui_surface *sf, tui_rect bar, const ls_btn_t *btn, int n,
                       int focus);
/* Keep a single raised row; caller must budget enough width for its labels. */
void ls_btn_bar_raised_row(tui_surface *sf,tui_rect bar,const ls_btn_t *btn,int n,int focus);
/* Tabs in exactly `rows` rows, in a hit slot of the caller's choosing. */
void ls_btn_bar_rows_slot(tui_surface *sf,tui_rect bar,const ls_btn_t *btn,int n,int focus,int slot,int rows);
void ls_btn_bar_raised_slot(tui_surface *sf, tui_rect bar, const ls_btn_t *btn,
                            int n, int focus, int slot);
/* Clip and inset one line against the physical corner at its absolute row. */
void ls_safe_line(tui_surface *sf, tui_rect area, int row,
                  const char *text, uint8_t attr);

/* Which button a tap landed on, or -1. Valid until that slot is drawn again. */
int  ls_btn_hit(int col, int row);
/* Navigate the last drawn controls; optionally include the second bar. */
bool ls_btn_navigate(ls_tk_t key, int *slot, int *focus, bool two_bars);
bool ls_btn_enabled(int slot, int focus);
int  ls_btn_hit_slot(int col, int row, int slot);

/* How many buttons the last draw of `slot` recorded, and where the i'th one
   landed. The hit table already knows both; this reads it out so a test can
   check what was actually drawn instead of a screenshot. Returns false when
   the index is out of range. Any of the out pointers may be NULL. */
int  ls_btn_count_slot(int slot);
bool ls_btn_rect_slot(int slot, int i, int *x, int *y, int *w, int *h);
/* Resolve the shortcuts actually displayed in a bar, including availability. */
int ls_btn_shortcut(char ch, int slot);
void ls_btn_clear_hits(void);

/* Which button a character matches, or -1. Case-insensitive. */
int  ls_btn_key(char ch, const ls_btn_t *btn, int n);

/* ------------------------------------------------------------------- tiles */

typedef struct {
    const char *name;    /* what the app is called                      */
    const char *sub;     /* one line under it, may be NULL              */
    int         icon;    /* ls_icon_t                                   */
    uint8_t     hue;     /* TUI_* colour the tile is drawn in           */
    bool        live;    /* something is happening in there right now   */
    bool        badge;   /* something new inside: a dot and NEW         */
    uint8_t     more;    /* a folder: how many apps sit behind this one */
} ls_tile_t;

/* A grid of large tiles filling `area`. Column count follows the width, so
   portrait gets two fat tiles across and landscape four or five - the caller
   does not choose and cannot get it wrong. */
void ls_tile_grid(tui_surface *sf, tui_rect area, const ls_tile_t *tile,
                  int n, int sel);

/* Which tile a tap landed on, or -1. Valid until the next ls_tile_grid. */
int  ls_tile_hit(int col, int row);

/* Grid shape the last ls_tile_grid used, for arrow-key movement. */
void ls_tile_shape(int *cols, int *rows);

/* --------------------------------------------------------------- fragments */

/* A framed panel with a title in the border, in one colour family. Same job
   as tui_box, one call shorter, and it tints the title so a screen full of
   panels has a readable hierarchy instead of eight identical rectangles. */

#define LS_DITHER_LIGHT  0   /* a quarter - a field you may press          */
#define LS_DITHER_MEDIUM 1   /* a half    - a selected row, a lit tile     */
#define LS_DITHER_HEAVY  2   /* three quarters - as near solid as it goes  */

/* TWO GREYS, AND ONLY ONE OF THEM MAY CARRY WORDS. */

#define LS_DIM_FG    TUI_WHITE
#define LS_FAINT_FG  (TUI_BLACK | TUI_BRIGHT)
#define LS_ATTR_DIM    TUI_ATTR(LS_DIM_FG, TUI_BLACK)
#define LS_ATTR_FAINT  TUI_ATTR(LS_FAINT_FG, TUI_BLACK)

void ls_fill_dither(tui_surface *sf, tui_rect r, int density, uint8_t hue);

/* Lettering inside a dithered field: the word on true black with a space
   either side, so the texture does not crowd the glyphs. Centred in `r`. */
void ls_dither_label(tui_surface *sf, tui_rect r, int row, const char *text,
                     uint8_t attr);

void ls_panel_box(tui_surface *sf, tui_rect r, const char *title, uint8_t hue);

void ls_panel_notice(tui_surface *sf, tui_rect area, const char *title,
                     const char *reason, const char *remedy);

/* label on the left, value on the right, one row, aligned to the panel. */
void ls_kv(tui_surface *sf, tui_rect r, int row, const char *label,
           const char *value, uint8_t vattr);

/* A horizontal bar meter, 0..1, coloured green through red. Height 1. */
void ls_bar(tui_surface *sf, tui_rect r, int row, int x, int w, float v);

/* One-row ASCII meter, absolute coordinates, quarter ticks and 0..1 fill.
   Caller supplies the physical scale/value beside it. No storage or I/O. */
void ls_meter_ascii(tui_surface *sf, tui_rect clip, int x, int y, int w,
                    float fraction, uint8_t hue);

#ifdef __cplusplus
}
#endif

#endif /* LS_TUI_UI_H */
