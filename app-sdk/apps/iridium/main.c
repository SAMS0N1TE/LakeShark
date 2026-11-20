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
#define LAP_TARGET_API_22   /* API 2.2, as the store has listed it from 2.19 firmware */
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
    uint32_t seen[IRID_CH_MAX];     /* each channel's count when last looked at */
    uint32_t last_ms[IRID_CH_MAX];  /* when it last grew, board time (0 never) */
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
    memset(s.seen, 0, sizeof(s.seen));
    memset(s.last_ms, 0, sizeof(s.last_ms));
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

/* When each channel last had a burst, by the board's clock: the bars
   brighten with it and fade as it ages. */
static void watch_channels(void)
{
    for (int c = 0; c < s.e.plan.ch && c < IRID_CH_MAX; c++)
        if (s.e.ch_count[c] != s.seen[c]) { s.seen[c] = s.e.ch_count[c]; s.last_ms[c] = lap_millis() | 1u; }
}

static uint8_t fresh_ink(int c)
{
    if (!s.e.ch_count[c] || !s.last_ms[c]) return LAP_UI_FAINT;
    const uint32_t age = lap_millis() - s.last_ms[c];
    return TRIAL_A(age < 10000 ? LAP_YELLOW | LAP_BRIGHT : age < 60000 ? LAP_GREEN | LAP_BRIGHT : LAP_GREEN);
}

/* The plan's channels across w cells: how wide each is and where the
   first starts. */
static int ch_w(int w, int nch) { const int cw = (w - 10) / nch; return cw < 1 ? 1 : cw > 3 ? 3 : cw; }

/* The satellite and the plan beside it. */
static void head_view(int x, int y, int w, bool live)
{
    static const char *const SAT[] = { "[==]-o-[==]", "    /|\\    ", "   / | \\   ", "  '  :  '  ", "~~~~~~~~~~~" };
    static const uint8_t INK[] = { TRIAL_A(LAP_CYAN | LAP_BRIGHT), TRIAL_A(LAP_YELLOW), TRIAL_A(LAP_YELLOW),
                                   TRIAL_A(LAP_YELLOW | LAP_BRIGHT), TRIAL_A(LAP_GREEN) };
    trial_art(x, y, w, SAT, 5, INK);
    const irid_plan_t *p = &IRID_PLAN[s.plan];
    const int tx = x + 13, tw = w - 13;
    char l[48];
    const bool ran = s.ran && s.e.reads;
    lap_put_str(tx, y, p->name, TRIAL_A(LAP_WHITE | LAP_BRIGHT));
    snprintf(l, sizeof(l), "%.3f-%.3f MHz", p->lo_hz / 1e6, p->hi_hz / 1e6);
    lap_put_str(tx, y + 1, l, TRIAL_A(LAP_YELLOW | LAP_BRIGHT));
    const uint32_t spacing = (p->hi_hz - p->lo_hz) / (uint32_t)p->ch;
    snprintf(l, sizeof(l), "%d ch x %.1f kHz", p->ch, spacing / 1e3);
    lap_put_str(tx, y + 2, l, LAP_UI_TEXT);
    const double cycle = ran && s.e.cycles ? (s.e.t_now - s.e.t_start) / 1e6 / s.e.cycles : p->ch * s.dwell_ms / 1000.0;
    snprintf(l, sizeof(l), "DWELL %d ms  CYCLE %.1f s", s.dwell_ms, cycle);
    lap_put_str(tx, y + 3, l, LAP_UI_DIM);
    if (ran && live) {
        snprintf(l, sizeof(l), "ON %.3f", irid_ch_hz(&s.e, s.e.ch) / 1e6);
        lap_put_str(tx, y + 4, l, TRIAL_A(LAP_GREEN | LAP_BRIGHT));
    } else lap_put_str(tx, y + 4, ran ? "held: START listens again" : "hops the plan on START", LAP_UI_DIM);
    (void)tw;
}

/* Totals in two columns. */
static void stats_view(int x, int y, int w)
{
    trial_rule(x, y, w, "BURSTS", NULL, LAP_WHITE | LAP_BRIGHT);
    const uint32_t s_now = (uint32_t)((s.e.t_now - s.e.t_start) / 1000000);
    uint32_t minute = 0;
    for (int i = 0; i < 60; i++)
        if (s.e.sec_at[i] + 60 > s_now && s.e.sec_at[i] <= s_now) minute += s.e.sec_n[i];
    char l[40], len_s[12];
    const int half = w / 2;
    snprintf(l, sizeof(l), "ALL      %lu", (unsigned long)s.e.log.count);
    lap_put_str(x, y + 1, l, s.e.log.count ? TRIAL_A(LAP_YELLOW | LAP_BRIGHT) : LAP_UI_TEXT);
    snprintf(l, sizeof(l), "LAST MIN %lu", (unsigned long)minute);
    lap_put_str(x, y + 2, l, LAP_UI_TEXT);
    snprintf(l, sizeof(l), "CYCLES   %lu", (unsigned long)s.e.cycles);
    lap_put_str(x, y + 3, l, LAP_UI_DIM);
    if (s.e.log.count) snprintf(l, sizeof(l), "PEAK %.0f dBFS", (double)s.e.log.peak_db);
    else snprintf(l, sizeof(l), "PEAK --");
    lap_put_str(x + half, y + 1, l, LAP_UI_TEXT);
    burst_rhythm_t r;
    burst_rhythm(&s.e.log, &r);
    burst_fmt_us(r.median_len_us, len_s, sizeof(len_s));
    snprintf(l, sizeof(l), "LEN  %s", s.e.log.count ? len_s : "--");
    lap_put_str(x + half, y + 2, l, LAP_UI_TEXT);
    snprintf(l, sizeof(l), "SPURS %lu", (unsigned long)s.e.spurs);
    lap_put_str(x + half, y + 3, l, s.e.spurs ? TRIAL_A(LAP_MAGENTA | LAP_BRIGHT) : LAP_UI_DIM);
}

/* A bar a channel, its bursts in ticks, bright while fresh; the channel
   being listened to now under it. */
static void channels_view(int x, int y, int w, int h, bool live)
{
    const int nch = s.e.plan.ch, cw = ch_w(w, nch), gw = cw * nch;
    char right[24];
    snprintf(right, sizeof(right), "%d x %.1fk", nch, s.e.spacing / 1e3);
    trial_rule(x, y, w, "CHANNELS", right, LAP_WHITE | LAP_BRIGHT);
    const int bh = h - 4;
    if (bh < 2) return;
    uint32_t most = 0;
    for (int c = 0; c < nch; c++) if (s.e.ch_count[c] > most) most = s.e.ch_count[c];
    const int gx = x + (w - 10 - gw) / 2;
    for (int c = 0; c < nch; c++) {
        const int cx = gx + c * cw + (cw > 2 ? 1 : 0);
        const int ticks = most ? (int)((s.e.ch_count[c] * (uint32_t)(bh * 2) + most - 1) / most) : 0;
        const uint8_t ink = fresh_ink(c);
        for (int r = 0; r < bh; r++) {
            const int from_bottom = bh - 1 - r;
            const int lit = ticks - from_bottom * 2;
            const char ch = lit >= 2 ? '|' : lit == 1 ? ':' : from_bottom == 0 ? '.' : ' ';
            lap_put_cell(cx, y + 1 + r, ch, lit > 0 ? ink : LAP_UI_FAINT);
            if (cw == 3 && lit > 0) lap_put_cell(cx + 1, y + 1 + r, ch, ink);
        }
        if (live && c == s.e.ch) {
            lap_put_cell(cx, y + 1 + bh, '^', TRIAL_A(LAP_GREEN | LAP_BRIGHT));
            if (cw == 3) lap_put_cell(cx + 1, y + 1 + bh, '^', TRIAL_A(LAP_GREEN | LAP_BRIGHT));
        } else lap_put_cell(cx, y + 1 + bh, '-', TRIAL_A(LAP_CYAN));
        if (cw >= 3 || c % 3 == 0) {
            char n[4];
            snprintf(n, sizeof(n), "%d", c + 1);
            lap_put_str(cx, y + 2 + bh, n, c == s.e.ch && live ? LAP_UI_TEXT : LAP_UI_DIM);
        }
        if (cw == 3 && s.e.ch_count[c]) {
            char p[5];
            snprintf(p, sizeof(p), "%.0f", (double)-s.e.ch_peak[c]);
            lap_put_str(cx, y + 3 + bh, p, ink);
        }
    }
    /* The scale: the busiest channel's count at the top. */
    char m[12];
    snprintf(m, sizeof(m), "%lu", (unsigned long)most);
    lap_put_str(gx + gw + 2, y + 1, m, most ? LAP_UI_TEXT : LAP_UI_FAINT);
    lap_put_str(gx + gw + 2, y + bh, "0", LAP_UI_FAINT);
    lap_put_str(gx + gw + 2, y + 1 + bh, "now", live ? TRIAL_A(LAP_GREEN | LAP_BRIGHT) : LAP_UI_FAINT);
    if (cw == 3) lap_put_str(gx + gw + 2, y + 3 + bh, "-dBFS", LAP_UI_FAINT);
}

/* Bursts a second over the last minute, two seconds a cell in braille;
   seconds before START are left blank. */
static void minute_view(int x, int y, int w)
{
    const uint32_t s_now = (uint32_t)((s.e.t_now - s.e.t_start) / 1000000);
    float lv[60];
    float max = 1;
    for (int k = 0; k < 60; k++) {
        const uint32_t back = (uint32_t)(59 - k);
        if (s_now < back) { lv[k] = -200.0f; continue; }
        const uint32_t sec = s_now - back;
        lv[k] = s.e.sec_at[sec % 60] == sec ? (float)s.e.sec_n[sec % 60] : 0.0f;
        if (lv[k] > max) max = lv[k];
    }
    char right[24];
    snprintf(right, sizeof(right), "peak %.0f/s", (double)max);
    trial_rule(x, y, w, "LAST MINUTE", s.e.log.count ? right : "bursts a second", LAP_WHITE | LAP_BRIGHT);
    const int cells = w - 6 < 30 ? w - 6 : 30;
    const int gx = x + (w - 6 - cells);
    lap_hline(gx, y + 2, cells, '.', LAP_UI_FAINT);
    trial_braille(gx, y + 1, cells, 2, lv + 60 - cells * 2, cells * 2, 0, max, max + 1,
                  TRIAL_A(LAP_GREEN | LAP_BRIGHT), TRIAL_A(LAP_GREEN | LAP_BRIGHT), false);
    lap_put_str(gx + cells + 1, y + 2, "now", LAP_UI_DIM);
    lap_put_str(gx - 5 > x ? gx - 5 : x, y + 2, "-60s", LAP_UI_FAINT);
}

/* The strongest channel each cycle, newest on top: over a pass it walks
   across the plan with the Doppler shift. */
static void trace_view(int x, int y, int w, int h)
{
    const int nch = s.e.plan.ch, cw = ch_w(w, nch), gw = cw * nch;
    trial_rule(x, y, w, "DOPPLER TRACE", "time  dBFS", LAP_WHITE | LAP_BRIGHT);
    const int gx = x + (w - 10 - gw) / 2;
    if (!s.e.trace_n) {
        for (int c = 0; c < nch; c++) lap_put_cell(gx + c * cw + (cw > 2 ? 1 : 0), y + 1, '.', LAP_UI_FAINT);
        lap_put_str(x, y + 3, "the strongest channel of each cycle", LAP_UI_DIM);
        lap_put_str(x, y + 4, "lands here; a pass walks across", LAP_UI_DIM);
        return;
    }
    for (int k = 0; k < h - 1 && k < s.e.trace_n; k++) {
        const irid_trace_t *t = &s.e.trace[(s.e.trace_head - 1 - k + 2 * IRID_TRACE_N) % IRID_TRACE_N];
        const float floor_db = s.e.det[t->ch].seeded ? s.e.det[t->ch].floor_db : t->db - 10;
        uint8_t ink = trial_ramp(t->db - floor_db);
        if (k >= 6) ink &= (uint8_t)~LAP_BRIGHT;
        for (int c = 0; c < nch; c++) {
            const int cx = gx + c * cw + (cw > 2 ? 1 : 0);
            if (c == t->ch) {
                lap_put_cell(cx, y + 1 + k, '*', ink);
                if (cw == 3) lap_put_cell(cx + 1, y + 1 + k, '*', ink);
            } else lap_put_cell(cx, y + 1 + k, '.', LAP_UI_FAINT);
        }
        char l[20], clock[12];
        burst_fmt_clock(t->t_s, clock, sizeof(clock));
        snprintf(l, sizeof(l), "%5s %4.0f", clock, (double)t->db);
        lap_put_str(gx + gw + 1, y + 1 + k, l, k ? LAP_UI_DIM : LAP_UI_TEXT);
    }
}

/* Before the first run: a satellite over the horizon (ten rows), and what
   it needs. */
static void idle_art(int x, int y, int w)
{
    static const char *const ART[] = {
        "             .-.             ",
        "   [=====]--( o )--[=====]   ",
        "             '-'             ",
        "            / | \\            ",
        "           /  |  \\           ",
        "          '   '   '          ",
        "      .                 .    ",
        " __/\\___/\\/\\________/\\_____  ",
    };
    static const uint8_t INK[] = { TRIAL_A(LAP_WHITE | LAP_BRIGHT), TRIAL_A(LAP_CYAN | LAP_BRIGHT),
                                   TRIAL_A(LAP_WHITE | LAP_BRIGHT), TRIAL_A(LAP_YELLOW), TRIAL_A(LAP_YELLOW),
                                   TRIAL_A(LAP_YELLOW | LAP_BRIGHT), LAP_UI_FAINT, TRIAL_A(LAP_GREEN) };
    trial_rule(x, y, w, "OPEN SKY", NULL, LAP_WHITE | LAP_BRIGHT);
    const int aw = (int)strlen(ART[0]);
    trial_art(x + (w > aw ? (w - aw) / 2 : 0), y + 2, w, ART, 8, INK);
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
        "START hops the plan a channel at a time and counts the satellites' 8 ms TDMA bursts on each.",
        LAP_UI_DIM);
    if (yy + 1 < y + h) trial_wrap(x, yy + 1, w, y + h - yy - 1,
        "OPTIONS: PLAN (the simplex channels or the whole band), DWELL and THRESHOLD.", LAP_UI_FAINT);
}

static void draw(void)
{
    lap_clear(LAP_UI_DIM);
    trial_refresh(&s.t);
    pump();
    watch_channels();
    if (!s.view_ms || lap_millis() - s.view_ms >= VIEW_MS) {
        if (s.ran)
            s.n = irid_lines(&s.e, s.t.lines, TRIAL_LINES);
        else
            s.n = irid_idle_lines(s.plan, s.dwell_ms, s.thresh_db, s.t.lines, TRIAL_LINES);
        s.view_ms = lap_millis() ? lap_millis() : 1;
    }
    const trial_rect_t in = trial_frame(&s.t);
    s.room = TRIAL_LINES;           /* NOTE saves the whole readout */
    if (!in.h) return;
    const bool live = s.t.on && s.t.run_ms;
    trial_flow_t f;
    trial_flow_init(&f, in, 37);
    int x, y, w, h;
    if (trial_flow_take(&f, 5, &x, &y, &w)) head_view(x, y, w, live);
    if (!s.ran || !s.e.reads) {
        if (f.cols > 1) {
            if (trial_flow_rest(&f, 4, &x, &y, &w, &h)) idle_text(x, y, w, h);
            if (trial_flow_take(&f, 10, &x, &y, &w)) idle_art(x, y, w);
        } else {
            if (trial_flow_take(&f, 10, &x, &y, &w)) idle_art(x, y, w);
            if (trial_flow_rest(&f, 4, &x, &y, &w, &h)) idle_text(x, y, w, h);
        }
        return;
    }
    if (trial_flow_take(&f, 4, &x, &y, &w)) stats_view(x, y, w);
    if (f.cols > 1) {
        /* Landscape: the totals and the minute, the channels, the trace. */
        if (trial_flow_take(&f, 3, &x, &y, &w)) minute_view(x, y, w);
        f.y = f.r.y + f.r.h;
        if (trial_flow_rest(&f, 8, &x, &y, &w, &h)) channels_view(x, y, w, h, live);
    } else {
        if (trial_flow_take(&f, 13, &x, &y, &w)) channels_view(x, y, w, 13, live);
        if (trial_flow_take(&f, 3, &x, &y, &w)) minute_view(x, y, w);
    }
    if (trial_flow_rest(&f, 4, &x, &y, &w, &h)) trace_view(x, y, w, h);
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

LAP_APP(.id = "iridium", .name = "IRIDIUM BURSTS", .sub = "satellite bursts", .version = "1.1",
        .author = "LakeShark", .icon = LAP_ICON_STAR, .color = LAP_CYAN, .flags = LAP_CAP_RADIO_RX,
        .icon_art = { " [=]o[=]  ", "    |     ", "   / \\    ", "  /   \\   ", "__________", " IRIDIUM  " },
        .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch);
