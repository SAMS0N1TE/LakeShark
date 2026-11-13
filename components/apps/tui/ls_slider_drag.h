/* Drag along a slider: a finger that goes down on a bar's travel and moves
   along it sets the value from the column under it, frame by frame, until it
   lifts. Once it has a bar, only the column counts, held to the bar's ends,
   so a thumb that drifts off the row above or below keeps the slider.

   Call once a frame from draw, before the bars are drawn again: the rects
   are the ones the finger went down on. A finger that never leaves the
   column it went down on is left alone, so it still arrives as a tap and the
   tap path sets the value as it always did. Once it moves, every frame it is
   on a new column answers MOVE - apply the value now, save nothing - and the
   frame it lets go answers DONE once, with the last column: write it then
   through the tap's own setter, the one save a drag makes. A finger that
   went down anywhere else never moves a bar, even dragged across one. */
#ifndef LS_SLIDER_DRAG_H
#define LS_SLIDER_DRAG_H

#include <stdbool.h>
#include "ls_tui_touch.h"
#include "tuilib/tui_core.h"
#include "esp_timer.h"

typedef struct {
    bool     held;      /* a finger is down, wherever it went down */
    bool     on;        /* and it went down on one of the bars */
    bool     moved;     /* and has left the column it went down on */
    int      bar;       /* which bar, as an index into the caller's array */
    tui_rect rect;      /* where that bar was when the finger took it */
    int      col;       /* the column it is on, held to the bar */
    int64_t  seen_us;   /* the last frame that saw it on a bar */
    int64_t  lift_us;   /* when the last drag that moved a bar let go */
} ls_slider_drag_t;

enum { LS_SLIDER_IDLE = 0, LS_SLIDER_MOVE, LS_SLIDER_DONE };

/* `bars` may hold empty rects (w or h 0) for controls not drawn or not
   settable; a finger cannot take one of those. */
static inline int ls_slider_drag(ls_slider_drag_t *d, const tui_rect *bars, int n)
{
    int sc, sr, c, r;
    if (!ls_tui_touch_held(&sc, &sr, &c, &r)) {
        const bool done = d->on && d->moved;
        /* The lift is the last frame the finger was seen, as for a list. */
        if (done) d->lift_us = d->seen_us;
        d->held = d->on = d->moved = false;
        return done ? LS_SLIDER_DONE : LS_SLIDER_IDLE;
    }
    if (!d->held) {
        /* Decided once, where it went down: a bar that is drawn under a
           finger already on the glass is not a bar it took. */
        d->held = true;
        d->on = d->moved = false;
        for (int i = 0; i < n; i++)
            if (bars[i].w > 0 && bars[i].h > 0 && tui_rect_contains(bars[i], sc, sr)) {
                d->on = true;
                d->bar = i;
                d->rect = bars[i];
                d->col = sc;
                break;
            }
    }
    if (!d->on) return LS_SLIDER_IDLE;
    d->seen_us = esp_timer_get_time();
    const int last = d->rect.x + d->rect.w - 1;
    if (c < d->rect.x) c = d->rect.x;
    if (c > last) c = last;
    if (c == d->col) return LS_SLIDER_IDLE;
    d->moved = true;
    d->col = c;
    return LS_SLIDER_MOVE;
}

/* True while a finger is carrying a bar: the caller draws the value it set. */
static inline bool ls_slider_drag_moving(const ls_slider_drag_t *d)
{
    return d->on && d->moved;
}

/* True for a tap that is really the lift of a drag: a finger that moved less
   than the tap slop still comes up as a tap on the cell it went down on, and
   that must not put the value back where the drag began. The screen ignores
   it. A drag no frame has seen for a while is not one. */
static inline bool ls_slider_drag_tap_is_lift(const ls_slider_drag_t *d)
{
    const int64_t now = esp_timer_get_time();
    if (d->on && d->moved) return now >= d->seen_us && now - d->seen_us < 500000;
    return d->lift_us && now >= d->lift_us && now - d->lift_us < 250000;
}

/* The screen is going away under a finger that is still on a bar. True when
   the drag had moved it, so the caller saves what it set, as the lift would
   have; either way the drag is over. */
static inline bool ls_slider_drag_drop(ls_slider_drag_t *d)
{
    const bool pending = d->on && d->moved;
    d->held = d->on = d->moved = false;
    return pending;
}

#endif /* LS_SLIDER_DRAG_H */
