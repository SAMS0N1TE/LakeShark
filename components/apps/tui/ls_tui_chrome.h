/* Chrome layout arithmetic, separated so it can be tested. */

#ifndef LS_TUI_CHROME_H
#define LS_TUI_CHROME_H

#include <stdbool.h>
#include "tuilib/tui_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The rotate control's width, at the left of the status row. */

/* And then it went. "Having the little R there isn't needed
   anymore. Remove it... it's too small anyway." The sensor turns the screen
   as the board turns and F11 turns it from the keyboard, so the row's one
   turn control was a three-column target doing what the hand already does.
   A clock takes its place: UTC as "HH:MMZ", six columns. */
#define LS_TUI_CLOCK_W 6

#define LS_TUI_HELP_W 3

/* -1 means "no room, do not draw this". */
typedef struct {
    int clock_x;   /* the UTC clock, dropped before help     */
    int help_x;    /* the help control, the row's one control */
    int brand_x;   /* the LAKESHARK wordmark, dropped first  */
    int name_x;    /* the active screen's name               */
    int left_x;    /* caller's left status text              */
    int right_x;   /* caller's right status text             */
    int left_n;    /* chars of the left text to draw, 0 = all */
} ls_tui_status_layout_t;

/* Priority when space runs out: the right status is what changes (keyboard
   present, theme), the screen name says where you are, and the wordmark is
   decoration. So the wordmark goes first and the right status goes last. */
ls_tui_status_layout_t ls_tui_status_layout(int cols, int brand_len,
                                            int name_len, int left_len,
                                            int right_len);

/* The same layout, placed in the span [x0, x0 + width) of the row instead of from column 0. */

ls_tui_status_layout_t ls_tui_status_layout_at(int x0, int width,
                                               int brand_len, int name_len,
                                               int left_len, int right_len);

/* The same fields laid out either side of a camera hole: [x0, g0) left of
   it, [g1, x1) right of it. Clock and [?] stay left. The wordmark goes left
   and the name right when everything fits; otherwise the wordmark is dropped,
   the name moves left and the left text gets the right span, cut at a word. */
ls_tui_status_layout_t ls_tui_status_layout_split(int x0, int g0, int g1,
                                                  int x1, int brand_len,
                                                  int name_len, int left_len,
                                                  int right_len,
                                                  const char *left);

/* Tabs either side of a hole: ntab / 2 in [0, g0), the rest in [g1, cols).
   Neighbours share a border column and each side's tabs are one width; a
   spare column goes to the gap. Writes x and w per tab (w includes both
   borders). False, and nothing written, when a tab would be under 5 wide. */
bool ls_tui_tab_split(int cols, int ntab, int g0, int g1, int *x, int *w);

/* Keep a grid's ink off the cells a hole covers. On a side edge, a border
   line through the hole bends inward around it, one cell clear, and text in
   the notch moves inward out of the way. Anything else under the hole,
   text included, is blanked; pictures are left alone. */
void ls_tui_cutout_runaround(tui_surface *sf, tui_rect hole);

typedef struct {
    const char *tail;      /* the fixed global-key reminder, never NULL */
    int tail_x;            /* -1 when even the short tail does not fit  */
    int hint_end_x;        /* exclusive: hint text must stop before this */
} ls_tui_hint_layout_t;

ls_tui_hint_layout_t ls_tui_hint_layout(int cols);

/* How much of a hint string to draw, in characters. */

int ls_tui_hint_fit(const char *hint, int width);

#ifdef __cplusplus
}
#endif

#endif /* LS_TUI_CHROME_H */
