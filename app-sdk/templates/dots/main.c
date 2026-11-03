/* @NAME@: @SUB@.
 *
 * Made by `lsdk new` from the dots template: a picture in dots, the way
 * COMPASS and DICE draw (include/lap_canvas.h). A lit ball rolls on a tray
 * as the board tilts; flick a finger to throw it. It moves only when the
 * board or a finger moves it, and comes to rest when nothing does.
 *
 * Keys: c puts the ball back in the middle, o opens OPTIONS.
 * In `lsdk sim` and Studio, the TILT and SWIPE buttons play the sensor. */
#include "lakeshark_app.h"
#include "lakeshark_ui.h"
#include "lap_canvas.h"
#include <stdio.h>
#include <string.h>

LAP_ICON_PX("....................",
            "....................",
            "..3333333333333333..",
            "..3..............3..",
            "..3.....ffe......3..",
            "..3....feeec.....3..",
            "..3....eeccc.....3..",
            "..3.....cc4......3..",
            "..3..............3..",
            "..3..............3..",
            "..3333333333333333..",
            "....................",
            "....................",
            "....................",
            "....................",
            "....................",
            "....................",
            "....................");

static struct {
    lap_canvas_t cv;
    float x, y, vx, vy;         /* the ball, in dots and dots a frame */
    int ramp;                   /* OPTIONS: which inks it is lit in */
    bool rolling;
    bool no_motion;             /* this board has no motion sensor (a Nano): swipe instead */
    lap_btn_t keys[2];
} s;

static const uint8_t *const RAMPS[] = { LC_RAMP_SEA, LC_RAMP_FIRE, LC_RAMP_GREEN, LC_RAMP_METAL };
static const char *const RAMP_NAMES[] = { "SEA", "FIRE", "GREEN", "METAL" };
static double opt_get(int id) { (void)id; return s.ramp; }
static void opt_set(int id, double v) { (void)id; s.ramp = (int)v & 3; }
static const lap_opt_t OPT_ROWS[] = {
    { .label = "BALL", .kind = LAP_OPT_CYCLE, .id = 0, .names = RAMP_NAMES, .n = 4 },
};
static const lap_opt_list_t OPTIONS = { "@NAME@", OPT_ROWS, 1, opt_get, opt_set };

static void centre(void)
{
    s.x = s.cv.w / 2.0f;
    s.y = s.cv.h / 2.0f;
    s.vx = 3.5f;                /* thrown in, so it moves the moment the app opens */
    s.vy = -2.5f;
}

/* Gravity along the glass from the motion sensor, a flick from a swipe,
   friction, and a bounce off the tray's walls. */
static void physics(int r)
{
    lap_motion_t m;
    if (lap_motion(&m)) {
        s.vx += -m.ax * 0.35f;
        s.vy += m.ay * 0.35f;
    } else {
        s.no_motion = true;
    }
    switch (lap_swipe()) {
    case LAP_SWIPE_LEFT: s.vx -= 6; break;
    case LAP_SWIPE_RIGHT: s.vx += 6; break;
    case LAP_SWIPE_UP: s.vy -= 6; break;
    case LAP_SWIPE_DOWN: s.vy += 6; break;
    default: break;
    }
    s.vx *= 0.97f;
    s.vy *= 0.97f;
    s.x += s.vx;
    s.y += s.vy;
    const float lo = 3 + r, hx = s.cv.w - 4 - r, hy = s.cv.h - 4 - r;
    if (s.x < lo) { s.x = lo; s.vx = -s.vx * 0.6f; }
    if (s.x > hx) { s.x = hx; s.vx = -s.vx * 0.6f; }
    if (s.y < lo) { s.y = lo; s.vy = -s.vy * 0.6f; }
    if (s.y > hy) { s.y = hy; s.vy = -s.vy * 0.6f; }
    s.rolling = s.vx * s.vx + s.vy * s.vy > 0.01f;
}

static void draw(void)
{
    const int w = lap_cols(), h = lap_rows();
    const int kh = lap_ui_keys_rows(2), ky = h - kh;
    lap_clear(LAP_UI_DIM);
    lap_put_str(1, 0, "@NAME@", LAP_ATTR(LAP_YELLOW | LAP_BRIGHT, LAP_BLACK));
    lap_put_str(2 + (int)strlen("@NAME@"), 0, s.rolling ? "rolling" : "at rest", s.rolling ? LAP_ATTR(LAP_UI_LIVE, LAP_BLACK) : LAP_UI_DIM);
    if (!lap_canvas_init(&s.cv, w, ky - 1)) return;
    if (s.x == 0 && s.y == 0) centre();
    const int r = s.cv.w / 10;
    physics(r);

    /* the tray: a felt floor dithered dark to light, a rim, the ball's shadow, the ball */
    for (int y = 0; y < s.cv.h; y++)
        for (int x = 0; x < s.cv.w; x++)
            lc_shade(&s.cv, x, y, 0.15f + 0.25f * y / s.cv.h, LC_RAMP_GREEN);
    lc_rect(&s.cv, 0, 0, s.cv.w, s.cv.h, LC_YELLOW);
    lc_rect(&s.cv, 1, 1, s.cv.w - 2, s.cv.h - 2, LC_RED);
    lc_fill_circle(&s.cv, (int)s.x + r / 3, (int)s.y + r / 2, r, LC_BLACK);
    lc_ball(&s.cv, (int)s.x, (int)s.y, r, RAMPS[s.ramp]);
    lap_canvas_show(&s.cv, 0, 1);
    if (s.no_motion)
        lap_put_glass(2, ky - 2, "no motion sensor here: swipe to throw", LAP_ATTR(LAP_YELLOW | LAP_BRIGHT, LAP_BLACK));
    else if (!s.rolling)
        lap_put_glass(2, ky - 2, "tilt the board, or swipe", LAP_ATTR(LAP_WHITE | LAP_BRIGHT, LAP_BLACK));

    s.keys[0] = (lap_btn_t){ "CENTRE", NULL, 'c', false, false, false };
    s.keys[1] = (lap_btn_t)LAP_BTN_OPTIONS;
    lap_ui_keys(0, ky, w, kh, s.keys, 2, -1);
}

static bool key(lap_key_t k, char ch)
{
    if (k != LAP_KEY_CHAR) return false;
    if ((ch | 0x20) == 'c') { centre(); return true; }
    if ((ch | 0x20) == 'o') return lap_options_open(&OPTIONS);
    return false;
}

static bool touch(int col, int row)
{
    const int k = lap_ui_key_hit(col, row);
    if (k == 0) { centre(); return true; }
    if (k == 1) return lap_options_open(&OPTIONS);
    return false;
}

static void enter(void) { memset(&s, 0, sizeof(s)); }
static void leave(void) { lap_canvas_free(&s.cv); }

LAP_APP(.id = "@ID@", .name = "@NAME@", .sub = "@SUB@", .version = "0.1",
        .author = "@AUTHOR@", .color = LAP_GREEN,
        .icon_art = { " ._____.  ", " |  _  |  ", " | (o) |  ", " |_____|  ", "          ", "@ART@" },
        .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch);
