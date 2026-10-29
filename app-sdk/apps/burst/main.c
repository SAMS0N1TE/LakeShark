/* BURST SCOPE: how one frequency is used in time.
 *
 * The radio picked under RADIO (the LR2021 in the LoRa socket, or an SDR)
 * listens there; its I/Q, through a channel filter of FILTER's width, is
 * read as a level about every 50 us, timed by the radio's own sample count.
 * Readings over the floor by the threshold are bursts; each one's length,
 * the gap to the next and the rhythm they keep are what tell a key fob from
 * a tyre sensor from a toll reader. Energy and timing only: nothing is
 * decoded and nothing is sent. Levels are dB relative to the radio's full
 * scale.
 *
 * Keys: s start/stop, r radio, o options, n note. */
#include "lakeshark_app.h"
#include "lakeshark_ui.h"
#include "trial_page.h"
#include "scope.h"

/* A strip of bursts over a floor line, with the timing bracket under. */
LAP_ICON_PX("....................",
            "....................",
            "...b......b......b..",
            "..bbb....bbb....bbb.",
            "..bbb....bbb....bbb.",
            "..bbb....bbb....bbb.",
            "4444444444444444444.",
            "....................",
            "..e......e..........",
            "..eeeeeeee..........",
            "..e......e..........",
            "....................",
            "....................",
            "....................",
            "....................",
            "....................",
            "....................",
            "....................");

#define READ_PAIRS 2048
#define VIEW_MS    250u

static struct {
    trial_t t;
    uint32_t hz, filter_hz;
    int thresh_db;
    scope_t sc;
    int16_t iq[2 * READ_PAIRS];
    uint32_t view_ms;
    int n;                      /* readout lines in t.lines */
    int room;
} s;

static void start(void)
{
    /* An SDR samples no faster than the filter needs; the LR2021 has one rate. */
    const uint32_t want = s.filter_hz * 5 / 4 > 250000u ? s.filter_hz * 5 / 4 : 250000u;
    s.t.on = trial_open(&s.t, s.hz, want);
    if (!s.t.on) return;
    scope_start(&s.sc, s.hz, s.t.cfg.rate_hz, s.filter_hz, s.thresh_db);
    s.view_ms = 0;
}

static void stop(void)
{
    trial_close(&s.t);
    s.t.on = false;
}

/* Each change restarts a running scope with it. */
static void restart(void) { if (s.t.on) { stop(); start(); } }

static void pump(void)
{
    if (!trial_live(&s.t)) return;
    for (int k = 0; k < 64; k++) {
        const int got = lap_rx_read(s.t.h, s.iq, READ_PAIRS, &s.t.meta);
        if (got <= 0) break;
        scope_feed(&s.sc, s.iq, got, s.t.meta.first_sample, (s.t.meta.flags & LAP_IQ_GAP) != 0);
    }
}

/* ---- OPTIONS ----------------------------------------------------------- */

/* Places worth timing: fobs and tyre sensors, and the five E-ZPass reader
   channels. The last name is whatever was typed. */
static const double PRESET_MHZ[] = { 315.0, 433.92, 911.5, 914.0, 915.75, 916.5, 919.0 };
static const char *const PRESET_NAME[] = {
    "315 fob/TPMS", "433.92 fob/TPMS", "911.50 toll", "914.00 toll", "915.75 toll",
    "916.50 toll", "919.00 toll", "typed",
};
#define PRESET_N ((int)(sizeof(PRESET_MHZ) / sizeof(PRESET_MHZ[0])))

enum { O_PLACE, O_FREQ, O_FILTER, O_THRESH };
static double opt_get(int id)
{
    switch (id) {
    case O_PLACE:
        for (int i = 0; i < PRESET_N; i++)
            if ((uint32_t)(PRESET_MHZ[i] * 1e6 + 0.5) == s.hz) return i;
        return PRESET_N;
    case O_FREQ: return s.hz / 1e6;
    case O_FILTER: return s.filter_hz / 1e3;
    default: return s.thresh_db;
    }
}
static void opt_set(int id, double v)
{
    switch (id) {
    case O_PLACE: {
        int i = (int)(v + 0.5);
        if (i < 0 || i >= PRESET_N) i = 0;
        s.hz = (uint32_t)(PRESET_MHZ[i] * 1e6 + 0.5);
        break;
    }
    case O_FREQ: s.hz = (uint32_t)(v * 1e6 + 0.5); break;
    case O_FILTER: s.filter_hz = (uint32_t)(v * 1e3 + 0.5); break;
    default: s.thresh_db = (int)(v + 0.5); break;
    }
    restart();
}
static const lap_opt_t OPT_ROWS[] = {
    { .label = "PLACE", .kind = LAP_OPT_CYCLE, .id = O_PLACE, .names = PRESET_NAME, .n = PRESET_N + 1 },
    { .label = "FREQUENCY", .kind = LAP_OPT_NUMBER, .id = O_FREQ, .lo = 1, .hi = 6000,
      .unit = "MHz, in the radio's reach" },
    { .label = "FILTER", .kind = LAP_OPT_NUMBER, .id = O_FILTER, .lo = 3.5, .hi = 3077,
      .unit = "kHz, up to the radio's rate" },
    { .label = "THRESHOLD", .kind = LAP_OPT_NUMBER, .id = O_THRESH, .lo = 3, .hi = 40,
      .unit = "dB over the floor" },
};
static const lap_opt_list_t OPTIONS = { "BURST", OPT_ROWS, 4, opt_get, opt_set };

/* ---- the screen -------------------------------------------------------- */

static void draw(void)
{
    lap_clear(LAP_UI_DIM);
    trial_refresh(&s.t);
    pump();
    if (!s.view_ms || lap_millis() - s.view_ms >= VIEW_MS) {
        if (s.t.on && s.sc.reads)
            s.n = scope_lines(&s.sc, s.t.lines, TRIAL_LINES);
        else
            s.n = scope_idle_lines(s.hz, s.filter_hz, s.thresh_db, s.t.lines, TRIAL_LINES);
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
    trial_init(&s.t, "burst", "BURST SCOPE", "Burst timing at one frequency",
               "something bursty nearby: a key fob, a tyre sensor, a toll reader", "TRYING");
    s.hz = 433920000u;
    s.filter_hz = 250000u;
    s.thresh_db = 10;
}

static void leave(void)
{
    stop();                         /* the board would do it anyway */
}

LAP_APP(.id = "burst", .name = "BURST SCOPE", .sub = "burst timing", .version = "1.0",
        .author = "LakeShark", .icon = LAP_ICON_WAVE, .color = LAP_YELLOW, .flags = LAP_CAP_RADIO_RX,
        .icon_art = { "  |  |  | ", " _|__|__|_", "          ", " |<-->|   ", "  BURST   ", "  SCOPE   " },
        .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch);
