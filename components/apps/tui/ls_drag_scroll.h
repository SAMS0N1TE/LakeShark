/* Drag to scroll a list: a finger that goes down inside the list and moves
   up or down carries the list with it, a row per row's height in cells.
   Call once a frame from draw, before the rows are laid out. A finger that
   stays on its cell is left alone, so it still arrives as a tap. */
#ifndef LS_DRAG_SCROLL_H
#define LS_DRAG_SCROLL_H

#include <stdbool.h>
#include "ls_tui_touch.h"
#include "tuilib/tui_core.h"
#include "esp_timer.h"

typedef struct {
    bool held;        /* this finger went down inside the list */
    bool moved;       /* and has carried it at least one row */
    int start_first;  /* the first row when it went down */
    int64_t lift_us;  /* when the last drag that moved the list let go */
} ls_drag_scroll_t;

/* Updates *first (clamped to 0..total-visible) while a drag is under way.
   True on the frames the list follows a finger. */
static inline bool ls_drag_scroll(ls_drag_scroll_t *d, tui_rect list, int row_cells,
                                  int *first, int total, int visible)
{
    int sc, sr, c, r;
    if (!ls_tui_touch_held(&sc, &sr, &c, &r)) {
        if (d->held && d->moved) d->lift_us = esp_timer_get_time();
        d->held = d->moved = false;
        return false;
    }
    if (!d->held) {
        if (!tui_rect_contains(list, sc, sr)) return false;
        d->held = true; d->moved = false; d->start_first = *first;
    }
    if (row_cells < 1) row_cells = 1;
    const int rows = (sr - r) / row_cells;
    if (!rows && !d->moved) return false;
    d->moved = true;
    int f = d->start_first + rows;
    const int last = total > visible ? total - visible : 0;
    if (f > last) f = last;
    if (f < 0) f = 0;
    *first = f;
    return true;
}

/* True for a tap that is really the lift of a drag: one landing while the
   list is being carried or just after it let go. The screen ignores it. */
static inline bool ls_drag_scroll_tap_is_lift(const ls_drag_scroll_t *d)
{
    if (d->held) return d->moved;
    return d->lift_us && esp_timer_get_time() - d->lift_us < 250000;
}

#endif /* LS_DRAG_SCROLL_H */
