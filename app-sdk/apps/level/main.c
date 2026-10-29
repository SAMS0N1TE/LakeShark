/* CARRIER LEVEL: how strong one frequency is.
 *
 * The radio picked under RADIO (the LR2021 in the LoRa socket, or an SDR)
 * is tuned there, and the power of a 12.5 kHz channel at the tuning is read
 * from its I/Q every 2 ms: now, over the last whole second and since START,
 * in dB relative to the radio's full scale, with a bar.
 *
 * Keys: s start/stop, r radio, o options, n note. */
#include "lakeshark_app.h"
#include "lakeshark_ui.h"
#include "../burst/trial_page.h"
#include "meter.h"

/* A level bar rising out of a floor, with the peak tick over it. */
LAP_ICON_PX("....................",
            "....................",
            "..............e.....",
            "....................",
            "...........aa.......",
            "........aaaaa.......",
            ".....aaaaaaaa.......",
            "..aaaaaaaaaaa.......",
            "..aaaaaaaaaaa.......",
            "..aaaaaaaaaaa.......",
            "4444444444444444444.",
            "....................",
            "..b..b..b..b..b..b..",
            "....................",
            "....................",
            "....................",
            "....................",
            "....................");

#define READ_PAIRS 2048
#define VIEW_MS    250u

static struct {
    trial_t t;
    uint32_t hz;
    meter_t m;
    bool ran;
    int16_t iq[2 * READ_PAIRS];
    uint32_t view_ms;
    int n, room;
} s;

static void start(void)
{
    s.t.on = trial_open(&s.t, s.hz, 250000u);
    if (!s.t.on) return;
    meter_start(&s.m, s.hz, s.t.cfg.rate_hz);
    s.ran = true;
    s.view_ms = 0;
}

static void stop(void)
{
    trial_close(&s.t);
    s.t.on = false;
}

static void pump(void)
{
    if (!trial_live(&s.t)) return;
    for (int k = 0; k < 64; k++) {
        const int got = lap_rx_read(s.t.h, s.iq, READ_PAIRS, &s.t.meta);
        if (got <= 0) break;
        meter_feed(&s.m, s.iq, got, s.t.meta.first_sample, (s.t.meta.flags & LAP_IQ_GAP) != 0);
    }
}

/* OPTIONS: the frequency, applied at once to a running session. */
static double opt_get(int id) { (void)id; return s.hz / 1e6; }
static void opt_set(int id, double v)
{
    (void)id;
    s.hz = (uint32_t)(v * 1e6 + 0.5);
    if (s.t.on) { stop(); start(); }
}
static const lap_opt_t OPT_ROWS[] = {
    { .label = "FREQUENCY", .kind = LAP_OPT_NUMBER, .id = 0, .lo = 1, .hi = 6000,
      .unit = "MHz, in the radio's reach" },
};
static const lap_opt_list_t OPTIONS = { "CARRIER", OPT_ROWS, 1, opt_get, opt_set };

static void draw(void)
{
    lap_clear(LAP_UI_DIM);
    trial_refresh(&s.t);
    pump();
    if (!s.view_ms || lap_millis() - s.view_ms >= VIEW_MS) {
        s.n = meter_lines(s.ran ? &s.m : NULL, s.hz, s.t.lines, TRIAL_LINES);
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
    trial_init(&s.t, "level", "CARRIER LEVEL", "Level of one frequency, a 12.5 kHz channel", NULL, "WORKS");
    s.hz = 915000000u;
}

static void leave(void)
{
    stop();                         /* the board would do it anyway */
}

LAP_APP(.id = "level", .name = "CARRIER LEVEL", .sub = "signal level", .version = "1.0",
        .author = "LakeShark", .icon = LAP_ICON_WAVE, .color = LAP_GREEN, .flags = LAP_CAP_RADIO_RX,
        .icon_art = { "      .   ", "     ##   ", "   ####   ", " ######   ", "__________", " CARRIER  " },
        .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch);
