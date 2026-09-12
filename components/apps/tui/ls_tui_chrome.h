/* Chrome layout arithmetic, separated so it can be tested. */

#ifndef LS_TUI_CHROME_H
#define LS_TUI_CHROME_H

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
