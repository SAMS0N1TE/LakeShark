/* A list that follows the finger (API 1.1: lap_touch, lap_cell_px).

   lap_swipe reports a swipe only once it has ended, so a list moved by swipes
   jumps half a screen per flick and stands still while the finger is down.
   This moves the list with the finger, row for row, and lets a flick carry on
   for a moment after it lifts, the way the board's own lists scroll.

   Pair it with the board's keys from lakeshark_ui.h (lap_ui_keys), which are
   sized for a finger; leave LAP_SWIPE_UP/DOWN unhandled when using this.

       static lap_scroll_t drag;            // one per list, zeroed
       ...in draw(), before the list is laid out:
       moving = lap_scroll_follow(&drag, &scroll, smax, top_row, bottom_row);
*/
#ifndef LAP_SCROLL_H
#define LAP_SCROLL_H
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool held, dragging, inside;
    int16_t x0, y0;          /* where this touch went down */
    int base;                /* the scroll when the drag began */
    int16_t last_y;
    uint32_t last_ms;
    float vel;               /* rows per second, after the finger lifts */
    float frac;
} lap_scroll_t;

/* Call once per draw. A finger that went down on rows top .. bottom-1 and
   moved up or down more than about half a row sets *scroll to where it was
   plus the rows the finger moved (finger up = further down the list),
   clamped to 0 .. smax; a flick keeps going and slows. True while the
   finger is dragging or the list is still moving: on those frames the app
   should not pull its selection back into view, and should treat the touch
   as a scroll, not a tap. */
bool lap_scroll_follow(lap_scroll_t *s, int *scroll, int smax, int top, int bottom);
/* Stop any momentum (a tab change, a cleared list). */
void lap_scroll_stop(lap_scroll_t *s);

#ifdef __cplusplus
}
#endif
#endif
