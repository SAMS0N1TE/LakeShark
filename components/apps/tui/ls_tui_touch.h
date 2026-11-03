/* Touch for the TUI: completed taps and held fingers for dragging controls. */
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

/* One finished touch: where it went down and came up in panel px, the cell
   it went down on (-1 off the grid), and whether it counted as a tap. */
typedef struct { int x0, y0, x1, y1, col, row; bool tap; } ls_tui_touch_event_t;

/* Copies up to max of the most recent touches, oldest first; returns how many. */
int ls_tui_touch_history(ls_tui_touch_event_t *out, int max);

/* True when a completed tap landed on a cell. Non-blocking; call once a
   frame. A press alone reports nothing - only a press and release on the
   same cell counts. */
bool ls_tui_touch_poll(ls_tui_touch_t *out);

/* True while a finger is down: the cell it went down on and the cell it is
   on now. For a control that follows a drag, which never becomes a tap. */
bool ls_tui_touch_held(int *start_col, int *start_row, int *col, int *row);

/* The finger in logical grid pixels (ls_tui_pixel_to_grid), for controls
   that need more than a cell: where it is, where it went down, and when
   (esp_timer microseconds). False while no finger is down. */
bool ls_tui_touch_px(int *x, int *y, int *x0, int *y0, int64_t *since_us);
/* Desktop input stays in grid coordinates; the app finger is a separate,
   window-relative channel. Never feed the latter back into desktop input. */
bool ls_tui_touch_input_held(int *sc, int *sr, int *c, int *r);
bool ls_tui_touch_input_px(int *x, int *y, int *x0, int *y0, int64_t *since_us);
void ls_tui_touch_client(int col, int row, int gx, int gy, bool down);
void ls_tui_touch_client_clear(void);

/* Pointer cell associated with the wheel currently being dispatched. UI task. */
void ls_tui_pointer_set(int col, int row);
bool ls_tui_pointer_cell(int *col, int *row);
/* Finished touches that were not taps, counted, and the last one in grid
   pixels: start and end. A screen compares the count with the one it saw. */
uint32_t ls_tui_touch_drags(int *x0, int *y0, int *x1, int *y1);

/* A finger from a remote screen's mouse (ls_rterm.c): its cell, and grid pixels:
   held, it is what ls_tui_touch_px and ls_tui_touch_held report while the
   glass has none; let go, it counts as a drag when it moved. Taps still
   go through the tap queue. Any task. */
void ls_tui_touch_remote(int col, int row, int gx, int gy, bool down);

#ifdef __cplusplus
}
#endif

#endif /* LS_TUI_TOUCH_H */
