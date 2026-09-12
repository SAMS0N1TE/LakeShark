/* Screen registry and router for the TUI. */

#ifndef LS_TUI_SCREEN_H
#define LS_TUI_SCREEN_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "ls_tui.h"

/* Shared by the app directory and router so every registered app fits. */
#define LS_TUI_MAX_SCREENS 16

/* Keys as the screens see them. Characters arrive as themselves; everything
   else is one of these. Deliberately the same vocabulary as tui_key_t in
   app_registry.h, which was designed for this and never wired up. */
typedef enum {
    LS_TK_NONE = 0,
    LS_TK_CHAR,          /* the character is in the `ch` argument */
    LS_TK_UP, LS_TK_DOWN, LS_TK_LEFT, LS_TK_RIGHT,
    LS_TK_ENTER, LS_TK_ESC, LS_TK_TAB, LS_TK_BACKSPACE,
    LS_TK_F1, LS_TK_F2, LS_TK_F3, LS_TK_F4, LS_TK_F5,
    LS_TK_F6, LS_TK_F7, LS_TK_F8, LS_TK_F9, LS_TK_F10, LS_TK_F11,
    /* Keys the T-Deck has and nothing was listening to. */

    LS_TK_ALT, LS_TK_CTRL, LS_TK_FN, LS_TK_META, LS_TK_MIC,

    LS_TK_MIC_HOLD,
} ls_tk_t;

typedef struct ls_tui_screen_s {
    const char *name;                 /* short: shown in the tab strip */
    const char *hint;                 /* key hints for the bottom bar */
    void (*enter)(void);              /* optional */
    void (*leave)(void);              /* optional */
    void (*draw)(tui_surface *sf, tui_rect area);
    /* Return true when the key was consumed. Anything not consumed falls
       through to the router, which owns screen switching and global keys, so
       a screen never has to know about its neighbours. */
    bool (*key)(ls_tk_t key, char ch);
    /* A tap, in cells, already resolved to the screen's own area.
       Optional: a screen that leaves this NULL still gets the router's
       fallback, which turns a tap into UP, DOWN or ENTER by where it landed.
       That fallback is enough to drive a list and hopeless for anything with
       controls in it, which is why every screen with buttons implements this
       and hit-tests the same rects it drew. */
    bool (*touch)(int col, int row);

    /* Which receiver this screen needs running, by the name the firmware's mode table uses: "P25", "ADS-B", "FM", "REC". */

    const char *radio;
} ls_tui_screen_t;

/* Registration is by pointer and the descriptor must outlive the program -
   these are static tables, not allocations. Returns the index, or -1 if full. */
int  ls_tui_screen_register(const ls_tui_screen_t *screen);
int  ls_tui_screen_count(void);
int  ls_tui_screen_current(void);
/* Short name of a registered screen, or NULL when the index is out of
   range. Exists so the console can say which screen an injected key
   reached - driving this board blind over serial is the normal case. */
const char *ls_tui_screen_name(int index);
void ls_tui_screen_show(int index);

/* "Run this receiver", or NULL for "nothing here needs one". */

void ls_tui_radio_want(const char *mode_name);

/* The mode currently claimed, or NULL when nothing is. The same
   strings a screen puts in its `radio` field, so a directory lamp and a
   screen's claim cannot disagree about which app owns the receiver. Stubbed
   on the host bench alongside ls_tui_radio_want. */
const char *ls_tui_radio_claimed(void);
/* A dispatcher returns true when it queued the switch for the UI task. */
void ls_tui_screen_set_dispatch_cb(bool (*cb)(int index));
void ls_tui_screen_next(void);
void ls_tui_screen_prev(void);

/* Draw one frame: chrome, then the active screen into what is left. Does not
   present; the caller decides when, because it owns the frame cadence. */
void ls_tui_router_draw(tui_surface *sf);

/* Feed a key. The active screen sees it first; the router handles what is
   left over. Returns true when anything consumed it. */
bool ls_tui_router_key(ls_tk_t key, char ch);

/* A tap, resolved to a cell. Returns true when anything consumed it.

   The router owns what a cell means before a screen does, because the chrome
   is the router's: row 1 is the tab strip, so a tap there switches app, and
   the last row is the hint bar, so a tap there opens the key list. That is
   what makes touch and the keyboard teach each other - the tab strip shows
   the number that is also the function key, and both do the same thing. */
bool ls_tui_router_touch(int col, int row);

/* How many of the registered screens are on the tab strip. */

void ls_tui_screen_set_tab_count(int n);

/* Name the strip's destinations outright. */

void ls_tui_screen_set_tabs(const int *indices, int n);

/* Where a registered screen sits in the register, or -1. The tab list is
   written in screen indices and everything else names apps; this is how a
   caller crosses between them without counting. */
int ls_tui_screen_index_of(const ls_tui_screen_t *screen);

/* Split a rect into two, along whichever axis has room. */

void ls_tui_split(tui_rect area, tui_rect *first, tui_rect *second);

void ls_tui_split_at(tui_rect area, int want, tui_rect *first,
                     tui_rect *second);

/* True when the grid is wider than it is tall. A screen should rarely need
   this; if it does, prefer ls_tui_split. */
bool ls_tui_is_wide(void);

/* Turning the screen is the router's to ask for and somebody else's to do:
   the rotation lives in the display layer, which this component deliberately
   knows nothing about. F11 lands here; took the status-row tap out. */
void ls_tui_screen_set_rotate_cb(void (*cb)(void));

/*"Stop the session and start it again."

   The grid is settled at ls_tui_begin and nothing may change it while a
   session is running, so a setting that changes the grid - the font - cannot
   apply itself. It asks, the same way the rotate control does, and the owner
   of the session does the stop and the start. NULL, and the setting is not
   offered. */
/* Rotations so far, and what the last rebuild cost in milliseconds.
   Whoever owns the session calls ls_tui_rebuild_note when one finishes; a
   build with no such session never calls and the count stays zero. */
void     ls_tui_rebuild_note(uint32_t ms);
uint32_t ls_tui_rebuild_count(void);
uint32_t ls_tui_rebuild_ms(void);

/* Called once whenever a tap was CONSUMED by something. Installed by
   whoever assembles the interface; a build with no way to answer a tap
   installs nothing and the router behaves exactly as before. */
void ls_tui_set_tap_cb(void (*cb)(void));

void ls_tui_screen_set_regrid_cb(void (*cb)(void));
void ls_tui_screen_request_regrid(void);
bool ls_tui_screen_can_regrid(void);

/* Status line content the router paints along the top. Set from wherever the
   truth lives; the router does not go looking for it. */
void ls_tui_status_set(const char *left, const char *right);

/* The clock the status row shows where the turn control was. The
   router draws the text it is given and knows nothing about time: the caller
   decides what the time is and whether it is known. At most LS_TUI_CLOCK_W
   characters; an empty string draws nothing. */
void ls_tui_status_set_clock(const char *text);

#ifdef __cplusplus
}
#endif

#endif /* LS_TUI_SCREEN_H */
