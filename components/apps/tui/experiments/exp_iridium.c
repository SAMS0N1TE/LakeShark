/* IRIDIUM BURSTS: Iridium's downlink on the LR2021's HF input. The
   satellites send in 90 ms TDMA frames, and the simplex channels at
   1626.0-1626.5 MHz (the ring alert among them) carry bursts a few
   milliseconds long that a receiver anywhere under the satellite can see.
   This listens on one channel for a frame or more at a time, hopping across
   the plan, times the bursts the way BURST SCOPE does, and keeps a trace of
   which channel was strongest each cycle: over a pass that wanders by tens
   of kilohertz with the Doppler shift. Energy and timing only; nothing is
   demodulated. */
#include "../ls_experiments.h"
#include "exp_rf.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "ls_lora.h"

extern const ls_experiment_t exp_iridium;

#define POLL_BUDGET_US 15000
#define POLL_MAX_READS 4096
#define VIEW_EVERY_US  250000
#define DRAIN_EVERY_US 100000
#define CH_MAX         24
#define TRACE_N        24
#define TEXT_LINES     16
/* An Iridium burst is at most a slot long; a level held for longer is
   something else, and becomes the floor. */
#define BURST_MAX_US   100000

typedef struct {
    const char *name;
    uint32_t lo_hz, hi_hz;
    int ch;
} plan_t;

/* The simplex band in channels near the 41.667 kHz Iridium raster, and the
   whole downlink band in 500 kHz steps. */
static const plan_t PLAN[] = {
    { "SIMPLEX", 1626000000u, 1626500000u, 12 },
    { "WHOLE BAND", 1616000000u, 1626500000u, 21 },
};
#define PLAN_N ((int)(sizeof(PLAN) / sizeof(PLAN[0])))
static const char *const PLAN_NAME[] = { "SIMPLEX 1626.0-1626.5", "WHOLE BAND 1616-1626.5" };

static volatile int s_plan;
static volatile int s_dwell_ms = 90;
static volatile int s_thresh_db = 8;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static EXT_RAM_BSS_ATTR char s_text[TEXT_LINES][LS_EXP_LINE];
static int s_text_n;

typedef struct { uint32_t t_s; int ch; float dbm; } trace_t;

static EXT_RAM_BSS_ATTR struct {
    plan_t   plan;
    uint32_t spacing, bw, settle_us;
    int      ch;
    int64_t  t_start, t_dwell, t_listen, t_drain, t_view;
    exp_burst_det_t det[CH_MAX];
    exp_burst_log_t log;
    uint32_t ch_count[CH_MAX];
    float    ch_peak[CH_MAX];
    /* The strongest burst this cycle, and the cycles kept. */
    int      cyc_ch;
    float    cyc_dbm;
    uint32_t cycles;
    trace_t  trace[TRACE_N];
    int      trace_n, trace_head;
    /* Bursts in each of the last 60 seconds. */
    uint32_t sec_n[60], sec_at[60];
    uint64_t reads, listen_us;
    uint32_t errors, hops;
    uint32_t spurs;          /* bursts too short or too long to be an Iridium slot */
} w;

/* An Iridium TDMA burst fills most of an 8.28 ms slot; anything well
   outside that is a spur or a carrier, counted apart and never shown as a
   satellite. */
#define SLOT_MIN_US  4000u
#define SLOT_MAX_US 12000u

static uint32_t ch_hz(int ch)
{
    return w.plan.lo_hz + w.spacing / 2 + w.spacing * (uint32_t)ch;
}

static void note_burst(int ch, const exp_burst_t *b, int64_t now)
{
    if (b->len_us < SLOT_MIN_US || b->len_us > SLOT_MAX_US) { w.spurs++; return; }
    exp_burst_log_add(&w.log, b);
    w.ch_count[ch]++;
    if (b->peak_dbm > w.ch_peak[ch]) w.ch_peak[ch] = b->peak_dbm;
    if (w.cyc_ch < 0 || b->peak_dbm > w.cyc_dbm) { w.cyc_ch = ch; w.cyc_dbm = b->peak_dbm; }
    const uint32_t s = (uint32_t)((now - w.t_start) / 1000000);
    if (w.sec_at[s % 60] != s) { w.sec_at[s % 60] = s; w.sec_n[s % 60] = 0; }
    w.sec_n[s % 60]++;
}

static void end_cycle(int64_t now)
{
    w.cycles++;
    if (w.cyc_ch >= 0) {
        trace_t *t = &w.trace[w.trace_head];
        t->t_s = (uint32_t)((now - w.t_start) / 1000000);
        t->ch = w.cyc_ch;
        t->dbm = w.cyc_dbm;
        w.trace_head = (w.trace_head + 1) % TRACE_N;
        if (w.trace_n < TRACE_N) w.trace_n++;
    }
    w.cyc_ch = -1;
}

static void view(int64_t now)
{
    static EXT_RAM_BSS_ATTR char t[TEXT_LINES][LS_EXP_LINE];
    int n = 0;
    const int nch = w.plan.ch;
    snprintf(t[n++], LS_EXP_LINE, "%s %.1f-%.1f MHz, %d x %.1f kHz", w.plan.name,
             w.plan.lo_hz / 1e6, w.plan.hi_hz / 1e6, nch, w.spacing / 1e3);
    const double listened = w.listen_us / 1e6;
    snprintf(t[n++], LS_EXP_LINE, "DWELL %d ms  CYCLE %.1f s  +%d dB  %.1fk/s",
             s_dwell_ms, nch * s_dwell_ms / 1000.0, s_thresh_db,
             listened > 0 ? w.reads / listened / 1e3 : 0.0);
    const uint32_t s_now = (uint32_t)((now - w.t_start) / 1000000);
    uint32_t minute = 0;
    for (int i = 0; i < 60; i++)
        if (w.sec_at[i] + 60 > s_now && w.sec_at[i] <= s_now) minute += w.sec_n[i];
    if (w.log.count)
        snprintf(t[n++], LS_EXP_LINE, "BURSTS %lu  LAST MIN %lu  PEAK %.0f dBm",
                 (unsigned long)w.log.count, (unsigned long)minute, (double)w.log.peak_dbm);
    else snprintf(t[n++], LS_EXP_LINE, "BURSTS 0 in %lu cycles  errors %lu",
                  (unsigned long)w.cycles, (unsigned long)w.errors);
    if (w.spurs)
        snprintf(t[n++], LS_EXP_LINE, "SPURS %lu (not slot length, not counted)",
                 (unsigned long)w.spurs);
    int best = -1;
    uint32_t most = 0;
    for (int c = 0; c < nch; c++) if (w.ch_count[c] > most) { most = w.ch_count[c]; best = c; }
    if (best >= 0)
        snprintf(t[n++], LS_EXP_LINE, "BEST %.3f MHz  %lu bursts  %.0f dBm",
                 ch_hz(best) / 1e6, (unsigned long)most, (double)w.ch_peak[best]);
    /* Bursts per channel, darker to brighter. */
    static const char LEVEL[] = " .:-=+*#%@";
    char bar[CH_MAX + 1];
    for (int c = 0; c < nch; c++)
        bar[c] = most ? LEVEL[(w.ch_count[c] * 9 + most - 1) / most] : '.';
    bar[nch] = 0;
    char floor_s[8] = "--";
    if (w.det[w.ch].seeded) snprintf(floor_s, sizeof(floor_s), "%.0f", (double)w.det[w.ch].floor_dbm);
    snprintf(t[n++], LS_EXP_LINE, "CH [%s] floor %s", bar, floor_s);
    char mode[12];
    exp_burst_rhythm_t r;
    exp_burst_rhythm(&w.log, &r);
    exp_fmt_us(r.median_len_us, mode, sizeof(mode));
    if (w.log.count) snprintf(t[n++], LS_EXP_LINE, "LEN ~%s  (Iridium slots are ~8 ms)", mode);
    snprintf(t[n++], LS_EXP_LINE, "TRACE  time       MHz   dBm  channel");
    for (int k = 0; k < w.trace_n && n < TEXT_LINES; k++) {
        const trace_t *x = &w.trace[(w.trace_head - 1 - k + 2 * TRACE_N) % TRACE_N];
        char clock[10], where[CH_MAX + 1];
        exp_fmt_clock(x->t_s, clock, sizeof(clock));
        for (int c = 0; c < nch; c++) where[c] = c == x->ch ? '*' : '.';
        where[nch] = 0;
        snprintf(t[n++], LS_EXP_LINE, "%6s %9.3f %5.0f  %s", clock, ch_hz(x->ch) / 1e6,
                 (double)x->dbm, where);
    }
    if (!w.trace_n && n < TEXT_LINES) snprintf(t[n++], LS_EXP_LINE, "  no burst yet");
    portENTER_CRITICAL(&s_lock);
    memcpy(s_text, t, sizeof(s_text));
    s_text_n = n;
    portEXIT_CRITICAL(&s_lock);
}

static bool iridium_start(char *why, size_t n)
{
    /* The last run's readout goes; until this one has its own, lines()
       says what it will do. */
    portENTER_CRITICAL(&s_lock);
    s_text_n = 0;
    portEXIT_CRITICAL(&s_lock);
    const uint32_t caps = ls_lora_caps();
    if (!(caps & LS_LORA_CAP_FSK) || !(caps & LS_LORA_CAP_RSSI_INST)) {
        snprintf(why, n, "This chip gives no FSK receiver or RSSI reading");
        return false;
    }
    int p = s_plan;
    if (p < 0 || p >= PLAN_N) p = 0;
    if (!ls_lora_rx_range_ok(caps, PLAN[p].lo_hz, PLAN[p].hi_hz)) {
        snprintf(why, n, "This radio has no 1.5-2.5 GHz input");
        return false;
    }
    memset(&w, 0, sizeof(w));
    w.plan = PLAN[p];
    w.spacing = (w.plan.hi_hz - w.plan.lo_hz) / (uint32_t)w.plan.ch;
    w.bw = ls_lora_fsk_bw_snap(w.spacing);
    w.settle_us = exp_settle_us(w.bw);
    uint32_t rate = w.bw / 4, dev = w.bw / 8;
    if (rate < 600) rate = 600;
    if (dev < 600) dev = 600;
    const ls_fsk_cfg_t cfg = {
        .freq_hz = ch_hz(0), .bitrate = rate, .deviation_hz = dev, .bandwidth_hz = w.bw,
        .sync_word = 0x9D3B6E15u, .payload_bytes = 8,
    };
    const esp_err_t err = ls_lora_fsk_begin(&cfg);
    if (err != ESP_OK) {
        snprintf(why, n, "%.3f MHz refused: %s", cfg.freq_hz / 1e6, esp_err_to_name(err));
        return false;
    }
    for (int c = 0; c < w.plan.ch; c++) {
        exp_burst_det_init(&w.det[c], (float)s_thresh_db);
        w.det[c].max_len_us = BURST_MAX_US;
        w.ch_peak[c] = -200.0f;
    }
    exp_burst_log_reset(&w.log);
    w.cyc_ch = -1;
    const int64_t now = esp_timer_get_time();
    w.t_start = w.t_dwell = w.t_drain = w.t_view = now;
    w.t_listen = now + w.settle_us;
    view(now);
    return true;
}

static void iridium_stop(void) { ls_lora_fsk_end(); }

static void hop(int64_t now)
{
    exp_burst_t b;
    if (exp_burst_det_flush(&w.det[w.ch], &b)) note_burst(w.ch, &b, now);
    w.ch = (w.ch + 1) % w.plan.ch;
    if (w.ch == 0) end_cycle(now);
    w.hops++;
    if (ls_lora_fsk_retune(ch_hz(w.ch)) != ESP_OK) w.errors++;
    w.t_dwell = now;
    w.t_listen = now + w.settle_us;
}

static void iridium_poll(void)
{
    int64_t now = esp_timer_get_time();
    if (now - w.t_dwell >= (int64_t)s_dwell_ms * 1000) hop(now);
    if (now >= w.t_listen) {
        const int64_t t0 = now, until = w.t_dwell + (int64_t)s_dwell_ms * 1000;
        for (int k = 0; k < POLL_MAX_READS; k++) {
            float dbm = 0;
            const esp_err_t err = ls_lora_rssi_inst(&dbm);
            now = esp_timer_get_time();
            if (err != ESP_OK) {
                w.errors++;
                if (err == ESP_ERR_INVALID_STATE) ls_lora_fsk_receive();
                break;
            }
            w.reads++;
            exp_burst_t b;
            if (exp_burst_det_feed(&w.det[w.ch], now, dbm, &b)) note_burst(w.ch, &b, now);
            if (now - t0 >= POLL_BUDGET_US || now >= until) break;
        }
        w.listen_us += (uint64_t)(now - t0);
    }
    if (now - w.t_drain >= DRAIN_EVERY_US) {
        uint8_t buf[8];
        float rssi;
        ls_lora_fsk_poll(buf, sizeof(buf), &rssi);
        w.t_drain = now;
    }
    if (now - w.t_view >= VIEW_EVERY_US) {
        view(now);
        w.t_view = now;
    }
}

static int iridium_lines(char (*out)[LS_EXP_LINE], int max)
{
    int n;
    portENTER_CRITICAL(&s_lock);
    n = s_text_n < max ? s_text_n : max;
    if (n > 0) memcpy(out, s_text, (size_t)n * LS_EXP_LINE);
    portEXIT_CRITICAL(&s_lock);
    if (n > 0) return n;
    int p = s_plan;
    if (p < 0 || p >= PLAN_N) p = 0;
    n = 0;
    if (n < max) snprintf(out[n++], LS_EXP_LINE, "%s %.1f-%.1f MHz in %d channels", PLAN[p].name,
                          PLAN[p].lo_hz / 1e6, PLAN[p].hi_hz / 1e6, PLAN[p].ch);
    if (n < max) snprintf(out[n++], LS_EXP_LINE, "%d ms on each, bursts +%d dB over the floor",
                          s_dwell_ms, s_thresh_db);
    return n;
}

static void restart(void) { if (ls_exp_running() == &exp_iridium) ls_exp_start(&exp_iridium); }

static int o_plan(const ls_opt_t *o) { (void)o; return s_plan; }
static void o_set_plan(const ls_opt_t *o, int v) { (void)o; s_plan = v >= 0 && v < PLAN_N ? v : 0; restart(); }
static double o_num(const ls_opt_t *o) { return o->arg ? s_thresh_db : s_dwell_ms; }
static void o_set_num(const ls_opt_t *o, double v)
{
    if (o->arg) s_thresh_db = (int)(v + 0.5);
    else s_dwell_ms = (int)(v + 0.5);
    restart();
}
static void o_show(const ls_opt_t *o, char *out, size_t n)
{
    if (o->arg) snprintf(out, n, "+%d dB", s_thresh_db);
    else snprintf(out, n, "%d ms", s_dwell_ms);
}

static const ls_opt_t OPTS[] = {
    { .label = "PLAN", .kind = LS_OPT_CYCLE, .names = PLAN_NAME, .n = PLAN_N,
      .get = o_plan, .set = o_set_plan },
    { .label = "DWELL", .kind = LS_OPT_NUMBER, .arg = 0, .num = o_num, .set_num = o_set_num,
      .lo = 20, .hi = 1000, .unit = "ms on each channel; a frame is 90", .show = o_show },
    { .label = "THRESHOLD", .kind = LS_OPT_NUMBER, .arg = 1, .num = o_num, .set_num = o_set_num,
      .lo = 3, .hi = 30, .unit = "dB over the floor", .show = o_show },
};

const ls_experiment_t exp_iridium = {
    .id = "iridium",
    .name = "IRIDIUM BURSTS",
    .sub = "Satellite bursts at 1616-1626.5 MHz",
    .maturity = LS_EXP_TRYING,
    .lr2021_only = true,
    .needs = "open sky; the HF input is matched for 2.4 GHz, so only strong bursts show",
    .start = iridium_start,
    .stop = iridium_stop,
    .poll = iridium_poll,
    .lines = iridium_lines,
    .opts = OPTS,
    .n_opts = (int)(sizeof(OPTS) / sizeof(OPTS[0])),
};
