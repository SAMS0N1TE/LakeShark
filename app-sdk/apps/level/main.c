/* CARRIER LEVEL: how strong one frequency is.
 *
 * The radio picked under RADIO (the LR2021 in the LoRa socket, or an SDR)
 * is tuned there, and the power of a 12.5 kHz channel at the tuning is read
 * from its I/Q every 2 ms: now, over the last whole second and since START,
 * in dB relative to the radio's full scale, with a bar.
 *
 * Keys: s start/stop, r radio, o options, n note. */
#define LAP_TARGET_API_22   /* API 2.2, as the store has listed it from 2.19 firmware */
#include "lakeshark_app.h"
#include "lakeshark_ui.h"
#include "../burst/trial_page.h"
#include "meter.h"
#include <math.h>

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
#define HIST_N     240          /* the history: 200 ms a value, 48 s */

static struct {
    trial_t t;
    uint32_t hz;
    meter_t m;
    bool ran;
    int16_t iq[2 * READ_PAIRS];
    uint32_t view_ms;
    int n, room;
    float hist[HIST_N];
    int hist_n;
    int64_t hist_bucket;
    double acc;
    int acc_n;
} s;

static void hist_add(uint64_t first_sample, uint32_t rate);

static void start(void)
{
    s.t.on = trial_open(&s.t, s.hz, 250000u);
    if (!s.t.on) return;
    meter_start(&s.m, s.hz, s.t.cfg.rate_hz);
    s.hist_n = 0;
    s.hist_bucket = 0;
    s.acc = 0;
    s.acc_n = 0;
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
        const uint32_t before = s.m.reads;
        meter_feed(&s.m, s.iq, got, s.t.meta.first_sample, (s.t.meta.flags & LAP_IQ_GAP) != 0);
        if (s.m.reads != before) hist_add(s.t.meta.first_sample, s.t.cfg.rate_hz);
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

/* ---- the screen -------------------------------------------------------- */

/* The level history: the mean of each 200 ms of radio time, in dB. */
static void hist_add(uint64_t first_sample, uint32_t rate)
{
    const int64_t b = (int64_t)(first_sample * 5u / (rate ? rate : 1));
    if (!s.hist_bucket) s.hist_bucket = b;
    if (b != s.hist_bucket && s.acc_n) {
        int64_t skip = b - s.hist_bucket - 1;
        if (skip > HIST_N) skip = HIST_N;
        if (skip < 0) skip = 0;
        for (int64_t i = -1; i < skip; i++) {
            if (s.hist_n == HIST_N) memmove(s.hist, s.hist + 1, sizeof(s.hist[0]) * (HIST_N - 1));
            else s.hist_n++;
            s.hist[s.hist_n - 1] = i < 0 ? (float)(s.acc / s.acc_n) : -200.0f;
        }
        s.acc = 0;
        s.acc_n = 0;
    }
    s.hist_bucket = b;
    s.acc += s.m.now;
    s.acc_n++;
}

/* A level's place on the -100..0 dBFS scale, 0..1. */
static float frac_of(float db)
{
    const float f = (db - METER_BAR_LO) / (METER_BAR_HI - METER_BAR_LO);
    return f < 0 ? 0 : f > 1 ? 1 : f;
}

/* Colour for how strong: faint cyan, clear green, strong yellow, near full
   scale red. */
static uint8_t level_ink(float db)
{
    return TRIAL_A(db < -80 ? LAP_CYAN | LAP_BRIGHT : db < -50 ? LAP_GREEN | LAP_BRIGHT
                   : db < -15 ? LAP_YELLOW | LAP_BRIGHT : LAP_RED | LAP_BRIGHT);
}

static void glass(int x, int y, int w, int h, bool live)
{
    const int below = trial_freq(x, y, w, h, s.hz);
    static char rd[12];
    snprintf(rd, sizeof(rd), "%u ms", METER_READING_US / 1000u);
    const lap_chip_t left[] = { { "CHANNEL", true, LAP_CYAN }, { "12.5k", true, LAP_CYAN }, { "READ", false, LAP_WHITE },
                                { rd, true, LAP_YELLOW } };
    const lap_chip_t right[] = { { live ? "LIVE" : s.ran ? "HELD" : "IDLE", live, live ? LAP_GREEN : LAP_WHITE } };
    lap_ui_rail(x, below < y + h ? below : y + h - 1, w, left, 4, right, 1);
}

/* NOW in big figures, the last second beside it. Six rows. */
static void now_view(int x, int y, int w)
{
    trial_rule(x, y, w, "LEVEL", "dBFS", LAP_WHITE | LAP_BRIGHT);
    char v[16];
    if (s.m.have_now) snprintf(v, sizeof(v), "%.1f", (double)s.m.now);
    else snprintf(v, sizeof(v), "--");
    const int bw = trial_big(x, y + 1, v, s.m.have_now ? level_ink(s.m.now) : LAP_UI_FAINT);
    lap_put_str(x + bw, y + 5, "dBFS", LAP_UI_DIM);
    const int tx = x + bw + 5;
    if (tx + 11 > x + w) return;
    const meter_span_t *p = &s.m.second;
    lap_put_str(tx, y + 1, "LAST SECOND", LAP_UI_DIM);
    char l[24];
    if (p->n) {
        snprintf(l, sizeof(l), "max %6.1f", (double)p->hi);
        lap_put_str(tx, y + 2, l, TRIAL_A(LAP_RED | LAP_BRIGHT));
        snprintf(l, sizeof(l), "avg %6.1f", p->sum / p->n);
        lap_put_str(tx, y + 3, l, TRIAL_A(LAP_YELLOW | LAP_BRIGHT));
        snprintf(l, sizeof(l), "min %6.1f", (double)p->lo);
        lap_put_str(tx, y + 4, l, TRIAL_A(LAP_CYAN | LAP_BRIGHT));
        snprintf(l, sizeof(l), "swing %.0f dB", (double)(p->hi - p->lo));
        lap_put_str(tx, y + 5, l, LAP_UI_DIM);
    } else lap_put_str(tx, y + 3, "--", LAP_UI_FAINT);
}

/* A span's low, mean and high on the scale: "....<===|===>....". */
static void span_row(int x, int y, int w, const char *label, const meter_span_t *p)
{
    lap_put_str(x, y, label, LAP_UI_DIM);
    const int bx = x + 6, bw = w - 6;
    for (int c = 0; c < bw; c++) lap_put_cell(bx + c, y, '.', LAP_UI_FAINT);
    if (!p->n) return;
    const int a = (int)(frac_of(p->lo) * (bw - 1) + 0.5f), b = (int)(frac_of(p->hi) * (bw - 1) + 0.5f);
    const int m = (int)(frac_of((float)(p->sum / p->n)) * (bw - 1) + 0.5f);
    for (int c = a; c <= b; c++) lap_put_cell(bx + c, y, '=', TRIAL_A(LAP_GREEN | LAP_BRIGHT));
    lap_put_cell(bx + a, y, '<', TRIAL_A(LAP_CYAN | LAP_BRIGHT));
    lap_put_cell(bx + b, y, '>', TRIAL_A(LAP_RED | LAP_BRIGHT));
    lap_put_cell(bx + m, y, '|', TRIAL_A(LAP_YELLOW | LAP_BRIGHT));
}

/* The board's VFD bargraph on the -100..0 scale, the last second's peak
   held on it, the scale, and the second and the run as spans. Seven rows. */
static void bar_view(int x, int y, int w)
{
    trial_rule(x, y, w, "SCALE", "-100 .. 0 dBFS", LAP_WHITE | LAP_BRIGHT);
    const int bx = x + 6, bw = w - 6;
    lap_put_str(x, y + 1, "NOW", LAP_UI_TEXT);
    lap_ui_bargraph(bx, y + 1, bw, 2, s.m.have_now ? frac_of(s.m.now) : 0,
                    s.m.second.n ? frac_of(s.m.second.hi) : -1);
    /* Ticks every 20 dB, and their figures. */
    for (int c = 0; c < bw; c++) lap_put_cell(bx + c, y + 3, '-', LAP_UI_FAINT);
    for (int d = 0; d <= 5; d++) {
        const int c = d * (bw - 1) / 5;
        lap_put_cell(bx + c, y + 3, '+', TRIAL_A(LAP_CYAN));
        char t[8];
        snprintf(t, sizeof(t), "%d", -100 + d * 20);
        int tx = bx + c - (int)strlen(t) / 2;
        if (tx + (int)strlen(t) > bx + bw) tx = bx + bw - (int)strlen(t);
        lap_put_str(tx, y + 4, t, LAP_UI_DIM);
    }
    span_row(x, y + 5, w, "1 SEC", &s.m.second);
    span_row(x, y + 6, w, "START", &s.m.total);
}

/* The level over the last stretch, a line in braille, newest at the right. */
static void hist_view(int x, int y, int w, int h)
{
    char right[24];
    const int gw = w - 6;
    snprintf(right, sizeof(right), "last %d s", gw * 2 / 5);
    trial_rule(x, y, w, "HISTORY", right, LAP_WHITE | LAP_BRIGHT);
    const int gh = h - 1;
    if (gh < 2 || gw < 8) return;
    float lo = 0, hi = 0;
    bool any = false;
    for (int i = s.hist_n - gw * 2; i < s.hist_n; i++) {
        if (i < 0 || s.hist[i] < -199) continue;
        if (!any || s.hist[i] < lo) lo = s.hist[i];
        if (!any || s.hist[i] > hi) hi = s.hist[i];
        any = true;
    }
    if (!any) {
        lap_put_str(x + 1, y + 2, "the level lands here every 200 ms", LAP_UI_DIM);
        return;
    }
    lo = floorf(lo / 5) * 5 - 5;
    hi = ceilf(hi / 5) * 5 + 5;
    if (hi - lo < 20) { const float mid = (hi + lo) / 2; lo = mid - 10; hi = mid + 10; }
    /* Faint rules at the scale's ten-dB lines. */
    for (float d = ceilf(lo / 10) * 10; d < hi; d += 10) {
        const int r = y + 1 + (int)((hi - d) / (hi - lo) * gh);
        if (r > y && r < y + 1 + gh) lap_hline(x, r, gw, '.', LAP_UI_FAINT);
    }
    trial_braille(x, y + 1, gw, gh, s.hist, s.hist_n, lo, hi, -15, TRIAL_A(LAP_GREEN | LAP_BRIGHT),
                  TRIAL_A(LAP_RED | LAP_BRIGHT), true);
    char v[12];
    snprintf(v, sizeof(v), "%.0f", (double)hi);
    lap_put_str(x + gw + 1, y + 1, v, LAP_UI_DIM);
    snprintf(v, sizeof(v), "%.0f", (double)lo);
    lap_put_str(x + gw + 1, y + gh, v, LAP_UI_DIM);
}

/* Since START: the run's own low, mean and high, how many readings and
   any holes in them. Four rows. */
static void run_view(int x, int y, int w)
{
    char right[24], l[48];
    const uint32_t secs = s.t.run_ms && s.t.on ? (lap_millis() - s.t.run_ms) / 1000 : s.m.reads / 500u;
    char clock[12];
    burst_fmt_clock(secs, clock, sizeof(clock));
    snprintf(right, sizeof(right), "%s", clock);
    trial_rule(x, y, w, "SINCE START", right, LAP_WHITE | LAP_BRIGHT);
    const meter_span_t *p = &s.m.total;
    if (p->n) {
        const int third = w / 3;
        snprintf(l, sizeof(l), "max %.1f", (double)p->hi);
        lap_put_str(x, y + 1, l, TRIAL_A(LAP_RED | LAP_BRIGHT));
        snprintf(l, sizeof(l), "avg %.1f", p->sum / p->n);
        lap_put_str(x + third, y + 1, l, TRIAL_A(LAP_YELLOW | LAP_BRIGHT));
        snprintf(l, sizeof(l), "min %.1f", (double)p->lo);
        lap_put_str(x + 2 * third, y + 1, l, TRIAL_A(LAP_CYAN | LAP_BRIGHT));
    }
    snprintf(l, sizeof(l), "READS %lu, one every 2 ms", (unsigned long)s.m.reads);
    lap_put_str(x, y + 2, l, LAP_UI_DIM);
    if (s.m.gaps) snprintf(l, sizeof(l), "GAPS %lu: readings were lost there", (unsigned long)s.m.gaps);
    else snprintf(l, sizeof(l), "no gaps");
    lap_put_str(x, y + 3, l, s.m.gaps ? TRIAL_A(LAP_YELLOW | LAP_BRIGHT) : LAP_UI_FAINT);
}

/* Before the first run: a mast and its carrier, and what it does. */
static void idle_art(int x, int y, int w)
{
    static const char *const ART[] = {
        "      \\  |  /                    ",
        "    -- ( o ) --    ~~~~~~~~~~~~  ",
        "      /  |  \\    ~~~~~~~~~~~~~~~~",
        "        /|\\        ~~~~~~~~~~~~  ",
        "       / | \\                     ",
        "      /  |  \\       12.5 kHz     ",
        "  ___/___|___\\___   one channel  ",
    };
    static const uint8_t INK[] = { TRIAL_A(LAP_YELLOW | LAP_BRIGHT), TRIAL_A(LAP_YELLOW | LAP_BRIGHT),
                                   TRIAL_A(LAP_YELLOW), TRIAL_A(LAP_CYAN | LAP_BRIGHT), TRIAL_A(LAP_CYAN | LAP_BRIGHT),
                                   TRIAL_A(LAP_CYAN | LAP_BRIGHT), TRIAL_A(LAP_GREEN) };
    trial_rule(x, y, w, "ONE CARRIER", NULL, LAP_WHITE | LAP_BRIGHT);
    const int aw = (int)strlen(ART[0]);
    trial_art(x + (w > aw ? (w - aw) / 2 : 0), y + 2, w, ART, 7, INK);
}

static void idle_text(int x, int y, int w, int h)
{
    int yy = y;
    yy += trial_wrap(x, yy, w, 2, s.t.sub, LAP_UI_TEXT);
    yy++;
    if (yy < y + h) yy += trial_wrap(x, yy, w, y + h - yy,
        "START reads the power of the channel every 2 ms: now, the last second and since START, on a -100 to 0 dBFS scale.",
        LAP_UI_DIM);
    if (yy + 1 < y + h) trial_wrap(x, yy + 1, w, y + h - yy - 1,
        "OPTIONS: FREQUENCY, in the picked radio's reach (LR2021, RTL-SDR, HackRF).", LAP_UI_FAINT);
}

static void draw(void)
{
    lap_clear(LAP_UI_DIM);
    trial_refresh(&s.t);
    pump();
    if (!s.view_ms || lap_millis() - s.view_ms >= VIEW_MS) {
        s.n = meter_lines(s.ran ? &s.m : NULL, s.hz, s.t.lines, TRIAL_LINES);
        s.view_ms = lap_millis() ? lap_millis() : 1;
    }
    const trial_rect_t in = trial_frame(&s.t);
    s.room = TRIAL_LINES;           /* NOTE saves the whole readout */
    if (!in.h) return;
    const bool live = s.t.on && s.t.run_ms;
    trial_flow_t f;
    trial_flow_init(&f, in, 37);
    int x, y, w, h;
    const int gh = f.cols > 1 ? 6 : 7;
    if (trial_flow_take(&f, gh, &x, &y, &w)) glass(x, y, w, gh, live);
    if (!s.ran) {
        if (f.cols > 1) {
            if (trial_flow_rest(&f, 4, &x, &y, &w, &h)) idle_text(x, y, w, h);
            if (trial_flow_take(&f, 9, &x, &y, &w)) idle_art(x, y, w);
        } else {
            if (trial_flow_take(&f, 9, &x, &y, &w)) idle_art(x, y, w);
            if (trial_flow_rest(&f, 4, &x, &y, &w, &h)) idle_text(x, y, w, h);
        }
        return;
    }
    if (trial_flow_take(&f, 6, &x, &y, &w)) now_view(x, y, w);
    if (trial_flow_take(&f, 7, &x, &y, &w)) bar_view(x, y, w);
    if (f.cols > 1) {
        /* Landscape: the run under the scale, the history a column of its own. */
        if (trial_flow_take(&f, 4, &x, &y, &w)) run_view(x, y, w);
        f.y = f.r.y + f.r.h;
        if (trial_flow_rest(&f, 5, &x, &y, &w, &h)) hist_view(x, y, w, h);
    } else if (trial_flow_rest(&f, 10, &x, &y, &w, &h)) {
        const int hh = h - 5;
        hist_view(x, y, w, hh);
        run_view(x, y + hh + 1, w);
    }
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

LAP_APP(.id = "level", .name = "CARRIER LEVEL", .sub = "signal level", .version = "1.1",
        .author = "LakeShark", .icon = LAP_ICON_WAVE, .color = LAP_GREEN, .flags = LAP_CAP_RADIO_RX,
        .icon_art = { "      .   ", "     ##   ", "   ####   ", " ######   ", "__________", " CARRIER  " },
        .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch);
