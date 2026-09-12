/* Touch for the TUI. See ls_tui_touch.c for why a drag deliberately does
   nothing. */
#ifndef LS_TUI_TOUCH_H
#define LS_TUI_TOUCH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { int col, row; } ls_tui_touch_t;

/* Where the samples come from, because there must be exactly one reader of the controller. */

typedef bool (*ls_tui_touch_src_fn)(int *x, int *y, bool *pressed);
void ls_tui_touch_set_source(ls_tui_touch_src_fn fn);

void ls_tui_touch_stats(uint32_t *reads, uint32_t *taps,
                        int *last_col, int *last_row);

/* True when a completed tap landed on a cell. Non-blocking; call once a
   frame. A press alone reports nothing - only a press and release on the
   same cell counts. */
bool ls_tui_touch_poll(ls_tui_touch_t *out);

#ifdef __cplusplus
}
#endif

#endif /* LS_TUI_TOUCH_H */
