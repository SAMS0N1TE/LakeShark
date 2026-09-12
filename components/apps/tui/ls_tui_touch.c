/* Touch, expressed as the same events a key produces. */

#include "ls_tui_touch.h"

#include "ls_tui.h"
#include "ls_touch.h"

#include <stdint.h>

static bool s_down;
static int  s_start_col, s_start_row;
static int  s_start_x, s_start_y;       /* where the press landed, in panel px */
static ls_tui_touch_src_fn s_source;

/* A tap is the cell the finger went DOWN on, within a slop - not the cell it happened to come back up on. */

#define TAP_SLOP_PX 32

/* The last position the finger was actually at. */

static int s_last_x, s_last_y;

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

    if (pressed && !s_down) {
        s_down = true;
        s_start_col = on_grid ? col : -1;
        s_start_row = on_grid ? row : -1;
        s_start_x = x;
        s_start_y = y;
        return false;              /* nothing fires on the way down */
    }
    if (!pressed && s_down) {
        s_down = false;

        const int dx = x > s_start_x ? x - s_start_x : s_start_x - x;
        const int dy = y > s_start_y ? y - s_start_y : s_start_y - y;
        if (s_start_col >= 0 && dx <= TAP_SLOP_PX && dy <= TAP_SLOP_PX) {
            s_taps++;
            s_last_col = s_start_col;
            s_last_row = s_start_row;
            if (out) { out->col = s_start_col; out->row = s_start_row; }
            return true;
        }
    }
    return false;
}
