/* LS_TEST_SOURCES: experiments/exp_rf.c and experiments/exp_burst.c with
   ls_experiments.c, against a LoRa chip whose RSSI is a made-up pulse train
   in time: the burst detector, the interval statistics, the strip, and
   BURST SCOPE end to end. */

#include "ls_test.h"

#include "ls_experiments.h"
#include "ls_lora.h"
#include "esp_timer.h"
#include "experiments/exp_rf.h"

#include <stdio.h>
#include <string.h>

void ls_shim_time_set(int64_t us);
void ls_shim_time_advance(int64_t us);

/* ---------------------------------------------------------- the board -- */

const char *ls_exp_hw_radio_busy(void) { return NULL; }
bool ls_exp_hw_radio_take(void) { return true; }
void ls_exp_hw_radio_give(void) { }
bool ls_exp_hw_wake(void) { return false; }

static uint32_t s_caps;
static bool s_fsk;
static ls_fsk_cfg_t s_cfg;
static int s_rearms;
/* The air: a burst of s_len_us every s_period_us from s_t0, at -70 dBm, over
   a floor near -110 that wobbles by a couple of dB. Each read costs s_read_us. */
static int64_t s_t0, s_period_us = 20000, s_len_us = 2000, s_read_us = 80;
static unsigned s_reads;

uint32_t ls_lora_caps(void) { return s_caps; }
uint32_t ls_lora_fsk_bw_snap(uint32_t hz) { return hz < 3472 ? 3472 : hz; }
esp_err_t ls_lora_fsk_begin(const ls_fsk_cfg_t *cfg) { s_cfg = *cfg; s_fsk = true; return ESP_OK; }
esp_err_t ls_lora_fsk_end(void) { s_fsk = false; return ESP_OK; }
esp_err_t ls_lora_fsk_receive(void) { s_rearms++; return ESP_OK; }
int ls_lora_fsk_poll(uint8_t *buf, size_t size, float *rssi) { (void)buf; (void)size; (void)rssi; return 0; }

static float air(int64_t t)
{
    static const float WOBBLE[7] = { 0.0f, 1.5f, -2.0f, 1.0f, -1.0f, 2.0f, -1.5f };
    const int64_t k = t - s_t0;
    if (k >= 0 && k % s_period_us < s_len_us) return -70.0f;
    return -110.0f + WOBBLE[(t / 97) % 7];
}

esp_err_t ls_lora_rssi_inst(float *dbm)
{
    if (!s_fsk) return ESP_ERR_INVALID_STATE;
    ls_shim_time_advance(s_read_us);
    *dbm = air(esp_timer_get_time());
    s_reads++;
    return ESP_OK;
}

/* --------------------------------------------------------- the detector -- */

/* Feed `air` from t0 to t1 every `dt` us; returns the bursts logged. */
static int run_air(exp_burst_det_t *d, exp_burst_log_t *l, int64_t t0, int64_t t1, int64_t dt)
{
    const uint32_t before = l->count;
    for (int64_t t = t0; t < t1; t += dt) {
        exp_burst_t b;
        if (exp_burst_det_feed(d, t, air(t), &b)) exp_burst_log_add(l, &b);
    }
    return (int)(l->count - before);
}

LS_CASE(a_periodic_pulse_train_is_found_timed_and_its_period_named)
{
    s_t0 = 1000000; s_period_us = 100000; s_len_us = 5000;
    exp_burst_det_t d;
    exp_burst_log_t l;
    exp_burst_det_init(&d, 10.0f);
    exp_burst_log_reset(&l);
    /* Half a second of floor first, then two seconds of bursts. */
    LS_EQ_INT(run_air(&d, &l, 500000, 3000000, 100), 20);
    LS_NEAR(d.floor_dbm, -111.0, 1.5);
    for (int k = 0; k < l.n; k++) {
        const exp_burst_t *b = exp_burst_log_at(&l, k);
        LS_NEAR((double)b->len_us, 5000.0, 150.0);
        LS_NEAR(b->peak_dbm, -70.0, 0.01);
        LS_CHECK(!b->cut);
    }
    LS_EQ_INT((int)exp_burst_log_at(&l, 0)->start_us, 1000000 + 19 * 100000);
    exp_burst_rhythm_t r;
    exp_burst_rhythm(&l, &r);
    LS_EQ_INT(r.intervals, 19);
    LS_NEAR((double)r.mode_us, 100000.0, 1.0);
    LS_EQ_INT(r.mode_hits, 19);
    LS_EQ_INT(r.period_hits, 19);
    LS_NEAR((double)r.median_len_us, 5000.0, 150.0);
    LS_NEAR(l.peak_dbm, -70.0, 0.01);
}

LS_CASE(missed_bursts_still_fit_the_period_as_whole_multiples)
{
    s_t0 = 0; s_period_us = 50000; s_len_us = 3000;
    exp_burst_det_t d;
    exp_burst_log_t l;
    exp_burst_det_init(&d, 10.0f);
    exp_burst_log_reset(&l);
    /* Every third burst is missed: the receiver looks away for it. */
    exp_burst_t b;
    for (int64_t t = 0; t < 1500000; t += 100) {
        if ((t / 50000) % 3 == 2) {
            if (exp_burst_det_flush(&d, &b)) exp_burst_log_add(&l, &b);
            continue;
        }
        if (exp_burst_det_feed(&d, t, air(t), &b)) exp_burst_log_add(&l, &b);
    }
    exp_burst_rhythm_t r;
    exp_burst_rhythm(&l, &r);
    LS_CHECK(r.intervals >= 18);
    LS_NEAR((double)r.mode_us, 50000.0, 1.0);
    LS_CHECK(r.mode_hits < r.intervals);        /* some are 100 ms */
    LS_EQ_INT(r.period_hits, r.intervals);      /* and all fit */
}

LS_CASE(hysteresis_holds_a_burst_through_a_shallow_dip_and_not_a_deep_one)
{
    exp_burst_det_t d;
    exp_burst_log_t l;
    exp_burst_det_init(&d, 10.0f);
    exp_burst_log_reset(&l);
    exp_burst_t b;
    int64_t t = 0;
    for (; t < 10000; t += 100) LS_CHECK(!exp_burst_det_feed(&d, t, -110.0f, &b));
    /* Up, a dip to 8 dB over the floor (inside the 3 dB of hysteresis), up. */
    for (; t < 12000; t += 100) LS_CHECK(!exp_burst_det_feed(&d, t, -80.0f, &b));
    for (; t < 12500; t += 100) LS_CHECK(!exp_burst_det_feed(&d, t, -102.0f, &b));
    for (; t < 14000; t += 100) LS_CHECK(!exp_burst_det_feed(&d, t, -80.0f, &b));
    LS_CHECK(exp_burst_det_feed(&d, t, -110.0f, &b));
    LS_EQ_INT((int)b.start_us, 10000);
    LS_EQ_UINT(b.len_us, 4000);
    LS_NEAR(b.floor_dbm, -110.0, 0.01);
    t += 100;
    /* Up, down to the floor, up: two. */
    int ended = 0;
    for (int k = 0; k < 2; k++) {
        for (int64_t e = t + 1000; t < e; t += 100) ended += exp_burst_det_feed(&d, t, -80.0f, &b);
        for (int64_t e = t + 1000; t < e; t += 100) ended += exp_burst_det_feed(&d, t, -110.0f, &b);
    }
    LS_EQ_INT(ended, 2);
    LS_EQ_UINT(b.len_us, 1000);
}

LS_CASE(a_hole_in_the_readings_cuts_a_burst_and_a_held_level_becomes_the_floor)
{
    exp_burst_det_t d;
    exp_burst_t b;
    exp_burst_det_init(&d, 10.0f);
    int64_t t = 0;
    for (; t < 5000; t += 100) exp_burst_det_feed(&d, t, -110.0f, &b);
    for (; t < 6000; t += 100) LS_CHECK(!exp_burst_det_feed(&d, t, -75.0f, &b));
    /* Ten milliseconds with no reading, then two that are still up: the old
       one is cut where the readings stopped, and the second reading opens
       a new one (a single reading over the line is not a burst). */
    t += 10000;
    LS_CHECK(exp_burst_det_feed(&d, t, -75.0f, &b));
    LS_CHECK(b.cut);
    LS_EQ_INT((int)b.start_us, 5000);
    LS_EQ_UINT(b.len_us, 1000);
    LS_CHECK(!d.open);
    t += 100;
    LS_CHECK(!exp_burst_det_feed(&d, t, -75.0f, &b));
    LS_CHECK(d.open);
    LS_CHECK(exp_burst_det_flush(&d, &b));
    LS_CHECK(b.cut);
    LS_CHECK(!exp_burst_det_flush(&d, &b));

    /* A carrier switched on and left: past the longest burst it is the
       floor, and nothing more is reported. */
    exp_burst_det_init(&d, 10.0f);
    d.max_len_us = 50000;
    t = 0;
    for (; t < 5000; t += 100) exp_burst_det_feed(&d, t, -110.0f, &b);
    int ended = 0;
    for (; t < 500000; t += 100) ended += exp_burst_det_feed(&d, t, -60.0f, &b);
    LS_EQ_INT(ended, 0);
    LS_CHECK(!d.open);
    LS_NEAR(d.floor_dbm, -60.0, 0.5);
}

LS_CASE(the_strip_marks_the_cells_a_burst_covers)
{
    exp_burst_log_t l;
    exp_burst_log_reset(&l);
    exp_burst_t b = { .start_us = 1000000, .len_us = 100000, .peak_dbm = -70 };
    exp_burst_log_add(&l, &b);
    b.start_us = 1850000; b.len_us = 10;
    exp_burst_log_add(&l, &b);
    char s[11];
    /* Ten cells of 100 ms from 1.0 s to 2.0 s. */
    exp_burst_strip(&l, 2000000, 1000000, 0, s, 10);
    LS_EQ_STR(s, "#.......#.");
    /* Before the start, nothing was listening. */
    exp_burst_strip(&l, 2000000, 1000000, 1300000, s, 10);
    LS_EQ_STR(s, "#  .....#.");
    exp_burst_strip(&l, 2000000, 1000000, 1200000, s, 10);
    LS_EQ_STR(s, "# ......#.");
}

LS_CASE(buckets_and_durations_read_as_printed)
{
    LS_EQ_INT(exp_len_bucket(0), 0);
    LS_EQ_INT(exp_len_bucket(99), 0);
    LS_EQ_INT(exp_len_bucket(100), 1);
    LS_EQ_INT(exp_len_bucket(5000), 4);
    LS_EQ_INT(exp_len_bucket(100000), 7);
    LS_EQ_INT(exp_gap_bucket(999), 0);
    LS_EQ_INT(exp_gap_bucket(90000), 4);
    LS_EQ_INT(exp_gap_bucket(100000), 5);
    LS_EQ_INT(exp_gap_bucket(4000000), 8);
    char s[16];
    exp_fmt_us(840, s, sizeof(s));      LS_EQ_STR(s, "840us");
    exp_fmt_us(1234, s, sizeof(s));     LS_EQ_STR(s, "1.23ms");
    exp_fmt_us(12345, s, sizeof(s));    LS_EQ_STR(s, "12.3ms");
    exp_fmt_us(1250000, s, sizeof(s));  LS_EQ_STR(s, "1.25s");
    exp_fmt_clock(247, s, sizeof(s));   LS_EQ_STR(s, "4:07");
    exp_fmt_clock(11520, s, sizeof(s)); LS_EQ_STR(s, "3h12");
    LS_EQ_UINT(exp_settle_us(500000), 300);
    LS_EQ_UINT(exp_settle_us(3076923), 300);
    LS_EQ_UINT(exp_settle_us(41667), 3600);
}

/* ------------------------------------------------------- BURST SCOPE -- */

static const ls_experiment_t *scope(void)
{
    extern const ls_experiment_t exp_burst;
    ls_exp_forget();
    ls_exp_register(&exp_burst);
    s_caps = LS_LORA_CAP_LORA | LS_LORA_CAP_FSK | LS_LORA_CAP_RSSI_INST;
    s_fsk = false;
    s_reads = 0;
    s_rearms = 0;
    ls_shim_time_set(1000000);
    return &exp_burst;
}

static int run_console(const char *line)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", line);
    char *argv[8];
    int argc = 0;
    for (char *t = strtok(buf, " "); t && argc < 8; t = strtok(NULL, " ")) argv[argc++] = t;
    return ls_exp_console(argc, argv);
}

static bool has_line(char (*out)[LS_EXP_LINE], int n, const char *want)
{
    for (int i = 0; i < n; i++) if (strstr(out[i], want)) return true;
    return false;
}

LS_CASE(burst_scope_takes_a_frequency_and_a_filter_in_the_radio_s_reach)
{
    const ls_experiment_t *e = scope();
    char why[96];
    char *ok[] = { "915.75", "250" }, *one[] = { "433.92" }, *far[] = { "1200" }, *hf[] = { "2440" };
    char *bad_bw[] = { "915", "9000" };
    LS_CHECK(e->configure(2, ok, why, sizeof(why)));
    LS_CHECK(e->configure(1, one, why, sizeof(why)));
    LS_CHECK(!e->configure(1, far, why, sizeof(why)));
    LS_CHECK(!e->configure(1, hf, why, sizeof(why)));
    LS_CHECK(strstr(why, "150-960 MHz") != NULL);           /* an SX1262 */
    LS_CHECK(!e->configure(2, bad_bw, why, sizeof(why)));
    s_caps |= LS_LORA_CAP_RX_WIDE | LS_LORA_CAP_BAND_1G5_2G5;   /* an LR2021 */
    LS_CHECK(e->configure(1, hf, why, sizeof(why)));
    LS_CHECK(!e->configure(1, far, why, sizeof(why)));       /* between the inputs */
    LS_CHECK(strstr(why, "150-1100 or 1500-2500 MHz") != NULL);
    LS_EQ_INT(e->n_opts, 4);
}

LS_CASE(burst_scope_times_a_20_ms_train_through_the_framework)
{
    const ls_experiment_t *e = scope();
    s_t0 = 0; s_period_us = 20000; s_len_us = 2000; s_read_us = 80;
    LS_EQ_INT(run_console("exp burst start 915.75 250"), 0);
    LS_CHECK(ls_exp_running() == e);
    LS_EQ_UINT(s_cfg.freq_hz, 915750000u);
    LS_EQ_UINT(s_cfg.bandwidth_hz, 250000u);
    LS_CHECK(s_cfg.bitrate + 2 * s_cfg.deviation_hz <= s_cfg.bandwidth_hz);
    /* Two seconds of polls with the worker's sleep between them. */
    for (int i = 0; i < 120; i++) { ls_exp_service(); ls_shim_time_advance(2000); }
    static char out[24][LS_EXP_LINE];
    const int n = ls_exp_read_lines(e, out, 24);
    LS_CHECK(n >= 10);
    for (int i = 0; i < n; i++) printf("  | %s\n", out[i]);   /* the readout, for the log */
    for (int i = 0; i < n; i++) LS_CHECK(strlen(out[i]) <= 48);
    LS_EQ_STR(out[0], "FREQ 915.7500 MHz  FILTER 250.0 kHz  +10 dB");
    LS_CHECK(strstr(out[1], "RATE ") == out[1]);
    LS_CHECK(strstr(out[1], "80us a read") != NULL);         /* the read's cost, as measured */
    LS_CHECK(has_line(out, n, "EVERY 20.0ms"));
    LS_CHECK(out[3][0] == '[' && strchr(out[3], '#') && strchr(out[3], '.'));
    /* The lengths sit in the 1-3 ms bucket: two milliseconds and a read. */
    LS_CHECK(strstr(out[4], "LEN ") == out[4]);
    int bucket_3m = -1;
    sscanf(out[5] + 4 + 3 * 4, "%d", &bucket_3m);
    LS_CHECK(bucket_3m >= 40);
    LS_CHECK(has_line(out, n, "LAST ") && has_line(out, n, " -70"));
    LS_CHECK(s_reads > 10000);
    LS_EQ_INT(run_console("exp burst stop"), 0);
    LS_CHECK(!s_fsk);
}

LS_CASE(burst_scope_options_choose_a_toll_channel_and_restart)
{
    const ls_experiment_t *e = scope();
    ls_exp_start(e);
    ls_exp_service();
    LS_CHECK(ls_exp_running() == e);
    const ls_opt_t *place = &e->opts[0];
    place->set(place, 4);
    LS_CHECK(ls_exp_busy());
    ls_exp_service();
    LS_EQ_UINT(s_cfg.freq_hz, 915750000u);
    LS_EQ_INT(place->get(place), 4);
    e->opts[1].set_num(&e->opts[1], 912.345);
    ls_exp_service();
    LS_EQ_INT(place->get(place), 7);                          /* typed */
    char shown[32];
    e->opts[3].show(&e->opts[3], shown, sizeof(shown));
    LS_EQ_STR(shown, "+10 dB");
    ls_exp_stop();
    ls_exp_service();
}
