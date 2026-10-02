/* BURST SCOPE: how one frequency is used in time. The LoRa chip sits in a
   receive session there (FSK only to tune and hold it; the sync word is one
   nothing sends) and its instantaneous RSSI is read as fast as the bus
   allows. Readings over the floor by the threshold are bursts; each one's
   length, the gap to the next and the rhythm they keep are what tell a key
   fob from a tyre sensor from a toll reader. Energy and timing only:
   nothing is decoded and nothing is sent. */
#include "../ls_experiments.h"
#include "exp_rf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "ls_lora.h"

extern const ls_experiment_t exp_burst;

/* One poll reads for this long, or this many times, whichever ends first;
   the worker's 2 ms sleep between polls is the only hole, well inside the
   detector's 5 ms gap. */
#define POLL_BUDGET_US 15000
#define POLL_MAX_READS 4096
#define VIEW_EVERY_US  250000
#define DRAIN_EVERY_US 100000
#define STRIP_CELLS    36
#define STRIP_SPAN_US  2000000
#define STRIP_SPAN_MIN_US 50000
#define STRIP_SPAN_MAX_US 10000000
#define TEXT_LINES     12

/* What the next start uses: one word each, so the console and OPTIONS can
   set them while the worker reads. */
static volatile uint32_t s_hz = 433920000u;
static volatile uint32_t s_bw_hz = 250000u;
static volatile int s_thresh_db = 10;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
/* The readout, formatted by the worker a few times a second. */
static EXT_RAM_BSS_ATTR char s_text[TEXT_LINES][LS_EXP_LINE];
static int s_text_n;

/* The worker's own. */
static EXT_RAM_BSS_ATTR struct {
    exp_burst_det_t det;
    exp_burst_log_t log;
    uint32_t hz, bw;
    int64_t  t_start, t_drain, t_view;
    uint64_t reads, read_us;
    uint32_t errors;
    float    now_dbm;
    bool     have_now;
} w;

static void view(int64_t now)
{
    static EXT_RAM_BSS_ATTR char t[TEXT_LINES][LS_EXP_LINE];
    int n = 0;
    const double secs = (now - w.t_start) / 1e6;
    snprintf(t[n++], LS_EXP_LINE, "FREQ %.4f MHz  FILTER %.1f kHz  +%.0f dB",
             w.hz / 1e6, w.bw / 1e3, (double)w.det.thresh_db);
    if (w.reads)
        snprintf(t[n++], LS_EXP_LINE, "RATE %.1fk/s, %luus a read  FLOOR %.0f dBm",
                 secs > 0 ? w.reads / secs / 1e3 : 0.0,
                 (unsigned long)(w.read_us / w.reads), (double)w.det.floor_dbm);
    else snprintf(t[n++], LS_EXP_LINE, "RATE --  FLOOR --");
    if (w.have_now)
        snprintf(t[n++], LS_EXP_LINE, "NOW %.0f dBm  BURSTS %lu  PEAK %.0f dBm", (double)w.now_dbm,
                 (unsigned long)w.log.count, w.log.count ? (double)w.log.peak_dbm : 0.0);
    else snprintf(t[n++], LS_EXP_LINE, "NOW no reading yet  errors %lu", (unsigned long)w.errors);
    /* The strip spans eight periods once there is a rhythm, so each burst
       and each gap gets cells of its own; two seconds until then. What the
       ring no longer holds is blank, not quiet. */
    exp_burst_rhythm_t r;
    exp_burst_rhythm(&w.log, &r);
    int64_t span = STRIP_SPAN_US;
    if (r.mode_us) {
        span = (int64_t)r.mode_us * 8;
        if (span < STRIP_SPAN_MIN_US) span = STRIP_SPAN_MIN_US;
        if (span > STRIP_SPAN_MAX_US) span = STRIP_SPAN_MAX_US;
    }
    int64_t since = w.t_start;
    if (w.log.n == EXP_BURST_RING) {
        const int64_t oldest = exp_burst_log_at(&w.log, w.log.n - 1)->start_us;
        if (oldest > since) since = oldest;
    }
    char strip[STRIP_CELLS + 1], span_s[12];
    exp_burst_strip(&w.log, now, span, since, strip, STRIP_CELLS);
    exp_fmt_us((uint32_t)span, span_s, sizeof(span_s));
    snprintf(t[n++], LS_EXP_LINE, "[%s] %s", strip, span_s);

    uint32_t len[EXP_LEN_BUCKETS], gap[EXP_GAP_BUCKETS];
    exp_burst_hist(&w.log, len, gap);
    char a[LS_EXP_LINE], b[LS_EXP_LINE];
    int pa = snprintf(a, sizeof(a), "LEN "), pb = snprintf(b, sizeof(b), "    ");
    for (int i = 0; i < EXP_LEN_BUCKETS; i++) {
        pa += snprintf(a + pa, sizeof(a) - pa, "%4s", EXP_LEN_LABEL[i]);
        pb += snprintf(b + pb, sizeof(b) - pb, "%4lu", (unsigned long)len[i]);
    }
    snprintf(t[n++], LS_EXP_LINE, "%s", a);
    snprintf(t[n++], LS_EXP_LINE, "%s", b);
    pa = snprintf(a, sizeof(a), "GAP ");
    pb = snprintf(b, sizeof(b), "    ");
    for (int i = 0; i < EXP_GAP_BUCKETS; i++) {
        pa += snprintf(a + pa, sizeof(a) - pa, "%4s", EXP_GAP_LABEL[i]);
        pb += snprintf(b + pb, sizeof(b) - pb, "%4lu", (unsigned long)gap[i]);
    }
    snprintf(t[n++], LS_EXP_LINE, "%s", a);
    snprintf(t[n++], LS_EXP_LINE, "%s", b);

    char mode[12], med[12];
    exp_fmt_us(r.mode_us, mode, sizeof(mode));
    exp_fmt_us(r.median_len_us, med, sizeof(med));
    if (r.mode_us)
        snprintf(t[n++], LS_EXP_LINE, "EVERY %s: %d of %d gaps fit, len ~%s", mode,
                 r.period_hits, r.intervals, med);
    else snprintf(t[n++], LS_EXP_LINE, "EVERY -- (too few bursts yet)");

    /* The newest, three to a line. */
    for (int row = 0; row < 2 && n < TEXT_LINES; row++) {
        int p = snprintf(a, sizeof(a), row ? "     " : "LAST ");
        int shown = 0;
        for (int k = row * 3; k < row * 3 + 3; k++) {
            const exp_burst_t *x = exp_burst_log_at(&w.log, k);
            if (!x) break;
            char l[12];
            exp_fmt_us(x->len_us, l, sizeof(l));
            p += snprintf(a + p, sizeof(a) - p, "%s%s %.0f%s", shown ? " | " : "", l,
                          (double)x->peak_dbm, x->cut ? "~" : "");
            shown++;
        }
        if (!shown && row) break;
        if (!shown) snprintf(a + p, sizeof(a) - p, "none yet");
        snprintf(t[n++], LS_EXP_LINE, "%s", a);
    }

    portENTER_CRITICAL(&s_lock);
    memcpy(s_text, t, sizeof(s_text));
    s_text_n = n;
    portEXIT_CRITICAL(&s_lock);
}

static bool burst_start(char *why, size_t n)
{
    /* The last run's readout goes; until this one has its own, lines()
       says what it will do. */
    portENTER_CRITICAL(&s_lock);
    s_text_n = 0;
    portEXIT_CRITICAL(&s_lock);
    const uint32_t caps = ls_lora_caps();
    if (!(caps & LS_LORA_CAP_FSK)) { snprintf(why, n, "No FSK receiver on this chip"); return false; }
    if (!(caps & LS_LORA_CAP_RSSI_INST)) { snprintf(why, n, "This chip gives no RSSI reading"); return false; }
    const uint32_t hz = s_hz;
    if (!ls_lora_rx_range_ok(caps, hz, hz)) {
        snprintf(why, n, "%.4f MHz is out of this radio's reach", hz / 1e6);
        return false;
    }
    /* Nothing is demodulated: a rate and deviation the filter takes. */
    const uint32_t bw = ls_lora_fsk_bw_snap(s_bw_hz);
    uint32_t rate = bw / 4, dev = bw / 8;
    if (rate < 600) rate = 600;
    if (dev < 600) dev = 600;
    const ls_fsk_cfg_t cfg = {
        .freq_hz = hz, .bitrate = rate, .deviation_hz = dev, .bandwidth_hz = bw,
        .sync_word = 0x9D3B6E15u, .payload_bytes = 8,
    };
    const esp_err_t err = ls_lora_fsk_begin(&cfg);
    if (err != ESP_OK) {
        snprintf(why, n, "%.4f MHz refused: %s", hz / 1e6, esp_err_to_name(err));
        return false;
    }
    memset(&w, 0, sizeof(w));
    exp_burst_det_init(&w.det, (float)s_thresh_db);
    exp_burst_log_reset(&w.log);
    w.hz = hz;
    w.bw = bw;
    w.t_start = w.t_drain = w.t_view = esp_timer_get_time();
    view(w.t_start);
    return true;
}

static void burst_stop(void) { ls_lora_fsk_end(); }

static void burst_poll(void)
{
    const int64_t t0 = esp_timer_get_time();
    int64_t now = t0;
    for (int k = 0; k < POLL_MAX_READS; k++) {
        float dbm = 0;
        const int64_t before = now;
        const esp_err_t err = ls_lora_rssi_inst(&dbm);
        now = esp_timer_get_time();
        if (err != ESP_OK) {
            w.errors++;
            /* Out of receive: listen again, and look next poll. */
            if (err == ESP_ERR_INVALID_STATE) ls_lora_fsk_receive();
            break;
        }
        w.reads++;
        w.read_us += (uint64_t)(now - before);
        w.now_dbm = dbm;
        w.have_now = true;
        exp_burst_t b;
        if (exp_burst_det_feed(&w.det, now, dbm, &b)) exp_burst_log_add(&w.log, &b);
        if (now - t0 >= POLL_BUDGET_US) break;
    }
    /* Anything the demodulator took is read and dropped, so a full FIFO
       never stops the receiver. */
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

static int burst_lines(char (*out)[LS_EXP_LINE], int max)
{
    int n;
    portENTER_CRITICAL(&s_lock);
    n = s_text_n < max ? s_text_n : max;
    if (n > 0) memcpy(out, s_text, (size_t)n * LS_EXP_LINE);
    portEXIT_CRITICAL(&s_lock);
    if (n > 0) return n;
    /* Never run: what the next start will do. */
    n = 0;
    if (n < max) snprintf(out[n++], LS_EXP_LINE, "FREQ %.4f MHz  FILTER %.1f kHz  +%d dB",
                          s_hz / 1e6, s_bw_hz / 1e3, s_thresh_db);
    if (n < max) snprintf(out[n++], LS_EXP_LINE, "Times every burst over the floor there.");
    return n;
}

static bool parse_num(const char *text, double lo, double hi, double *v)
{
    char *end = NULL;
    const double x = strtod(text, &end);
    if (end == text || *end || !(x >= lo && x <= hi)) return false;
    *v = x;
    return true;
}

static bool burst_configure(int argc, char **argv, char *why, size_t n)
{
    double mhz, khz = s_bw_hz / 1e3;
    const uint32_t caps = ls_lora_caps();
    if (argc < 1 || argc > 2 || !parse_num(argv[0], 150, 2500, &mhz) ||
        !ls_lora_rx_range_ok(caps, (uint32_t)(mhz * 1e6 + 0.5), (uint32_t)(mhz * 1e6 + 0.5)) ||
        (argc == 2 && !parse_num(argv[1], 3.4, 3077, &khz))) {
        snprintf(why, n, "<MHz> [filter kHz 3.5-3077], %s",
                 caps & LS_LORA_CAP_BAND_1G5_2G5 ? "150-1100 or 1500-2500 MHz"
                 : caps & LS_LORA_CAP_RX_WIDE ? "150-1100 MHz" : "150-960 MHz");
        return false;
    }
    s_hz = (uint32_t)(mhz * 1e6 + 0.5);
    s_bw_hz = (uint32_t)(khz * 1e3 + 0.5);
    return true;
}

/* OPTIONS. Each change restarts a running scope with it. */
static void restart(void) { if (ls_exp_running() == &exp_burst) ls_exp_start(&exp_burst); }

/* Places worth timing: fobs and tyre sensors, and the five E-ZPass reader
   channels. The last name is whatever was typed. */
static const double PRESET_MHZ[] = { 315.0, 433.92, 911.5, 914.0, 915.75, 916.5, 919.0 };
static const char *const PRESET_NAME[] = {
    "315 fob/TPMS", "433.92 fob/TPMS", "911.50 toll", "914.00 toll", "915.75 toll",
    "916.50 toll", "919.00 toll", "typed",
};
#define PRESET_N ((int)(sizeof(PRESET_MHZ) / sizeof(PRESET_MHZ[0])))

static int o_preset(const ls_opt_t *o)
{
    (void)o;
    for (int i = 0; i < PRESET_N; i++)
        if ((uint32_t)(PRESET_MHZ[i] * 1e6 + 0.5) == s_hz) return i;
    return PRESET_N;
}
static void o_set_preset(const ls_opt_t *o, int v)
{
    (void)o;
    if (v < 0 || v >= PRESET_N) v = 0;
    s_hz = (uint32_t)(PRESET_MHZ[v] * 1e6 + 0.5);
    restart();
}

static double o_num(const ls_opt_t *o)
{
    switch (o->arg) {
    case 0: return s_hz / 1e6;
    case 1: return s_bw_hz / 1e3;
    default: return s_thresh_db;
    }
}
static void o_set_num(const ls_opt_t *o, double v)
{
    switch (o->arg) {
    case 0: s_hz = (uint32_t)(v * 1e6 + 0.5); break;
    case 1: s_bw_hz = (uint32_t)(v * 1e3 + 0.5); break;
    default: s_thresh_db = (int)(v + 0.5); break;
    }
    restart();
}
static void o_show(const ls_opt_t *o, char *out, size_t n)
{
    switch (o->arg) {
    case 0: snprintf(out, n, "%.4f MHz", s_hz / 1e6); break;
    case 1: snprintf(out, n, "%.1f kHz", s_bw_hz / 1e3); break;
    default: snprintf(out, n, "+%d dB", s_thresh_db); break;
    }
}

static const ls_opt_t OPTS[] = {
    { .label = "PLACE", .kind = LS_OPT_CYCLE, .names = PRESET_NAME, .n = PRESET_N + 1,
      .get = o_preset, .set = o_set_preset },
    { .label = "FREQUENCY", .kind = LS_OPT_NUMBER, .arg = 0, .num = o_num, .set_num = o_set_num,
      .lo = 150, .hi = 1100, .unit = "MHz, 150 to 1100", .show = o_show },
    { .label = "FILTER", .kind = LS_OPT_NUMBER, .arg = 1, .num = o_num, .set_num = o_set_num,
      .lo = 3.5, .hi = 3077, .unit = "kHz, the next rung up is used", .show = o_show },
    { .label = "THRESHOLD", .kind = LS_OPT_NUMBER, .arg = 2, .num = o_num, .set_num = o_set_num,
      .lo = 3, .hi = 40, .unit = "dB over the floor", .show = o_show },
};

const ls_experiment_t exp_burst = {
    .id = "burst",
    .name = "BURST SCOPE",
    .sub = "Burst timing at one frequency",
    .maturity = LS_EXP_TRYING,
    .needs = "something bursty nearby: a key fob, a tyre sensor, a toll reader",
    .start = burst_start,
    .stop = burst_stop,
    .poll = burst_poll,
    .lines = burst_lines,
    .configure = burst_configure,
    .opts = OPTS,
    .n_opts = (int)(sizeof(OPTS) / sizeof(OPTS[0])),
};
