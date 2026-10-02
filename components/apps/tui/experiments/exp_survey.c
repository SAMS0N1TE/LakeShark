/* BAND SURVEY: what is active around here, left to run. The LoRa chip's
   RSSI sweep walks everything it can reach in chunks of at most 50 MHz, a
   few rows on each, and keeps per chunk a noise floor and every bin that
   rose over it: how strong, how often (the fraction of rows it was over:
   its duty), and when it was first and last seen. Anything that comes up
   goes in a short log. On an LR2021 that is 150-1100 MHz on the LF input
   and 1500-2500 MHz on the HF one; on an SX1262, 150-960 MHz. Receive
   only. */
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

extern const ls_experiment_t exp_survey;

#define CHUNK_MAX_HZ   50000000u
#define CHUNKS_MAX     48
#define LOG_N          32
#define VIEW_EVERY_US  500000
#define TEXT_LINES     24
#define BAND_LINES     12
#define TOP_LINES      3

/* The band list OPTIONS switches on and off. Below about 200 MHz this
   board's front end hears next to nothing, so the first is off to start. */
typedef struct { uint32_t lo_hz, hi_hz; bool on; } group_t;
static const group_t GROUP[] = {
    {  150000000u,  300000000u, false },
    {  300000000u,  470000000u, true },
    {  470000000u,  806000000u, true },
    {  806000000u,  902000000u, true },
    {  902000000u,  928000000u, true },
    {  928000000u,  960000000u, true },
    {  960000000u, 1100000000u, true },
    { 1500000000u, 1700000000u, true },
    { 1700000000u, 2200000000u, true },
    { 2200000000u, 2400000000u, true },
    { 2400000000u, 2500000000u, true },
};
#define GROUP_N ((int)(sizeof(GROUP) / sizeof(GROUP[0])))

/* What the next start uses. */
static volatile uint32_t s_groups;
static volatile bool s_groups_set;
static volatile uint32_t s_custom_lo, s_custom_hi;   /* 0: the band list */
static volatile int s_looks = 6;                      /* rows a chunk each visit */
static volatile int s_thresh_db = 10;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static EXT_RAM_BSS_ATTR char s_text[TEXT_LINES][LS_EXP_LINE];
static int s_text_n;

static EXT_RAM_BSS_ATTR exp_survey_chunk_t s_chunk[CHUNKS_MAX];
static EXT_RAM_BSS_ATTR float s_row[EXP_SURVEY_BINS];
static EXT_RAM_BSS_ATTR struct {
    int      n, cur, rows_here;
    uint32_t cycles, rows, errors;
    int64_t  t_start, t_view;
    exp_survey_event_t log[LOG_N];
    int      log_n, log_head;
} w;

static uint32_t groups(void)
{
    if (s_groups_set) return s_groups;
    uint32_t m = 0;
    for (int i = 0; i < GROUP_N; i++) if (GROUP[i].on) m |= 1u << i;
    return m;
}

/* The chunks for the band list or the typed range, as far as `caps` reach. */
static int plan(uint32_t caps, exp_span_t *out, int max)
{
    if (s_custom_lo) return exp_survey_plan(s_custom_lo, s_custom_hi, caps, CHUNK_MAX_HZ, out, max);
    const uint32_t m = groups();
    int n = 0;
    for (int i = 0; i < GROUP_N && n < max; i++)
        if (m & (1u << i))
            n += exp_survey_plan(GROUP[i].lo_hz, GROUP[i].hi_hz, caps, CHUNK_MAX_HZ, out + n, max - n);
    return n;
}

static void fmt_band(char *out, size_t n, const exp_span_t *s)
{
    snprintf(out, n, "%.0f-%.0f", s->lo_hz / 1e6, s->hi_hz / 1e6);
}

static void view(int64_t now)
{
    static EXT_RAM_BSS_ATTR char t[TEXT_LINES][LS_EXP_LINE];
    int n = 0;
    const exp_survey_chunk_t *cur = &s_chunk[w.cur];
    char band[24];
    fmt_band(band, sizeof(band), &cur->span);
    snprintf(t[n++], LS_EXP_LINE, "CHUNK %d/%d %s MHz  CYCLE %lu", w.cur + 1, w.n, band,
             (unsigned long)w.cycles + 1);
    const double secs = (now - w.t_start) / 1e6;
    snprintf(t[n++], LS_EXP_LINE, "ROWS %lu  %.1f/s  +%d dB  errors %lu", (unsigned long)w.rows,
             secs > 0 ? w.rows / secs : 0.0, s_thresh_db, (unsigned long)w.errors);

    /* Each chunk with something in it, in frequency order; the rest in one
       line. The strongest across all of them are kept for after. */
    exp_survey_emitter_t top[TOP_LINES];
    int ntop = 0, quiet = 0, seen = 0;
    float qlo = 0, qhi = 0;
    snprintf(t[n++], LS_EXP_LINE, "BAND     FLOOR STRONGEST: MHz/dBm/duty");
    for (int c = 0; c < w.n; c++) {
        const exp_survey_chunk_t *k = &s_chunk[c];
        if (!k->rows) continue;
        seen++;
        exp_survey_emitter_t e[2];
        const int ne = exp_survey_top(k, e, 2);
        for (int i = 0; i < ne; i++) {
            int at;
            if (ntop < TOP_LINES) at = ntop++;
            else if (e[i].peak_dbm > top[TOP_LINES - 1].peak_dbm) at = TOP_LINES - 1;
            else continue;
            top[at] = e[i];
            while (at > 0 && top[at].peak_dbm > top[at - 1].peak_dbm) {
                const exp_survey_emitter_t x = top[at]; top[at] = top[at - 1]; top[at - 1] = x; at--;
            }
        }
        if (!ne) {
            if (!quiet || k->floor_dbm < qlo) qlo = k->floor_dbm;
            if (!quiet || k->floor_dbm > qhi) qhi = k->floor_dbm;
            quiet++;
            continue;
        }
        if (n >= 3 + BAND_LINES) continue;
        fmt_band(band, sizeof(band), &k->span);
        char line[96];
        int p = snprintf(line, sizeof(line), "%-9s %4.0f", band, (double)k->floor_dbm);
        for (int i = 0; i < ne; i++)
            p += snprintf(line + p, sizeof(line) - p, " %.2f/%.0f/%.0f%%", e[i].hz / 1e6,
                          (double)e[i].peak_dbm, (double)(e[i].duty * 100.0f));
        snprintf(t[n++], LS_EXP_LINE, "%s", line);
    }
    if (!seen) snprintf(t[n++], LS_EXP_LINE, "  waiting for the first row");
    else if (quiet) snprintf(t[n++], LS_EXP_LINE, "quiet: %d of %d, floors %.0f..%.0f dBm", quiet,
                             seen, (double)qlo, (double)qhi);

    if (ntop) {
        snprintf(t[n++], LS_EXP_LINE, "TOP      MHz   dBm  duty  first-last");
        for (int i = 0; i < ntop; i++) {
            char a[10], b[10];
            exp_fmt_clock(top[i].first_s, a, sizeof(a));
            exp_fmt_clock(top[i].last_s, b, sizeof(b));
            snprintf(t[n++], LS_EXP_LINE, "%11.3f %5.0f %4.0f%%  %s-%s", top[i].hz / 1e6,
                     (double)top[i].peak_dbm, (double)(top[i].duty * 100.0f), a, b);
        }
    }
    /* The newest of the log, two to a line. */
    for (int k = 0; k < w.log_n && n < TEXT_LINES; k += 2) {
        char line[96];
        int p = snprintf(line, sizeof(line), k ? "    " : "LOG ");
        for (int j = k; j < k + 2 && j < w.log_n; j++) {
            const exp_survey_event_t *e = &w.log[(w.log_head - 1 - j + 2 * LOG_N) % LOG_N];
            char clock[10];
            exp_fmt_clock(e->t_s, clock, sizeof(clock));
            p += snprintf(line + p, sizeof(line) - p, "%s%s %.3f %.0f", j > k ? "  " : "", clock,
                          e->hz / 1e6, (double)e->dbm);
        }
        snprintf(t[n++], LS_EXP_LINE, "%s", line);
    }
    portENTER_CRITICAL(&s_lock);
    memcpy(s_text, t, sizeof(s_text));
    s_text_n = n;
    portEXIT_CRITICAL(&s_lock);
}

static bool survey_start(char *why, size_t n)
{
    /* The last run's readout goes; until this one has its own, lines()
       says what it will do. */
    portENTER_CRITICAL(&s_lock);
    s_text_n = 0;
    portEXIT_CRITICAL(&s_lock);
    const uint32_t caps = ls_lora_caps();
    if (!(caps & LS_LORA_CAP_RSSI_INST)) { snprintf(why, n, "This chip gives no RSSI reading"); return false; }
    static EXT_RAM_BSS_ATTR exp_span_t spans[CHUNKS_MAX];
    const int nc = plan(caps, spans, CHUNKS_MAX);
    if (!nc) {
        snprintf(why, n, "Nothing in the list is in this radio's reach");
        return false;
    }
    esp_err_t err = ls_lora_scan_begin(spans[0].lo_hz, spans[0].hi_hz);
    /* A sweep hands the part back to LoRa as it found it, so it wants a LoRa
       configuration to go back to. With the mesh never started there is
       none, and the board's defaults stand in, as they do for `lora scan`. */
    if (err == ESP_ERR_INVALID_STATE && !ls_lora_cfg()) {
        ls_lora_cfg_t c;
        ls_lora_cfg_default(&c);
        if (ls_lora_configure(&c) == ESP_OK) err = ls_lora_scan_begin(spans[0].lo_hz, spans[0].hi_hz);
    }
    if (err != ESP_OK) {
        snprintf(why, n, "The sweep was refused: %s", esp_err_to_name(err));
        return false;
    }
    memset(&w, 0, sizeof(w));
    for (int c = 0; c < nc; c++) exp_survey_chunk_init(&s_chunk[c], spans[c], EXP_SURVEY_BINS);
    w.n = nc;
    w.t_start = w.t_view = esp_timer_get_time();
    view(w.t_start);
    return true;
}

static void survey_stop(void) { ls_lora_scan_end(); }

static void next_chunk(void)
{
    w.rows_here = 0;
    w.cur++;
    if (w.cur >= w.n) { w.cur = 0; w.cycles++; }
    const exp_span_t *s = &s_chunk[w.cur].span;
    if (ls_lora_scan_begin(s->lo_hz, s->hi_hz) != ESP_OK) w.errors++;
}

static void survey_poll(void)
{
    bool done = false;
    const int got = ls_lora_scan_pass(s_row, EXP_SURVEY_BINS, &done);
    const int64_t now = esp_timer_get_time();
    if (got <= 0) {
        /* A pass that failed starts the chunk over; one that keeps failing
           is left for the next. */
        w.errors++;
        next_chunk();
    } else if (done) {
        exp_survey_event_t ev[8];
        const uint32_t t_s = (uint32_t)((now - w.t_start) / 1000000);
        const int ne = exp_survey_row(&s_chunk[w.cur], s_row, (float)s_thresh_db, t_s, ev, 8);
        for (int i = 0; i < ne; i++) {
            w.log[w.log_head] = ev[i];
            w.log_head = (w.log_head + 1) % LOG_N;
            if (w.log_n < LOG_N) w.log_n++;
        }
        w.rows++;
        if (++w.rows_here >= s_looks) next_chunk();
    }
    if (now - w.t_view >= VIEW_EVERY_US) {
        view(now);
        w.t_view = now;
    }
}

static int survey_lines(char (*out)[LS_EXP_LINE], int max)
{
    int n;
    portENTER_CRITICAL(&s_lock);
    n = s_text_n < max ? s_text_n : max;
    if (n > 0) memcpy(out, s_text, (size_t)n * LS_EXP_LINE);
    portEXIT_CRITICAL(&s_lock);
    if (n > 0) return n;
    /* Never run: what the next start covers. */
    n = 0;
    if (s_custom_lo) {
        if (n < max) snprintf(out[n++], LS_EXP_LINE, "RANGE %.0f-%.0f MHz (start all: back to the list)",
                              s_custom_lo / 1e6, s_custom_hi / 1e6);
    } else {
        const uint32_t m = groups(), caps = ls_lora_caps();
        char line[96] = "BANDS";
        int p = 5;
        for (int i = 0; i < GROUP_N; i++) {
            exp_span_t probe[1];
            if (!(m & (1u << i)) ||
                !exp_survey_plan(GROUP[i].lo_hz, GROUP[i].hi_hz, caps, CHUNK_MAX_HZ, probe, 1)) continue;
            char b[16];
            snprintf(b, sizeof(b), " %.0f-%.0f", GROUP[i].lo_hz / 1e6, GROUP[i].hi_hz / 1e6);
            if (p + (int)strlen(b) >= LS_EXP_LINE - 1) {
                if (n < max) snprintf(out[n++], LS_EXP_LINE, "%s", line);
                p = snprintf(line, sizeof(line), "     ");
            }
            p += snprintf(line + p, sizeof(line) - p, "%s", b);
        }
        if (n < max) snprintf(out[n++], LS_EXP_LINE, "%s", line);
    }
    if (n < max) snprintf(out[n++], LS_EXP_LINE, "%d rows on each chunk of 50 MHz or less, +%d dB",
                          s_looks, s_thresh_db);
    return n;
}

static bool parse_mhz(const char *text, double *mhz)
{
    char *end = NULL;
    const double v = strtod(text, &end);
    if (end == text || *end || !(v >= 150 && v <= 2500)) return false;
    *mhz = v;
    return true;
}

/* "exp survey start <lo> <hi>" sweeps that range instead of the list;
   "exp survey start all" goes back to the list. */
static bool survey_configure(int argc, char **argv, char *why, size_t n)
{
    if (argc == 1 && !strcmp(argv[0], "all")) { s_custom_lo = s_custom_hi = 0; return true; }
    double lo, hi;
    if (argc != 2 || !parse_mhz(argv[0], &lo) || !parse_mhz(argv[1], &hi) || hi - lo < 1.0) {
        snprintf(why, n, "<lo MHz> <hi MHz>, 150-2500, or 'all'");
        return false;
    }
    exp_span_t probe[1];
    if (!exp_survey_plan((uint32_t)(lo * 1e6 + 0.5), (uint32_t)(hi * 1e6 + 0.5), ls_lora_caps(),
                         CHUNK_MAX_HZ, probe, 1)) {
        snprintf(why, n, "%.0f-%.0f MHz is out of this radio's reach", lo, hi);
        return false;
    }
    s_custom_lo = (uint32_t)(lo * 1e6 + 0.5);
    s_custom_hi = (uint32_t)(hi * 1e6 + 0.5);
    return true;
}

/* OPTIONS. Every change restarts a running survey, from nothing. */
static void restart(void) { if (ls_exp_running() == &exp_survey) ls_exp_start(&exp_survey); }

static int o_group(const ls_opt_t *o) { return (groups() >> o->arg) & 1u; }
static void o_set_group(const ls_opt_t *o, int v)
{
    uint32_t m = groups();
    if (v) m |= 1u << o->arg;
    else m &= ~(1u << o->arg);
    s_groups = m;
    s_groups_set = true;
    s_custom_lo = s_custom_hi = 0;   /* touching the list means the list */
    restart();
}
static const char *o_group_why(const ls_opt_t *o)
{
    exp_span_t probe[1];
    return exp_survey_plan(GROUP[o->arg].lo_hz, GROUP[o->arg].hi_hz, ls_lora_caps(),
                           CHUNK_MAX_HZ, probe, 1) ? NULL : "out of this radio's reach";
}
static double o_num(const ls_opt_t *o) { return o->arg == 100 ? s_looks : s_thresh_db; }
static void o_set_num(const ls_opt_t *o, double v)
{
    if (o->arg == 100) s_looks = (int)(v + 0.5);
    else s_thresh_db = (int)(v + 0.5);
    restart();
}
static void o_show(const ls_opt_t *o, char *out, size_t n)
{
    if (o->arg == 100) snprintf(out, n, "%d rows", s_looks);
    else snprintf(out, n, "+%d dB", s_thresh_db);
}

/* One row a band of the list, in its order; the label says what lives
   there. */
#define GROUP_ROW(i, text) { .label = (text), .kind = LS_OPT_TOGGLE, .arg = (i), .get = o_group, \
                             .set = o_set_group, .why_not = o_group_why }
static const ls_opt_t OPTS[] = {
    GROUP_ROW(0, "150-300 MHz"),
    GROUP_ROW(1, "300-470 UHF"),
    GROUP_ROW(2, "470-806 TV, LTE"),
    GROUP_ROW(3, "806-902 CELL"),
    GROUP_ROW(4, "902-928 ISM"),
    GROUP_ROW(5, "928-960 PAGERS"),
    GROUP_ROW(6, "960-1100 AIR"),
    GROUP_ROW(7, "1500-1700 GNSS, SAT"),
    GROUP_ROW(8, "1700-2200 CELL"),
    GROUP_ROW(9, "2200-2400"),
    GROUP_ROW(10, "2400-2500 ISM"),
    { .label = "LOOKS PER CHUNK", .kind = LS_OPT_NUMBER, .arg = 100, .num = o_num,
      .set_num = o_set_num, .lo = 1, .hi = 50, .unit = "rows on a chunk before the next",
      .show = o_show },
    { .label = "ABOVE FLOOR", .kind = LS_OPT_NUMBER, .arg = 101, .num = o_num,
      .set_num = o_set_num, .lo = 3, .hi = 30, .unit = "dB over the floor to count",
      .show = o_show },
};
_Static_assert(sizeof(OPTS) / sizeof(OPTS[0]) == GROUP_N + 2, "one OPTIONS row a band");

const ls_experiment_t exp_survey = {
    .id = "survey",
    .name = "BAND SURVEY",
    .sub = "What is on the air, band by band",
    .maturity = LS_EXP_TRYING,
    .start = survey_start,
    .stop = survey_stop,
    .poll = survey_poll,
    .lines = survey_lines,
    .configure = survey_configure,
    .opts = OPTS,
    .n_opts = (int)(sizeof(OPTS) / sizeof(OPTS[0])),
};
