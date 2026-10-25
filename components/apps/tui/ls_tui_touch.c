/* Touch, expressed as the same events a key produces. */

#include "ls_tui_touch.h"

#include "ls_tui.h"
#include "ls_touch.h"

#include <stdint.h>
#include "esp_attr.h"
#include "esp_timer.h"

static bool s_down;
static int  s_start_col, s_start_row;
static int  s_start_x, s_start_y;       /* where the press landed, in panel px */
static ls_tui_touch_src_fn s_source;

/* A tap is the cell the finger went DOWN on, within a slop - not the cell it happened to come back up on. */

#define TAP_SLOP_PX 32

/* The last position the finger was actually at. */

static int s_last_x, s_last_y;

/* Where the finger is now, while it is down, for controls that follow it. */
static int s_cur_col = -1, s_cur_row = -1;

/* The remote mouse's finger, in grid px: written by the terminal's task,
   read by the UI's. A torn pair is one frame's wobble. */
static volatile bool s_r_down;
static volatile int s_r_x, s_r_y, s_r_x0, s_r_y0;
static volatile int s_r_col, s_r_row, s_r_col0, s_r_row0;
static volatile int64_t s_r_us;
static uint32_t s_drags;
static int s_drag[4];

void ls_tui_touch_remote(int col, int row, int gx, int gy, bool down)
{
    if (down && !s_r_down) {
        s_r_x0 = gx;
        s_r_y0 = gy;
        s_r_col0 = col;
        s_r_row0 = row;
        s_r_us = esp_timer_get_time();
    }
    if (!down && s_r_down && (col != s_r_col0 || row != s_r_row0)) {
        s_drag[0] = s_r_x0; s_drag[1] = s_r_y0; s_drag[2] = gx; s_drag[3] = gy;
        s_drags++;
    }
    s_r_x = gx;
    s_r_y = gy;
    s_r_col = col;
    s_r_row = row;
    s_r_down = down;
}

static bool remote_held(void) { return !s_down && s_r_down; }

bool ls_tui_touch_held(int *start_col, int *start_row, int *col, int *row)
{
    if (remote_held()) {
        if (start_col) *start_col = s_r_col0;
        if (start_row) *start_row = s_r_row0;
        if (col)       *col = s_r_col;
        if (row)       *row = s_r_row;
        return true;
    }
    if (!s_down || s_start_col < 0 || s_cur_col < 0) return false;
    if (start_col) *start_col = s_start_col;
    if (start_row) *start_row = s_start_row;
    if (col)       *col = s_cur_col;
    if (row)       *row = s_cur_row;
    return true;
}

static int64_t s_down_us;

bool ls_tui_touch_px(int *x, int *y, int *x0, int *y0, int64_t *since_us)
{
    if (remote_held()) {
        if (x) *x = s_r_x;
        if (y) *y = s_r_y;
        if (x0) *x0 = s_r_x0;
        if (y0) *y0 = s_r_y0;
        if (since_us) *since_us = s_r_us;
        return true;
    }
    if (!s_down) return false;
    int gx = 0, gy = 0, sx = 0, sy = 0;
    ls_tui_pixel_to_grid(s_last_x, s_last_y, &gx, &gy);
    ls_tui_pixel_to_grid(s_start_x, s_start_y, &sx, &sy);
    if (x) *x = gx;
    if (y) *y = gy;
    if (x0) *x0 = sx;
    if (y0) *y0 = sy;
    if (since_us) *since_us = s_down_us;
    return true;
}

uint32_t ls_tui_touch_drags(int *x0, int *y0, int *x1, int *y1)
{
    if (x0) *x0 = s_drag[0];
    if (y0) *y0 = s_drag[1];
    if (x1) *x1 = s_drag[2];
    if (y1) *y1 = s_drag[3];
    return s_drags;
}

void ls_tui_touch_set_source(ls_tui_touch_src_fn fn) { s_source = fn; }

/* Counters for the diagnostics screen; see the header. */
static uint32_t s_reads, s_taps;
static int      s_last_col = -1, s_last_row = -1;

void ls_tui_touch_stats(uint32_t *reads, uint32_t *taps,
                        int *last_col, int *last_row)
{
    if (reads)    *reads    = s_reads;
    if (taps)     *taps     = s_taps;
    if (last_col) *last_col = s_last_col;
    if (last_row) *last_row = s_last_row;
}

/* The last few touches as they were judged, for 'tui touch'. A key that
   "does not work" is either missed by the grid or refused as a drag, and
   only the press and release positions tell which. */
#define TOUCH_LOG 16
EXT_RAM_BSS_ATTR static ls_tui_touch_event_t s_log[TOUCH_LOG];
static unsigned s_log_n;

static void log_touch(int x, int y, bool tap)
{
    ls_tui_touch_event_t *e = &s_log[s_log_n++ % TOUCH_LOG];
    e->x0 = s_start_x; e->y0 = s_start_y; e->x1 = x; e->y1 = y;
    e->col = s_start_col; e->row = s_start_row; e->tap = tap;
}

int ls_tui_touch_history(ls_tui_touch_event_t *out, int max)
{
    int n = s_log_n < TOUCH_LOG ? (int)s_log_n : TOUCH_LOG;
    if (n > max) n = max;
    for (int i = 0; i < n; i++)               /* oldest first */
        out[i] = s_log[(s_log_n - n + i) % TOUCH_LOG];
    return n;
}

bool ls_tui_touch_poll(ls_tui_touch_t *out)
{
    int x = s_last_x, y = s_last_y;
    bool pressed = false;

    if (s_source) {
        if (!s_source(&x, &y, &pressed)) return false;
    } else {
        uint16_t rx = (uint16_t)x, ry = (uint16_t)y;
        if (!ls_touch_read(&rx, &ry, &pressed)) return false;
        x = rx;
        y = ry;
    }
    s_reads++;
    if (pressed) { s_last_x = x; s_last_y = y; }

    int col = 0, row = 0;
    bool on_grid = ls_tui_pixel_to_cell(x, y, &col, &row);
    if (pressed && on_grid) { s_cur_col = col; s_cur_row = row; }

    if (pressed && !s_down) {
        s_down = true;
        s_start_col = on_grid ? col : -1;
        s_start_row = on_grid ? row : -1;
        s_start_x = x;
        s_start_y = y;
        s_down_us = esp_timer_get_time();
        return false;              /* nothing fires on the way down */
    }
    if (!pressed && s_down) {
        s_down = false;

        const int dx = x > s_start_x ? x - s_start_x : s_start_x - x;
        const int dy = y > s_start_y ? y - s_start_y : s_start_y - y;
        const bool tap = s_start_col >= 0 && dx <= TAP_SLOP_PX && dy <= TAP_SLOP_PX;
        log_touch(x, y, tap);
        if (!tap) {
            ls_tui_pixel_to_grid(s_start_x, s_start_y, &s_drag[0], &s_drag[1]);
            ls_tui_pixel_to_grid(x, y, &s_drag[2], &s_drag[3]);
            s_drags++;
        }
        if (tap) {
            s_taps++;
            s_last_col = s_start_col;
            s_last_row = s_start_row;
            if (out) { out->col = s_start_col; out->row = s_start_row; }
            return true;
        }
    }
    return false;
}
