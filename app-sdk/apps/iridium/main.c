/* IRIDIUM BURSTS: Iridium's downlink, 1616-1626.5 MHz.
 *
 * The satellites send in 90 ms TDMA frames, and the simplex channels at
 * 1626.0-1626.5 MHz (the ring alert among them) carry bursts a few
 * milliseconds long that a receiver anywhere under the satellite can see.
 * This listens on one channel for a frame or more at a time, hopping across
 * the plan, times the bursts the way BURST SCOPE does, and keeps a trace of
 * which channel was strongest each cycle: over a pass that wanders by tens
 * of kilohertz with the Doppler shift. Energy and timing only; nothing is
 * demodulated.
 *
 * The radio is the one picked under RADIO: the LR2021 (its HF input) or an
 * SDR that tunes 1.6 GHz. Keys: s start/stop, r radio, o options, n note. */
#include "lakeshark_app.h"
#include "lakeshark_ui.h"
#include "../burst/trial_page.h"
#include "irid.h"

/* A satellite over a horizon line with its bursts coming down. */
LAP_ICON_PX("....................",
            "........ff..........",
            "...bbb.ffff.bbb.....",
            "...bbbbffffbbbb.....",
            "...bbb.ffff.bbb.....",
            "........ff..........",
            "........e...........",
            ".......e.e..........",
            "......e...e.........",
            ".....e.....e........",
            "....................",
            "2222222222222222222.",
            "2222222222222222222.",
            "....................",
            "....................",
            "....................",
            "....................",
            "....................");

#define READ_PAIRS 2048
#define VIEW_MS    250u

static struct {
    trial_t t;
    int plan, dwell_ms, thresh_db;
    irid_t e;
    bool ran;
    int16_t iq[2 * READ_PAIRS];
    uint32_t view_ms;
    int n, room;
} s;

static void start(void)
{
    const irid_plan_t *p = &IRID_PLAN[s.plan];
    const uint32_t spacing = (p->hi_hz - p->lo_hz) / (uint32_t)p->ch;
    const uint32_t first = p->lo_hz + spacing / 2;
    s.t.on = trial_open(&s.t, first, spacing * 2 > 250000u ? spacing * 2 : 250000u);
    if (!s.t.on) {
        if (s.t.radio < 0 && strstr(s.t.why, "reach"))
            snprintf(s.t.why, sizeof(s.t.why), "This radio has no 1.5-2.5 GHz input");
        return;
    }
    irid_start(&s.e, s.plan, s.dwell_ms, s.thresh_db, s.t.cfg.rate_hz);
    s.ran = true;
    s.view_ms = 0;
}

static void stop(void)
{
    trial_close(&s.t);
    s.t.on = false;
}

static void restart(void) { if (s.t.on) { stop(); start(); } }

static void pump(void)
{
    if (!trial_live(&s.t)) return;
    for (int k = 0; k < 64; k++) {
        const int got = lap_rx_read(s.t.h, s.iq, READ_PAIRS, &s.t.meta);
        if (got <= 0) break;
        if (irid_feed(&s.e, s.iq, got, s.t.meta.first_sample, s.t.meta.center_hz,
                      (s.t.meta.flags & LAP_IQ_GAP) != 0))
            lap_rx_tune(s.t.h, irid_hop(&s.e));
    }
}

/* ---- OPTIONS ----------------------------------------------------------- */

static const char *const PLAN_NAME[] = { "SIMPLEX 1626.0-1626.5", "WHOLE BAND 1616-1626.5" };
enum { O_PLAN, O_DWELL, O_THRESH };
static double opt_get(int id)
{
    return id == O_PLAN ? s.plan : id == O_DWELL ? s.dwell_ms : s.thresh_db;
}
static void opt_set(int id, double v)
{
    const int i = (int)(v + 0.5);
    if (id == O_PLAN) s.plan = i >= 0 && i < IRID_PLAN_N ? i : 0;
    else if (id == O_DWELL) s.dwell_ms = i;
    else s.thresh_db = i;
    restart();
}
static const lap_opt_t OPT_ROWS[] = {
    { .label = "PLAN", .kind = LAP_OPT_CYCLE, .id = O_PLAN, .names = PLAN_NAME, .n = IRID_PLAN_N },
    { .label = "DWELL", .kind = LAP_OPT_NUMBER, .id = O_DWELL, .lo = 20, .hi = 1000,
      .unit = "ms on each channel; a frame is 90" },
    { .label = "THRESHOLD", .kind = LAP_OPT_NUMBER, .id = O_THRESH, .lo = 3, .hi = 30,
      .unit = "dB over the floor" },
};
static const lap_opt_list_t OPTIONS = { "IRIDIUM", OPT_ROWS, 3, opt_get, opt_set };

/* ---- the screen -------------------------------------------------------- */

static void draw(void)
{
    lap_clear(LAP_UI_DIM);
    trial_refresh(&s.t);
    pump();
    if (!s.view_ms || lap_millis() - s.view_ms >= VIEW_MS) {
        if (s.t.on && s.ran)
            s.n = irid_lines(&s.e, s.t.lines, TRIAL_LINES);
        else
            s.n = irid_idle_lines(s.plan, s.dwell_ms, s.thresh_db, s.t.lines, TRIAL_LINES);
        s.view_ms = lap_millis() ? lap_millis() : 1;
    }
    s.room = trial_draw(&s.t, s.n);
}

static bool key(lap_key_t k, char ch)
{
    if (k != LAP_KEY_CHAR) return false;
    switch (ch | 0x20) {
    case 's':
        s.t.feedback[0] = 0;
        if (s.t.on) stop(); else start();
        s.view_ms = 0;
        return true;
    case 'o': lap_options_open(&OPTIONS); return true;
    default: return trial_key(&s.t, ch, s.n < s.room ? s.n : s.room);
    }
}

static bool touch(int col, int row)
{
    const char ch = trial_touch(&s.t, col, row);
    return ch && key(LAP_KEY_CHAR, ch);
}

static void enter(void)
{
    memset(&s, 0, sizeof(s));
    trial_init(&s.t, "iridium", "IRIDIUM BURSTS", "Satellite bursts at 1616-1626.5 MHz",
               "open sky; the LR2021's HF input is matched for 2.4 GHz, so only strong bursts show", "TRYING");
    s.dwell_ms = 90;
    s.thresh_db = 8;
}

static void leave(void)
{
    stop();                         /* the board would do it anyway */
}

LAP_APP(.id = "iridium", .name = "IRIDIUM BURSTS", .sub = "satellite bursts", .version = "1.0",
        .author = "LakeShark", .icon = LAP_ICON_STAR, .color = LAP_CYAN, .flags = LAP_CAP_RADIO_RX,
        .icon_art = { " [=]o[=]  ", "    |     ", "   / \\    ", "  /   \\   ", "__________", " IRIDIUM  " },
        .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch);
