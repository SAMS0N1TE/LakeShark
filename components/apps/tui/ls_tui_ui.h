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
/* The same with a colour per key (TUI_* hue, 0 for the usual): each key
   filled in its own, bright when on, its darker shade when not. */
void ls_btn_bar_raised_hued(tui_surface *sf, tui_rect bar, const ls_btn_t *btn, int n,
                            int focus, const uint8_t *hues);
/* Keep a single raised row; caller must budget enough width for its labels. */
void ls_btn_bar_raised_row(tui_surface *sf,tui_rect bar,const ls_btn_t *btn,int n,int focus);
/* Tabs in exactly `rows` rows, in a hit slot of the caller's choosing. */
void ls_btn_bar_rows_slot(tui_surface *sf,tui_rect bar,const ls_btn_t *btn,int n,int focus,int slot,int rows);
/* A key grid: raised keys in exactly `rows` rows, each row across the bar's
   whole width (keys are not capped at the bar's usual width), down into the
   panel's rounded corners with the outer keys' sides following the curve.
   `hues` (or NULL) gives each key a colour of its own, as on HOME. */
void ls_btn_bar_grid_slot(tui_surface *sf,tui_rect bar,const ls_btn_t *btn,int n,int focus,int slot,int rows,
                          const uint8_t *hues);
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

/* ------------------------------------------------------------ desk compact */

/* COMPACT: the desk is drawing this app in a window.

   On the desktop the pointer is a mouse and the keyboard is a real one, so
   the thumb-sized keys above are wasted rows. While this is true the shared
   widgets draw their desk forms instead: one-row keys "[ LABEL value ]", a
   popup list for the picker and OPTIONS, a text field where a keypad was.
   Hit-testing is unchanged - every form records the rects it drew.

   Only a board that runs the desk (the Nano's runtime panel) and the host
   bench can ever answer true. Everywhere else it is a constant false, so the
   compact branches fold away and a touch build draws exactly as before. */
#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#endif
#if defined(ESP_PLATFORM) && !(defined(CONFIG_LS_PANEL_RUNTIME) && CONFIG_LS_PANEL_RUNTIME)
#define LS_TUI_HAS_COMPACT 0
static inline bool ls_tui_compact(void) { return false; }
#else
#define LS_TUI_HAS_COMPACT 1
bool ls_tui_compact(void);
#endif
/* The desk turns it on with itself and off again. A no-op without a desk. */
void ls_tui_set_compact(bool on);

/* One desk key, "[ LABEL value ]", on one row: label and value on a cap,
   the shortcut letter picked out when the label contains it. */
int  ls_key1_width(const ls_btn_t *b);
/* Draws at (x, y), clipped to `clip`; answers the columns it took. */
int  ls_key1_draw(tui_surface *sf, tui_rect clip, int x, int y,
                  const ls_btn_t *b, bool focus);
/* Columns a row of these keys takes, one blank between keys. */
int  ls_btn_bar_width(const ls_btn_t *btn, int n);
/* A one-row text field "[162.5500_     ]": sunken ends, text on black. */
void ls_field1_draw(tui_surface *sf, tui_rect clip, int x, int y, int w,
                    const char *text, bool caret, uint8_t ink);
/* A desk dialog's raised frame in box lines, its title on the top rule in
   `hue`, the inside cleared to black. */
void ls_dialog1_frame(tui_surface *sf, tui_rect r, const char *title, uint8_t hue);
/* A one-row slider "LABEL [val] |---====#----| [-][+]" for a level.
   Fills the four rects (down, up, value, bar) the way a VFO card does, so
   the caller's hit-test is the touch one. `frac` is 0..1. */
void ls_slider1_draw(tui_surface *sf, tui_rect r, const char *label,
                     const char *value, float frac, uint8_t hue, bool live,
                     tui_rect *down, tui_rect *up, tui_rect *value_rect,
                     tui_rect *bar);

/* ------------------------------------------------------------------- tiles */

typedef struct {
    const char *name;    /* what the app is called                      */
    const char *sub;     /* one line under it, may be NULL              */
    int         icon;    /* ls_icon_t                                   */
    uint8_t     hue;     /* TUI_* colour the tile is drawn in           */
    bool        live;    /* something is happening in there right now   */
    bool        badge;   /* something new inside: a dot and NEW         */
    uint8_t     more;    /* a folder: how many apps sit behind this one */
    bool        sd;      /* lives on the card: [SD] in the bottom border    */
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

/* COLOUR ROLES.

   A screen says what a coloured thing IS and keeps the colour it has always
   had as the fallback: ls_ink(LS_INK_SELECT, TUI_ATTR(TUI_BLACK, hue)).
   In a desk window the desk fills the roles from the active scheme (Selected
   Items, Button face, Menu, Title bar, the scheme's good/warn/bad hues), so
   the bar is the scheme's; everywhere else the fallback is drawn, unchanged.
   A build with no desk folds ls_ink() to its second argument: no code, no
   RAM, the same cells as before.

   Role attributes are in the app's own colour space (text on TUI_BLACK is
   text on the window's face); the desk translates them with everything else
   when it copies the app into its window. Take the fg or bg of a role with
   TUI_ATTR_FG / TUI_ATTR_BG when a cell mixes two. */
enum {
    LS_INK_SELECT,      /* the selected row, tab, tile or menu line: a band */
    LS_INK_SELECT_DIM,  /* the same in a pane the keyboard is not in */
    LS_INK_KEY,         /* a key cap or a column header: a band (the face itself in a Win95 scheme) */
    LS_INK_KEY_ON,      /* a key that is latched on, a lit caption: a band */
    LS_INK_MENU,        /* a menu or popup face and its words */
    LS_INK_TITLE,       /* a caption band across a panel */
    LS_INK_TEXT,        /* plain words on the ground */
    LS_INK_STRONG,      /* values, headings */
    LS_INK_DIM,         /* greyed, unavailable */
    LS_INK_ACCENT,      /* the scheme's information hue */
    LS_INK_GOOD, LS_INK_WARN, LS_INK_BAD,   /* never alike in any scheme */
    LS_INK_RULE,        /* separators and quiet lines */
    LS_INK_ACCEL,       /* the letter a key answers to */
    LS_INK_CAPTION,     /* a panel's caption: a band (the inactive title bar's colours) */
    LS_INK_COUNT
};
#if LS_TUI_HAS_COMPACT
extern uint8_t ls_ink_tab[LS_INK_COUNT];
extern bool ls_ink_on;
static inline uint8_t ls_ink(int role, uint8_t fallback) { return ls_ink_on ? ls_ink_tab[role] : fallback; }
/* The desk: the roles for the frame about to be drawn; NULL ends it. */
void ls_ink_set(const uint8_t *tab);
/* A scope, waterfall or spectrum keeps its black ground in any scheme: the
   screen names the rect (surface cells) each frame it draws one. */
#define LS_INK_KEEP_MAX 12
typedef struct { uint8_t n; struct { int16_t x, y, w, h; } r[LS_INK_KEEP_MAX]; } ls_ink_keep_t;
void ls_ink_keep(tui_rect r);
ls_ink_keep_t *ls_ink_kept(void);
/* A popup drawn over a scope takes its rect back out: it is the scheme's, not the scope's. */
void ls_ink_unkeep(tui_rect r);
/* A hue on the ground that says a state, by what it says: yellow warns, red
   is bad, green is good. For a colour a screen works out as it draws, or one
   it draws at the quiet weight (kept quiet on a dark face); any
   other hue, and any band, is handed back as it came. Not for cells inside
   an ls_ink_keep rect: those stay on black and keep their literals. */
static inline uint8_t ls_ink_hue(uint8_t lit)
{
    if (!ls_ink_on || (lit >> 4)) return lit;
    const uint8_t h = lit & 7;
    if (h != TUI_YELLOW && h != TUI_RED && h != TUI_GREEN) return lit;
    const uint8_t role = ls_ink_tab[h == TUI_YELLOW ? LS_INK_WARN : h == TUI_RED ? LS_INK_BAD : LS_INK_GOOD];
    /* The scheme's hue is this one, bright, and the screen drew it quiet: a
       dark face, where the quiet weight reads and is meant. */
    if ((role & 7) == h && (role & TUI_BRIGHT) && !(lit & TUI_BRIGHT) && !(role >> 4)) return lit;
    return role;
}
#else
#define ls_ink(role, fallback) ((uint8_t)(fallback))
#define ls_ink_hue(lit) ((uint8_t)(lit))
enum { ls_ink_on = 0 };
#define ls_ink_keep(r) ((void)0)
#define ls_ink_unkeep(r) ((void)0)
#endif

void ls_fill_dither(tui_surface *sf, tui_rect r, int density, uint8_t hue);

/* Lettering inside a dithered field: the word on true black with a space
   either side, so the texture does not crowd the glyphs. Centred in `r`. */
void ls_dither_label(tui_surface *sf, tui_rect r, int row, const char *text,
                     uint8_t attr);

void ls_panel_box(tui_surface *sf, tui_rect r, const char *title, uint8_t hue);

/* The lock page's card: rounded corners and rules of `h` ('-', or '=' for
   the chosen one), clipped to `clip`. No title: the status row is the title. */
void ls_rframe(tui_surface *sf, tui_rect clip, tui_rect r, char h, uint8_t attr);

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
