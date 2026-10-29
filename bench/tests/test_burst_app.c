/* BURST SCOPE as the SD app builds it (app-sdk/apps/burst): the burst
   detector, the interval statistics and the strip on level series made up
   in time, the level readings on synthetic I/Q, and the scope end to end on
   a pulse train in noise. */

#include "ls_test.h"
#include "scope.h"   /* app-sdk/apps/burst, with burst_det.h */

#include <math.h>
#include <stdio.h>
#include <string.h>

/* The air in level readings: a burst of s_len_us every s_period_us from
   s_t0 at -70, over a floor near -110 that wobbles by a couple of dB. */
static int64_t s_t0, s_period_us = 20000, s_len_us = 2000;

static float air(int64_t t)
{
    static const float WOBBLE[7] = { 0.0f, 1.5f, -2.0f, 1.0f, -1.0f, 2.0f, -1.5f };
    const int64_t k = t - s_t0;
    if (k >= 0 && k % s_period_us < s_len_us) return -70.0f;
    return -110.0f + WOBBLE[(t / 97) % 7];
}

/* --------------------------------------------------------- the detector -- */

/* Feed `air` from t0 to t1 every `dt` us; returns the bursts logged. */
static int run_air(burst_det_t *d, burst_log_t *l, int64_t t0, int64_t t1, int64_t dt)
{
    const uint32_t before = l->count;
    for (int64_t t = t0; t < t1; t += dt) {
        burst_t b;
        if (burst_det_feed(d, t, air(t), &b)) burst_log_add(l, &b);
    }
    return (int)(l->count - before);
}

LS_CASE(a_periodic_pulse_train_is_found_timed_and_its_period_named)
{
    s_t0 = 1000000; s_period_us = 100000; s_len_us = 5000;
    burst_det_t d;
    burst_log_t l;
    burst_det_init(&d, 10.0f);
    burst_log_reset(&l);
    /* Half a second of floor first, then two seconds of bursts. */
    LS_EQ_INT(run_air(&d, &l, 500000, 3000000, 100), 20);
    LS_NEAR(d.floor_db, -111.0, 1.5);
    for (int k = 0; k < l.n; k++) {
        const burst_t *b = burst_log_at(&l, k);
        LS_NEAR((double)b->len_us, 5000.0, 150.0);
        LS_NEAR(b->peak_db, -70.0, 0.01);
        LS_CHECK(!b->cut);
    }
    LS_EQ_INT((int)burst_log_at(&l, 0)->start_us, 1000000 + 19 * 100000);
    burst_rhythm_t r;
    burst_rhythm(&l, &r);
    LS_EQ_INT(r.intervals, 19);
    LS_NEAR((double)r.mode_us, 100000.0, 1.0);
    LS_EQ_INT(r.mode_hits, 19);
    LS_EQ_INT(r.period_hits, 19);
    LS_NEAR((double)r.median_len_us, 5000.0, 150.0);
    LS_NEAR(l.peak_db, -70.0, 0.01);
}

LS_CASE(missed_bursts_still_fit_the_period_as_whole_multiples)
{
    s_t0 = 0; s_period_us = 50000; s_len_us = 3000;
    burst_det_t d;
    burst_log_t l;
    burst_det_init(&d, 10.0f);
    burst_log_reset(&l);
    /* Every third burst is missed: the receiver looks away for it. */
    burst_t b;
    for (int64_t t = 0; t < 1500000; t += 100) {
        if ((t / 50000) % 3 == 2) {
            if (burst_det_flush(&d, &b)) burst_log_add(&l, &b);
            continue;
        }
        if (burst_det_feed(&d, t, air(t), &b)) burst_log_add(&l, &b);
    }
    burst_rhythm_t r;
    burst_rhythm(&l, &r);
    LS_CHECK(r.intervals >= 18);
    LS_NEAR((double)r.mode_us, 50000.0, 1.0);
    LS_CHECK(r.mode_hits < r.intervals);        /* some are 100 ms */
    LS_EQ_INT(r.period_hits, r.intervals);      /* and all fit */
}

LS_CASE(hysteresis_holds_a_burst_through_a_shallow_dip_and_not_a_deep_one)
{
    burst_det_t d;
    burst_log_t l;
    burst_det_init(&d, 10.0f);
    burst_log_reset(&l);
    burst_t b;
    int64_t t = 0;
    for (; t < 10000; t += 100) LS_CHECK(!burst_det_feed(&d, t, -110.0f, &b));
    /* Up, a dip to 8 dB over the floor (inside the 3 dB of hysteresis), up. */
    for (; t < 12000; t += 100) LS_CHECK(!burst_det_feed(&d, t, -80.0f, &b));
    for (; t < 12500; t += 100) LS_CHECK(!burst_det_feed(&d, t, -102.0f, &b));
    for (; t < 14000; t += 100) LS_CHECK(!burst_det_feed(&d, t, -80.0f, &b));
    LS_CHECK(burst_det_feed(&d, t, -110.0f, &b));
    LS_EQ_INT((int)b.start_us, 10000);
    LS_EQ_UINT(b.len_us, 4000);
    LS_NEAR(b.floor_db, -110.0, 0.01);
    t += 100;
    /* Up, down to the floor, up: two. */
    int ended = 0;
    for (int k = 0; k < 2; k++) {
        for (int64_t e = t + 1000; t < e; t += 100) ended += burst_det_feed(&d, t, -80.0f, &b);
        for (int64_t e = t + 1000; t < e; t += 100) ended += burst_det_feed(&d, t, -110.0f, &b);
    }
    LS_EQ_INT(ended, 2);
    LS_EQ_UINT(b.len_us, 1000);
}

LS_CASE(a_hole_in_the_readings_cuts_a_burst_and_a_held_level_becomes_the_floor)
{
    burst_det_t d;
    burst_t b;
    burst_det_init(&d, 10.0f);
    int64_t t = 0;
    for (; t < 5000; t += 100) burst_det_feed(&d, t, -110.0f, &b);
    for (; t < 6000; t += 100) LS_CHECK(!burst_det_feed(&d, t, -75.0f, &b));
    /* Ten milliseconds with no reading, then two that are still up: the old
       one is cut where the readings stopped, and the second reading opens
       a new one (a single reading over the line is not a burst). */
    t += 10000;
    LS_CHECK(burst_det_feed(&d, t, -75.0f, &b));
    LS_CHECK(b.cut);
    LS_EQ_INT((int)b.start_us, 5000);
    LS_EQ_UINT(b.len_us, 1000);
    LS_CHECK(!d.open);
    t += 100;
    LS_CHECK(!burst_det_feed(&d, t, -75.0f, &b));
    LS_CHECK(d.open);
    LS_CHECK(burst_det_flush(&d, &b));
    LS_CHECK(b.cut);
    LS_CHECK(!burst_det_flush(&d, &b));

    /* A carrier switched on and left: past the longest burst it is the
       floor, and nothing more is reported. */
    burst_det_init(&d, 10.0f);
    d.max_len_us = 50000;
    t = 0;
    for (; t < 5000; t += 100) burst_det_feed(&d, t, -110.0f, &b);
    int ended = 0;
    for (; t < 500000; t += 100) ended += burst_det_feed(&d, t, -60.0f, &b);
    LS_EQ_INT(ended, 0);
    LS_CHECK(!d.open);
    LS_NEAR(d.floor_db, -60.0, 0.5);
}

LS_CASE(the_strip_marks_the_cells_a_burst_covers)
{
    burst_log_t l;
    burst_log_reset(&l);
    burst_t b = { .start_us = 1000000, .len_us = 100000, .peak_db = -70 };
    burst_log_add(&l, &b);
    b.start_us = 1850000; b.len_us = 10;
    burst_log_add(&l, &b);
    char s[11];
    /* Ten cells of 100 ms from 1.0 s to 2.0 s. */
    burst_strip(&l, 2000000, 1000000, 0, s, 10);
    LS_EQ_STR(s, "#.......#.");
    /* Before the start, nothing was listening. */
    burst_strip(&l, 2000000, 1000000, 1300000, s, 10);
    LS_EQ_STR(s, "#  .....#.");
    burst_strip(&l, 2000000, 1000000, 1200000, s, 10);
    LS_EQ_STR(s, "# ......#.");
}

LS_CASE(buckets_and_durations_read_as_printed)
{
    LS_EQ_INT(burst_len_bucket(0), 0);
    LS_EQ_INT(burst_len_bucket(99), 0);
    LS_EQ_INT(burst_len_bucket(100), 1);
    LS_EQ_INT(burst_len_bucket(5000), 4);
    LS_EQ_INT(burst_len_bucket(100000), 7);
    LS_EQ_INT(burst_gap_bucket(999), 0);
    LS_EQ_INT(burst_gap_bucket(90000), 4);
    LS_EQ_INT(burst_gap_bucket(100000), 5);
    LS_EQ_INT(burst_gap_bucket(4000000), 8);
    char s[16];
    burst_fmt_us(840, s, sizeof(s));      LS_EQ_STR(s, "840us");
    burst_fmt_us(1234, s, sizeof(s));     LS_EQ_STR(s, "1.23ms");
    burst_fmt_us(12345, s, sizeof(s));    LS_EQ_STR(s, "12.3ms");
    burst_fmt_us(1250000, s, sizeof(s));  LS_EQ_STR(s, "1.25s");
    burst_fmt_clock(247, s, sizeof(s));   LS_EQ_STR(s, "4:07");
    burst_fmt_clock(11520, s, sizeof(s)); LS_EQ_STR(s, "3h12");
    LS_EQ_UINT(burst_settle_us(500000), 300);
    LS_EQ_UINT(burst_settle_us(3076923), 300);
    LS_EQ_UINT(burst_settle_us(41667), 3600);
}

/* ---------------------------------------------------------- readings -- */

#define PI 3.14159265358979323846
#define RATE 125000u
static int16_t iq[2 * 2048];
static ls_rng_t rng;

static double gauss(void)
{
    double u1 = ((ls_rng_u32(&rng) >> 8) + 1.0) / 16777217.0;
    double u2 = (ls_rng_u32(&rng) >> 8) / 16777216.0;
    return sqrt(-2.0 * log(u1)) * cos(2 * PI * u2);
}

/* n pairs from sample `first`: a tone of amplitude amp (of full scale) at
   f Hz while on(sample) says so, in complex noise of noise_db dBFS. */
static void make(uint64_t first, int n, double f, double amp, double noise_db, bool (*on)(uint64_t))
{
    const double sigma = sqrt(pow(10.0, noise_db / 10.0) / 2.0);
    for (int k = 0; k < n; k++) {
        const uint64_t t = first + (uint64_t)k;
        const double ph = 2 * PI * f * (double)t / RATE, a = on && on(t) ? amp : 0;
        double i = a * cos(ph) + sigma * gauss(), q = a * sin(ph) + sigma * gauss();
        i = i > 0.99997 ? 0.99997 : i < -1 ? -1 : i;
        q = q > 0.99997 ? 0.99997 : q < -1 ? -1 : q;
        iq[2 * k] = (int16_t)lrint(i * 32768.0);
        iq[2 * k + 1] = (int16_t)lrint(q * 32768.0);
    }
}

static bool always(uint64_t t) { (void)t; return true; }

LS_CASE(a_tone_reads_its_power_and_the_filter_takes_its_neighbour_out)
{
    burst_level_t l;
    int64_t tt[512];
    float db[512];
    ls_rng_seed(&rng, 3);
    /* A tone at a tenth of full scale in the middle of a 12.5 kHz channel. */
    burst_level_init(&l, RATE, 12500, 2000);
    LS_EQ_INT(l.filt, 10);
    LS_EQ_INT(l.win, 25);
    LS_EQ_UINT(burst_level_us(&l), 2000);
    make(0, 2000, 1000, 0.1, -90, always);
    int got = burst_level_feed(&l, iq, 2000, 0, tt, db, 512);
    LS_EQ_INT(got, 8);
    LS_NEAR(db[3], -20.0, 0.3);
    LS_EQ_INT((int)tt[1], 2000);
    /* The same tone 12.5 kHz off, on the boxcar's first null: gone. */
    burst_level_init(&l, RATE, 12500, 2000);
    make(0, 2000, 12500, 0.1, -90, always);
    got = burst_level_feed(&l, iq, 2000, 0, tt, db, 512);
    LS_CHECK_MSG(db[3] < -45.0f, "%.1f", (double)db[3]);
    /* A block that does not follow the last starts the sums again, and its
       readings are stamped from its own first sample. */
    make(100000, 500, 1000, 0.1, -90, always);
    got = burst_level_feed(&l, iq, 500, 100000, tt, db, 512);
    LS_EQ_INT(got, 2);
    LS_EQ_INT((int)tt[0], 800000);
}

/* ------------------------------------------------------- BURST SCOPE -- */

/* 2 ms on in every 20 ms, from 0.5 s. */
static bool train(uint64_t t)
{
    const uint64_t us = t * 1000000u / RATE;
    return us >= 500000 && us % 20000 < 2000;
}

static bool has_line(char (*out)[SCOPE_LINE], int n, const char *want)
{
    for (int i = 0; i < n; i++) if (strstr(out[i], want)) return true;
    return false;
}

LS_CASE(burst_scope_times_a_20_ms_train_from_the_radio_s_i_q)
{
    static scope_t sc;
    ls_rng_seed(&rng, 9);
    scope_start(&sc, 433920000u, RATE, 250000u, 10);
    /* Two and a half seconds of the LR2021's 125 kHz I/Q, a block at a time. */
    for (uint64_t first = 0; first < RATE * 5 / 2; first += 2048) {
        make(first, 2048, 3000, 0.1, -50, train);
        scope_feed(&sc, iq, 2048, first, false);
    }
    static char out[SCOPE_LINES][SCOPE_LINE];
    const int n = scope_lines(&sc, out, SCOPE_LINES);
    for (int i = 0; i < n; i++) printf("  | %s\n", out[i]);   /* the readout, for the log */
    LS_CHECK(n >= 10);
    for (int i = 0; i < n; i++) LS_CHECK_MSG(strlen(out[i]) <= 48, "line %d: %s", i, out[i]);
    /* The filter can be no wider than the rate. */
    LS_EQ_STR(out[0], "FREQ 433.9200 MHz  FILTER 125.0 kHz  +10 dB");
    LS_CHECK(strstr(out[1], "RATE 20.8k/s, 48us a read") == out[1]);
    LS_CHECK(strstr(out[1], "FLOOR -50 dBFS") != NULL);
    LS_CHECK(has_line(out, n, "EVERY 20.0ms"));
    LS_CHECK(out[3][0] == '[' && strchr(out[3], '#') && strchr(out[3], '.'));
    /* The lengths sit in the 1-3 ms bucket. */
    LS_CHECK(strstr(out[4], "LEN ") == out[4]);
    int bucket_3m = -1;
    sscanf(out[5] + 4 + 3 * 4, "%d", &bucket_3m);
    LS_CHECK(bucket_3m >= 40);
    LS_CHECK(has_line(out, n, "LAST ") && has_line(out, n, " -20"));
    LS_EQ_UINT(sc.gaps, 0);

    /* Lost samples: counted, and the burst they cut marked so. */
    const uint32_t before = sc.log.count;
    uint64_t first = RATE * 5 / 2 + RATE / 2;
    make(first, 2048, 3000, 0.1, -50, train);
    scope_feed(&sc, iq, 2048, first, true);
    LS_EQ_UINT(sc.gaps, 1);
    LS_CHECK(sc.log.count >= before);

    static char idle[2][SCOPE_LINE];
    LS_EQ_INT(scope_idle_lines(315000000u, 250000u, 12, idle, 2), 2);
    LS_EQ_STR(idle[0], "FREQ 315.0000 MHz  FILTER 250.0 kHz  +12 dB");
}
