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
#define LAP_TARGET_API_22   /* API 2.2, as the store has listed it from 2.19 firmware */
#include "lakeshark_app.h"
#include "lakeshark_ui.h"
#include "trial_page.h"
#include "scope.h"
#include <math.h>
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
/* The level strip: the loudest reading of each 20 ms, the last HIST_N. */
#define HIST_N         256
#define HIST_BUCKET_US 20000
/* The timeline spans eight periods once there is a rhythm, two seconds
   until then (as the readout's strip does). */
#define STRIP_SPAN_US     2000000
#define STRIP_SPAN_MIN_US 50000
#define STRIP_SPAN_MAX_US 10000000

static struct {
    trial_t t;
    uint32_t hz, filter_hz;
    int thresh_db;
    scope_t sc;
    int16_t iq[2 * READ_PAIRS];
    uint32_t view_ms;
    int n;                      /* readout lines in t.lines */
    int room;
    float hist[HIST_N];
    int hist_n;
    int64_t hist_bucket;
    float hist_max;
    burst_rhythm_t rh;
    uint32_t len[BURST_LEN_BUCKETS], gap[BURST_GAP_BUCKETS];
    int64_t span;
} s;

static void hist_feed(int got);

static void start(void)
{
    /* An SDR samples no faster than the filter needs; the LR2021 has one rate. */
    const uint32_t want = s.filter_hz * 5 / 4 > 250000u ? s.filter_hz * 5 / 4 : 250000u;
    s.t.on = trial_open(&s.t, s.hz, want);
    if (!s.t.on) return;
    scope_start(&s.sc, s.hz, s.t.cfg.rate_hz, s.filter_hz, s.thresh_db);
    s.hist_n = 0;
    s.hist_bucket = 0;
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
        const uint64_t before = s.sc.reads;
        scope_feed(&s.sc, s.iq, got, s.t.meta.first_sample, (s.t.meta.flags & LAP_IQ_GAP) != 0);
        hist_feed((int)(s.sc.reads - before));
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

/* The level as it came in: the loudest reading in each 20 ms of radio time,
   newest last, so the strip moves only as readings arrive. */
static void hist_feed(int got)
{
    for (int k = 0; k < got; k++) {
        const int64_t b = s.sc.tt[k] / HIST_BUCKET_US;
        if (!s.hist_bucket) { s.hist_bucket = b; s.hist_max = s.sc.db[k]; continue; }
        if (b == s.hist_bucket) { if (s.sc.db[k] > s.hist_max) s.hist_max = s.sc.db[k]; continue; }
        /* The bucket just closed, then a hole for each one nothing came in. */
        int64_t skip = b - s.hist_bucket - 1;
        if (skip > HIST_N) skip = HIST_N;
        if (skip < 0) skip = 0;
        for (int64_t i = -1; i < skip; i++) {
            if (s.hist_n == HIST_N) memmove(s.hist, s.hist + 1, sizeof(s.hist[0]) * (HIST_N - 1));
            else s.hist_n++;
            s.hist[s.hist_n - 1] = i < 0 ? s.hist_max : -200.0f;
        }
        s.hist_bucket = b;
        s.hist_max = s.sc.db[k];
    }
}

/* What the views share, worked out every VIEW_MS rather than every frame. */
static void derive(void)
{
    burst_rhythm(&s.sc.log, &s.rh);
    burst_hist(&s.sc.log, s.len, s.gap);
    s.span = STRIP_SPAN_US;
    if (s.rh.mode_us) {
        s.span = (int64_t)s.rh.mode_us * 8;
        if (s.span < STRIP_SPAN_MIN_US) s.span = STRIP_SPAN_MIN_US;
        if (s.span > STRIP_SPAN_MAX_US) s.span = STRIP_SPAN_MAX_US;
    }
}

static float on_db(void)
{
    const burst_det_t *d = &s.sc.det;
    const float margin = d->thresh_db > BURST_SPREADS * d->spread_db ? d->thresh_db : BURST_SPREADS * d->spread_db;
    return d->floor_db + margin;
}

static void glass(int x, int y, int w, int h, bool live)
{
    const int below = trial_freq(x, y, w, h, s.hz);
    static char f[12], th[12], rate[12];
    snprintf(f, sizeof(f), "%.1fk", (s.t.on || s.sc.reads ? s.sc.filter_hz : s.filter_hz) / 1e3);
    snprintf(th, sizeof(th), "+%d dB", s.thresh_db);
    snprintf(rate, sizeof(rate), "%luus", (unsigned long)(s.sc.reads ? burst_level_us(&s.sc.lvl) : SCOPE_READING_US));
    const lap_chip_t left[] = { { "FILTER", true, LAP_CYAN }, { f, true, LAP_CYAN }, { th, true, LAP_YELLOW },
                                { rate, false, LAP_WHITE } };
    const lap_chip_t right[] = { { live ? "LIVE" : s.sc.reads ? "HELD" : "IDLE", live, live ? LAP_GREEN : LAP_WHITE } };
    lap_ui_rail(x, below < y + h ? below : y + h - 1, w, left, 4, right, 1);
}

static void level_view(int x, int y, int w, int h)
{
    char right[24];
    snprintf(right, sizeof(right), "floor %.0f dBFS", (double)s.sc.det.floor_db);
    trial_rule(x, y, w, "LEVEL", s.sc.reads ? right : "dBFS", LAP_WHITE | LAP_BRIGHT);
    const int gh = h - 2, gw = w - 6;
    if (gh < 2 || gw < 8) return;
    const float on = on_db();
    float top = on + 8;
    for (int i = s.hist_n - gw * 2; i < s.hist_n; i++) if (i >= 0 && s.hist[i] > top) top = s.hist[i];
    const float lo = floorf(s.sc.det.floor_db - 4), hi = ceilf((top + 2) / 5) * 5;
    /* The threshold as a faint rule the trace rises through. */
    const int thr_row = y + 1 + (int)((hi - on) / (hi - lo) * gh);
    if (thr_row > y && thr_row < y + 1 + gh) lap_hline(x, thr_row, gw, '-', LAP_UI_FAINT);
    trial_braille(x, y + 1, gw, gh, s.hist, s.hist_n, lo, hi, on, TRIAL_A(LAP_CYAN | LAP_BRIGHT),
                  TRIAL_A(LAP_YELLOW | LAP_BRIGHT), true);
    char v[12];
    snprintf(v, sizeof(v), "%.0f", (double)hi);
    lap_put_str(x + gw + 1, y + 1, v, LAP_UI_DIM);
    snprintf(v, sizeof(v), "%.0f", (double)lo);
    lap_put_str(x + gw + 1, y + gh, v, LAP_UI_DIM);
    if (thr_row > y + 1 && thr_row < y + gh) {
        snprintf(v, sizeof(v), "<%+d", s.thresh_db);
        lap_put_str(x + gw + 1, thr_row, v, TRIAL_A(LAP_YELLOW | LAP_BRIGHT));
    }
    /* NOW, against the same scale. */
    const int ny = y + h - 1;
    if (s.sc.have_now) {
        snprintf(v, sizeof(v), "NOW %4.0f", (double)s.sc.now_db);
        const bool over = s.sc.now_db >= on;
        lap_put_str(x, ny, v, over ? TRIAL_A(LAP_YELLOW | LAP_BRIGHT) : LAP_UI_TEXT);
        float f = (s.sc.now_db - lo) / (hi - lo);
        lap_ui_meter(x + 9, ny, gw - 9, f < 0 ? 0 : f > 1 ? 1 : f, over ? LAP_YELLOW | LAP_BRIGHT : LAP_GREEN | LAP_BRIGHT);
        lap_put_str(x + gw + 1, ny, "dBFS", LAP_UI_DIM);
    } else lap_put_str(x, ny, "NOW  no reading yet", LAP_UI_DIM);
}

/* The last span of radio time, a cell a slice: a burst stands one row high,
   two when it is strong, coloured by how far it cleared the floor; older
   than half the strip it dims. */
static void timeline_view(int x, int y, int w)
{
    char span_s[12], right[24];
    burst_fmt_us((uint32_t)s.span, span_s, sizeof(span_s));
    snprintf(right, sizeof(right), "last %s", span_s);
    trial_rule(x, y, w, "TIMELINE", right, LAP_WHITE | LAP_BRIGHT);
    const int64_t now = s.sc.t_now, t0 = now - s.span;
    int64_t since = s.sc.t_start;
    if (s.sc.log.n == BURST_RING) {
        const int64_t oldest = burst_log_at(&s.sc.log, s.sc.log.n - 1)->start_us;
        if (oldest > since) since = oldest;
    }
    for (int c = 0; c < w; c++) {
        const int64_t z = t0 + s.span * (c + 1) / w;
        lap_put_cell(x + c, y + 2, s.sc.reads && z > since ? '_' : ' ', TRIAL_A(LAP_CYAN));
    }
    for (int k = s.sc.log.n - 1; k >= 0; k--) {          /* oldest first, newest on top */
        const burst_t *b = burst_log_at(&s.sc.log, k);
        const int64_t a = b->start_us, e = b->start_us + b->len_us;
        if (e <= t0 || a >= now) continue;
        int c0 = (int)((a - t0) * w / s.span), c1 = (int)((e - 1 - t0) * w / s.span);
        if (c0 < 0) c0 = 0;
        if (c1 >= w) c1 = w - 1;
        const float over = b->peak_db - b->floor_db;
        uint8_t ink = trial_ramp(over);
        if (now - a > s.span / 2) ink &= (uint8_t)~LAP_BRIGHT;
        for (int c = c0; c <= c1; c++) {
            lap_put_cell(x + c, y + 2, '|', ink);
            lap_put_cell(x + c, y + 1, over >= 16 ? '|' : '.', ink);
        }
    }
    /* The ruler: quarters of the span back from now. */
    for (int q = 0; q <= 4; q++) {
        const int c = q == 4 ? w - 1 : q * (w - 1) / 4;
        lap_put_cell(x + c, y + 3, '\'', LAP_UI_FAINT);
    }
    char l[16];
    burst_fmt_us((uint32_t)s.span, span_s, sizeof(span_s));
    snprintf(l, sizeof(l), "-%s", span_s);
    lap_put_str(x + 1, y + 3, l, LAP_UI_DIM);
    burst_fmt_us((uint32_t)(s.span / 2), span_s, sizeof(span_s));
    snprintf(l, sizeof(l), "-%s", span_s);
    lap_put_str(x + (w - 1) / 2 - (int)strlen(l) / 2, y + 3, l, LAP_UI_DIM);
    lap_put_str(x + w - 4, y + 3, "now", LAP_UI_DIM);
}

/* One histogram: a row a bucket, ticks to its count, the commonest in
   VALUE ink. */
static void hist_view(int x, int y, int w, const char *title, const uint32_t *n, int buckets,
                      const char *(*label)(int))
{
    uint32_t most = 0;
    for (int i = 0; i < buckets; i++) if (n[i] > most) most = n[i];
    trial_rule(x, y, w, title, NULL, LAP_WHITE | LAP_BRIGHT);
    const int bar = w - 9;
    for (int i = 0; i < buckets; i++) {
        lap_put_str(x + 4 - (int)strlen(label(i)), y + 1 + i, label(i), LAP_UI_DIM);
        int fill = most ? (int)((n[i] * (uint32_t)bar + most - 1) / most) : 0;
        if (n[i] && fill < 1) fill = 1;
        const uint8_t ink = n[i] == most && most ? TRIAL_A(LAP_YELLOW | LAP_BRIGHT) : TRIAL_A(LAP_GREEN | LAP_BRIGHT);
        trial_ticks(x + 5, y + 1 + i, bar, fill, ink, '|');
        char c[8];
        snprintf(c, sizeof(c), "%3lu", (unsigned long)(n[i] > 999 ? 999 : n[i]));
        lap_put_str(x + w - 3, y + 1 + i, n[i] ? c : "  .", n[i] ? LAP_UI_TEXT : LAP_UI_FAINT);
    }
}

static void hists_view(int x, int y, int w)
{
    const int half = (w - 2) / 2;
    hist_view(x, y, half, "LENGTH", s.len, BURST_LEN_BUCKETS, burst_len_label);
    hist_view(x + half + 2, y, w - half - 2, "GAP", s.gap, BURST_GAP_BUCKETS, burst_gap_label);
}

/* The rhythm in big figures, and one period of it drawn to scale. */
static void rhythm_view(int x, int y, int w)
{
    trial_rule(x, y, w, "RHYTHM", s.rh.mode_us ? "every" : NULL, LAP_WHITE | LAP_BRIGHT);
    if (!s.rh.mode_us) {
        lap_put_str(x, y + 2, "EVERY  --", TRIAL_A(LAP_YELLOW | LAP_BRIGHT));
        lap_put_str(x, y + 3, s.sc.log.count ? "too few bursts in a row yet" : "no burst yet", LAP_UI_DIM);
        lap_put_str(x, y + 4, "a steady sender shows its period here", LAP_UI_FAINT);
        return;
    }
    const uint32_t us = s.rh.mode_us;
    char big[12], unit[4];
    if (us < 1000) { snprintf(big, sizeof(big), "%lu", (unsigned long)us); strcpy(unit, "us"); }
    else if (us < 1000000) { snprintf(big, sizeof(big), us < 100000 ? "%.1f" : "%.0f", us / 1000.0); strcpy(unit, "ms"); }
    else { snprintf(big, sizeof(big), "%.2f", us / 1e6); strcpy(unit, "s"); }
    const int bw = trial_big(x, y + 1, big, TRIAL_A(LAP_YELLOW | LAP_BRIGHT));
    lap_put_str(x + bw, y + 5, unit, TRIAL_A(LAP_YELLOW));
    const int tx = x + bw + 4;
    char l[40], len_s[12];
    burst_fmt_us(s.rh.median_len_us, len_s, sizeof(len_s));
    const int duty = (int)(100.0 * s.rh.median_len_us / us + 0.5);
    if (tx + 12 < x + w) {
        snprintf(l, sizeof(l), "%d of %d fit", s.rh.period_hits, s.rh.intervals);
        lap_put_str(tx, y + 1, l, s.rh.period_hits * 4 >= s.rh.intervals * 3 ? TRIAL_A(LAP_GREEN | LAP_BRIGHT) : LAP_UI_TEXT);
        snprintf(l, sizeof(l), "LEN  ~%s", len_s);
        lap_put_str(tx, y + 2, l, LAP_UI_TEXT);
        snprintf(l, sizeof(l), "DUTY %d%%", duty > 100 ? 100 : duty);
        lap_put_str(tx, y + 3, l, LAP_UI_TEXT);
        snprintf(l, sizeof(l), "ALL  %lu", (unsigned long)s.sc.log.count);
        lap_put_str(tx, y + 4, l, LAP_UI_DIM);
    }
    /* Three periods: on for the length, off for the rest. */
    const int wy = y + 7, pc = w / 3;
    int on = pc > 2 ? (int)((int64_t)pc * s.rh.median_len_us / us + 0.5) : 1;
    if (on < 1) on = 1;
    if (on > pc - 2) on = pc - 2;
    for (int p = 0; p < 3; p++) {
        const int a = x + p * pc + 1;
        for (int c = 0; c < pc; c++) {
            const bool high = c >= 1 && c <= on;
            const bool edge = c == 0 || c == on + 1;
            lap_put_cell(a + c - 1, wy - 1, high ? '-' : edge ? '.' : ' ', TRIAL_A(LAP_YELLOW | LAP_BRIGHT));
            lap_put_cell(a + c - 1, wy, edge ? '|' : high ? ' ' : '_', edge ? TRIAL_A(LAP_YELLOW | LAP_BRIGHT) : TRIAL_A(LAP_CYAN));
        }
    }
}

/* The newest bursts, newest on top: how long ago, how long, how strong and
   how far over the floor. */
static void newest_view(int x, int y, int w, int h)
{
    trial_rule(x, y, w, "NEWEST", "len  peak  over", LAP_WHITE | LAP_BRIGHT);
    if (!s.sc.log.n) {
        lap_put_str(x + 1, y + 2, "nothing over the floor yet", LAP_UI_DIM);
        return;
    }
    for (int k = 0; k < h - 1 && k < s.sc.log.n; k++) {
        const burst_t *b = burst_log_at(&s.sc.log, k);
        char ago[12], len_s[12], l[48];
        burst_fmt_us((uint32_t)(s.sc.t_now - b->start_us > 0 ? s.sc.t_now - b->start_us : 0), ago, sizeof(ago));
        burst_fmt_us(b->len_us, len_s, sizeof(len_s));
        const float over = b->peak_db - b->floor_db;
        snprintf(l, sizeof(l), "-%-7s %7s %4.0f %+3.0f", ago, len_s, (double)b->peak_db, (double)over);
        const uint8_t ink = k == 0 ? LAP_UI_TEXT : k < 4 ? LAP_UI_DIM : LAP_UI_FAINT;
        lap_put_str(x, y + 1 + k, l, ink);
        if (b->cut) lap_put_cell(x + 8, y + 1 + k, '~', TRIAL_A(LAP_YELLOW | LAP_BRIGHT));
        const int bx = x + (int)strlen(l) + 1, bw = x + w - bx;
        if (bw >= 4) {
            int fill = (int)(over / 40.0f * bw + 0.5f);
            if (fill > bw) fill = bw;
            if (fill < 1) fill = 1;
            uint8_t bi = trial_ramp(over);
            if (k >= 4) bi &= (uint8_t)~LAP_BRIGHT;
            trial_ticks(bx, y + 1 + k, bw, fill, bi, '=');
        }
    }
}

/* Before the first run: the three kinds of sender it was made for, each in
   its own ink, and a pulse train under them. Eleven rows. */
static void idle_art(int x, int y, int w)
{
    static const char *const FOB[] = { ".------.", "| o  o |", "|  ==  |", "'--.---'", "KEY FOB ", "315/433 " };
    static const char *const TYRE[] = { "  .---.  ", " / .-. \\ ", "| ( o ) |", " \\ '-' / ", "   TYRE  ", " 315/433 " };
    static const char *const GATE[] = { " ___|___ ", "[_TOLL_] ", "  |   |  ", "  |   |  ", " READER  ", " 911-919 " };
    static const uint8_t INK[3] = { TRIAL_A(LAP_CYAN | LAP_BRIGHT), TRIAL_A(LAP_WHITE | LAP_BRIGHT), TRIAL_A(LAP_GREEN | LAP_BRIGHT) };
    trial_rule(x, y, w, "WHAT IT TIMES", NULL, LAP_WHITE | LAP_BRIGHT);
    const char *const *art[3] = { FOB, TYRE, GATE };
    const int aw = 30, ax = x + (w > aw ? (w - aw) / 2 : 0);
    for (int o = 0; o < 3; o++)
        for (int r = 0; r < 6; r++)
            lap_put_str(ax + o * 11 - (o ? 1 : 0), y + 2 + r, art[o][r],
                        r < 4 ? INK[o] : r == 4 ? LAP_UI_TEXT : TRIAL_A(LAP_YELLOW));
    /* What a sender looks like here: drawn, not heard. */
    for (int c = 0; c < w; c++) {
        const int p = c % 12;
        lap_put_cell(x + c, y + 9, p == 2 || p == 5 ? '.' : p > 2 && p < 5 ? '-' : ' ', TRIAL_A(LAP_YELLOW));
        lap_put_cell(x + c, y + 10, p == 2 || p == 5 ? '|' : p > 2 && p < 5 ? ' ' : '_', TRIAL_A(LAP_CYAN));
    }
}

static void idle_text(int x, int y, int w, int h)
{
    int yy = y;
    yy += trial_wrap(x, yy, w, 2, s.t.sub, LAP_UI_TEXT);
    char needs[160];
    snprintf(needs, sizeof(needs), "NEEDS %s", s.t.needs);
    yy += trial_wrap(x, yy, w, 3, needs, TRIAL_A(LAP_YELLOW | LAP_BRIGHT));
    yy++;
    if (yy < y + h) yy += trial_wrap(x, yy, w, y + h - yy,
        "START times every burst over the floor: its length, the gap to the next and the rhythm they keep.",
        LAP_UI_DIM);
    if (yy + 1 < y + h) trial_wrap(x, yy + 1, w, y + h - yy - 1,
        "OPTIONS > PLACE has fobs and tyre sensors at 315 and 433.92 MHz and the five toll reader channels.",
        LAP_UI_FAINT);
}

static void draw(void)
{
    lap_clear(LAP_UI_DIM);
    trial_refresh(&s.t);
    pump();
    if (!s.view_ms || lap_millis() - s.view_ms >= VIEW_MS) {
        if (s.sc.reads)
            s.n = scope_lines(&s.sc, s.t.lines, TRIAL_LINES);
        else
            s.n = scope_idle_lines(s.hz, s.filter_hz, s.thresh_db, s.t.lines, TRIAL_LINES);
        derive();
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
    if (!s.sc.reads) {
        if (f.cols > 1) {
            if (trial_flow_rest(&f, 4, &x, &y, &w, &h)) idle_text(x, y, w, h);
            if (trial_flow_take(&f, 11, &x, &y, &w)) idle_art(x, y, w);
        } else {
            if (trial_flow_take(&f, 11, &x, &y, &w)) idle_art(x, y, w);
            if (trial_flow_rest(&f, 4, &x, &y, &w, &h)) idle_text(x, y, w, h);
        }
        return;
    }
    if (f.cols > 1) {
        if (trial_flow_rest(&f, 7, &x, &y, &w, &h)) level_view(x, y, w, h);
    } else if (trial_flow_take(&f, 9, &x, &y, &w)) level_view(x, y, w, 9);
    if (trial_flow_take(&f, 4, &x, &y, &w)) timeline_view(x, y, w);
    if (trial_flow_take(&f, 1 + BURST_GAP_BUCKETS, &x, &y, &w)) hists_view(x, y, w);
    if (trial_flow_take(&f, 8, &x, &y, &w)) rhythm_view(x, y, w);
    if (trial_flow_rest(&f, 3, &x, &y, &w, &h)) newest_view(x, y, w, h);
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

LAP_APP(.id = "burst", .name = "BURST SCOPE", .sub = "burst timing", .version = "1.1",
        .author = "LakeShark", .icon = LAP_ICON_WAVE, .color = LAP_YELLOW, .flags = LAP_CAP_RADIO_RX,
        .icon_art = { "  |  |  | ", " _|__|__|_", "          ", " |<-->|   ", "  BURST   ", "  SCOPE   " },
        .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch);
