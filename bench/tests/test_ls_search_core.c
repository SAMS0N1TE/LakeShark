/* LS_TEST_SOURCES: ${FW}/components/lakeshark/dsp/ls_search_core.c ${FW}/components/lakeshark/dsp/ls_sweep_core.c */
/* LS_TEST_INCLUDE: ${FW}/components/lakeshark/dsp */
/* The search's floor, hit, merge and snap logic, and the plan it hops on, on
   the host. */

#include "ls_test.h"
#include "ls_search_core.h"
#include "ls_sweep_core.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N_BINS    800
#define BIN_HZ    5000u
#define START_HZ  154700000ull

/* LOUD is +20 dB over dead air at -60: a strong one. weak() below is the
   other kind. */
#define LOUD      (-40)

/* FFT frames ls_sweep averages per tune; LS_SEARCH_STRONG_DB is worked out
   for it, and ls_search.c holds that with a _Static_assert. */
#define AVG_FRAMES 8

/* Passes of dead air that leave every floor past its warm-up and every
   history clear. */
#define SETTLE    (LS_SEARCH_WARMUP + 2)

static float    g_mem[(N_BINS * LS_SEARCH_BYTES_PER_BIN + 3) / 4];
static int8_t   g_db[N_BINS];
static ls_search_t g_s;
static ls_rng_t g_rng;

static unsigned g_new, g_again;
static ls_search_hit_kind_t g_kinds[256];

static void on_hit(const ls_search_hit_t *h, ls_search_hit_kind_t kind, void *user)
{
    (void)h; (void)user;
    if (kind == LS_SEARCH_HIT_NEW) g_new++; else g_again++;
    if (g_new + g_again <= 256) g_kinds[g_new + g_again - 1] = kind;
}

static void fresh(void)
{
    ls_search_init(&g_s, START_HZ, BIN_HZ, N_BINS, g_mem);
    ls_rng_seed(&g_rng, 12345);
    g_new = g_again = 0;
}

/* Centre of bin k, as the search sees it. */
static uint64_t centre(uint32_t k) { return START_HZ + (uint64_t)BIN_HZ * k + BIN_HZ / 2; }

/* Dead air: -60 dBFS with +-6 dB of noise, bounded under the 9 dB threshold
   so that "never hits" is a property of the logic and not of the seed. */
static void quiet(float level)
{
    for (int i = 0; i < N_BINS; i++)
        g_db[i] = (int8_t)lroundf(level + 6.0f * ls_rng_noise(&g_rng));
}

/* A bin reading `over` dB above what its floor has learned. */
static int8_t above(int bin, float over)
{
    return (int8_t)lroundf(g_s.floor[bin] + over);
}

/* Over HIT_DB and under STRONG_DB, so only ever a hit by the two-look rule.
   Relative to the floor as it stands: a floor that is still a little off
   from the dead air it has seen does not decide what these tests mean. */
static int8_t weak(int bin)
{
    return above(bin, 10.5f);
}

/* What one noise bin reads out of ls_sweep: AVG_FRAMES frames of exponential
   noise power averaged, in dB about a mean power of `level_db`. */
static float averaged_noise_db(float level_db)
{
    double sum = 0.0;
    for (int f = 0; f < AVG_FRAMES; f++)
        sum += -log(((double)ls_rng_u32(&g_rng) + 1.0) / 4294967296.0);
    return level_db + 10.0f * (float)log10(sum / AVG_FRAMES);
}

/* Uptime is 100 s plus the pass number, so a hit's seconds are checkable. */
static void pass(void)
{
    ls_search_feed(&g_s, g_db, 101u + g_s.pass, on_hit, NULL);
}

static void settle(int n)
{
    for (int p = 0; p < n; p++) { quiet(-60.0f); pass(); }
}

/* ---------------------------------------------------------------- snap */

LS_CASE(snap_lands_on_the_2500_hz_raster)
{
    LS_EQ_UINT(ls_search_snap(154786100ull), 154785000ull);
    LS_EQ_UINT(ls_search_snap(154786300ull), 154787500ull);
    LS_EQ_UINT(ls_search_snap(154785000ull), 154785000ull);
    LS_EQ_UINT(ls_search_snap(162550000ull), 162550000ull);
}

/* --------------------------------------------------------------- floors */

LS_CASE(noise_only_bins_never_hit)
{
    fresh();
    for (int p = 0; p < 500; p++) { quiet(-60.0f); pass(); }
    LS_EQ_UINT(g_s.n_hits, 0u);
    LS_EQ_UINT(g_new, 0u);
}

LS_CASE(floor_tracks_a_slow_rise_without_hitting)
{
    /* The whole band comes up 20 dB over 400 passes (0.05 dB a pass): a warm
       front end, a neighbour's supply. A floor that did not follow would call
       every bin a signal by the end. */
    fresh();
    for (int p = 0; p < 400; p++) { quiet(-70.0f + 0.05f * (float)p); pass(); }
    LS_EQ_UINT(g_s.n_hits, 0u);
    LS_EQ_UINT(g_new, 0u);
    LS_CHECK(g_s.floor[100] > -53.0f && g_s.floor[100] < -49.0f);
    LS_CHECK(g_s.floor[300] > -53.0f && g_s.floor[300] < -49.0f);
}

LS_CASE(a_blip_is_not_a_hit)
{
    /* One look over HIT_DB, or two that are not consecutive, is a click. */
    fresh();
    settle(SETTLE);
    for (int p = 1; p <= 10; p++) {
        quiet(-60.0f);
        if (p == 5 || p == 7) g_db[200] = above(200, LS_SEARCH_HIT_DB + 1.0f);
        pass();
    }
    LS_EQ_UINT(g_s.n_hits, 0u);
}

LS_CASE(bins_with_no_reading_never_hit_and_learn_a_floor_late)
{
    fresh();
    for (int p = 0; p < 4; p++) { memset(g_db, LS_SWEEP_NO_DATA, sizeof(g_db)); pass(); }
    LS_EQ_UINT(g_s.n_hits, 0u);
    for (int p = 0; p < 4; p++) { quiet(-60.0f); pass(); }
    LS_EQ_UINT(g_s.n_hits, 0u);
    LS_CHECK(g_s.floor[10] > -70.0f && g_s.floor[10] < -50.0f);
}

/* -------------------------------------------------------------- warm-up */

LS_CASE(a_carrier_that_is_there_from_the_start_is_the_baseline_not_a_hit)
{
    /* On the air from the first pass and never off: during warm-up the floor
       follows everything, so after it the carrier is part of the floor. This
       is the spur comb that came through at passes 5 and 6 on the board. */
    fresh();
    for (int p = 1; p <= 2 * LS_SEARCH_WARMUP; p++) {
        quiet(-60.0f);
        g_db[50] = LOUD;
        pass();
    }
    LS_EQ_UINT(g_new, 0u);
    LS_EQ_UINT(g_s.n_hits, 0u);
    LS_CHECK(g_s.floor[50] > -45.0f);               /* learned to within a few dB */

    /* A change on top of that baseline is still seen. */
    quiet(-60.0f);
    g_db[50] = (int8_t)(LOUD + 20);
    pass();
    LS_EQ_UINT(g_new, 1u);
}

LS_CASE(no_hit_of_either_path_until_the_floor_has_settled)
{
    /* A burst on the last look of warm-up is ignored, on the next one not. */
    fresh();
    for (int p = 1; p <= LS_SEARCH_WARMUP; p++) {
        quiet(-60.0f);
        if (p == LS_SEARCH_WARMUP) g_db[200] = LOUD;
        pass();
    }
    LS_EQ_UINT(g_s.n_hits, 0u);

    quiet(-60.0f);
    g_db[300] = LOUD;
    pass();
    LS_EQ_UINT(g_new, 1u);
    LS_EQ_UINT(g_s.hit[0].freq_hz, centre(300));
    LS_EQ_UINT(g_s.hit[0].first_pass, LS_SEARCH_WARMUP + 1u);
}

/* ----------------------------------------------------------------- hits */

LS_CASE(a_steady_carrier_hits_once_and_counts_up)
{
    fresh();
    settle(SETTLE);
    for (int p = 1; p <= 6; p++) {
        quiet(-60.0f);
        g_db[50] = weak(50);
        pass();
    }
    LS_EQ_UINT(g_new, 1u);
    LS_EQ_UINT(g_again, 0u);
    LS_EQ_UINT(g_s.n_hits, 1u);

    const ls_search_hit_t *h = &g_s.hit[0];
    LS_CHECK(h->path == LS_SEARCH_PATH_STEADY);
    LS_CHECK(!h->constant);
    LS_EQ_UINT(h->freq_hz, centre(50));
    LS_EQ_UINT(h->count, 5u);               /* confirmed on its passes 2..6 */
    LS_EQ_UINT(h->first_pass, SETTLE + 2u);
    LS_EQ_UINT(h->last_pass, SETTLE + 6u);
    LS_EQ_UINT(h->first_s, 100u + SETTLE + 2u);
    LS_EQ_UINT(h->last_s, 100u + SETTLE + 6u);
    LS_CHECK(h->max_db >= 8.5f && h->max_db <= 11.5f);
    LS_NEAR(ls_search_duty(&g_s, h), 1.0, 0.001);
}

LS_CASE(adjacent_hit_bins_merge_into_one_hit)
{
    fresh();
    settle(SETTLE);
    for (int p = 1; p <= 4; p++) {
        quiet(-60.0f);
        g_db[50] = weak(50); g_db[51] = weak(51); g_db[52] = weak(52);
        pass();
    }
    LS_EQ_UINT(g_new, 1u);
    LS_EQ_UINT(g_s.n_hits, 1u);
    LS_EQ_UINT(g_s.hit[0].freq_hz, centre(51));     /* the middle of three */
}

LS_CASE(bins_with_a_quiet_one_between_are_two_hits)
{
    fresh();
    settle(SETTLE);
    for (int p = 1; p <= 4; p++) {
        quiet(-60.0f);
        g_db[50] = weak(50);
        g_db[60] = weak(60);
        pass();
    }
    LS_EQ_UINT(g_new, 2u);
    LS_EQ_UINT(g_s.n_hits, 2u);
}

LS_CASE(the_centre_is_weighted_and_snapped_to_the_raster)
{
    /* Two bins, the upper one 4 dB hotter (so a strong hit on the first
       look). The centroid is 154.78608 MHz, and the hit is reported on the
       raster at 154.785000. Constant dead air so the arithmetic is exact. */
    fresh();
    for (int p = 1; p <= SETTLE + 2; p++) {
        memset(g_db, -60, sizeof(g_db));
        if (p > SETTLE) { g_db[16] = -50; g_db[17] = -46; }
        pass();
    }
    LS_EQ_UINT(g_s.n_hits, 1u);
    LS_EQ_UINT(g_s.hit[0].freq_hz, 154785000u);
}

LS_CASE(a_signal_that_comes_back_is_reported_again_a_flicker_is_not)
{
    fresh();
    settle(SETTLE);
    /* On 1..6, gone 7..20, on again 21..24. */
    for (int p = 1; p <= 24; p++) {
        quiet(-60.0f);
        if (p <= 6 || p >= 21) g_db[120] = weak(120);
        pass();
    }
    LS_EQ_UINT(g_new, 1u);
    LS_EQ_UINT(g_again, 1u);
    LS_EQ_UINT(g_s.n_hits, 1u);
    LS_CHECK(g_kinds[0] == LS_SEARCH_HIT_NEW);
    LS_CHECK(g_kinds[1] == LS_SEARCH_HIT_AGAIN);

    /* One pass in three missing never produces an AGAIN. */
    fresh();
    settle(SETTLE);
    for (int p = 1; p <= 30; p++) {
        quiet(-60.0f);
        if (p % 3 != 0) g_db[120] = weak(120);
        pass();
    }
    LS_EQ_UINT(g_new, 1u);
    LS_EQ_UINT(g_again, 0u);
}

LS_CASE(duty_is_the_fraction_of_passes_it_was_there)
{
    fresh();
    settle(SETTLE);
    for (int p = 1; p <= 20; p++) {
        quiet(-60.0f);
        if (p <= 10) g_db[80] = weak(80);
        pass();
    }
    LS_EQ_UINT(g_s.hit[0].count, 9u);               /* its passes 2..10 */
    LS_NEAR(ls_search_duty(&g_s, &g_s.hit[0]), 9.0 / 19.0, 0.001);
}

LS_CASE(top_lists_what_is_not_constant_first_then_the_most_seen)
{
    fresh();
    settle(SETTLE);
    for (int p = 1; p <= 12; p++) {
        quiet(-60.0f);
        if (p <= 4)  g_db[40] = weak(40);               /* seen 3 times */
        g_db[90] = weak(90);                            /* always there: constant */
        if (p >= 8) g_db[140] = weak(140);               /* seen 4 times */
        pass();
    }
    uint32_t idx[8];
    uint32_t n = ls_search_top(&g_s, idx, 8);
    LS_EQ_UINT(n, 3u);
    LS_EQ_UINT(g_s.hit[idx[0]].freq_hz, centre(140));
    LS_EQ_UINT(g_s.hit[idx[1]].freq_hz, centre(40));
    LS_EQ_UINT(g_s.hit[idx[2]].freq_hz, centre(90));
    LS_CHECK(g_s.hit[idx[2]].constant);
    LS_EQ_UINT(ls_search_n_constant(&g_s), 1u);
    LS_EQ_UINT(ls_search_top(&g_s, idx, 2), 2u);
}

LS_CASE(reset_forgets_floors_and_hits)
{
    fresh();
    settle(SETTLE);
    for (int p = 1; p <= 4; p++) { quiet(-60.0f); g_db[50] = weak(50); pass(); }
    LS_EQ_UINT(g_s.n_hits, 1u);
    ls_search_reset(&g_s);
    LS_EQ_UINT(g_s.n_hits, 0u);
    LS_EQ_UINT(g_s.pass, 0u);
    LS_CHECK(g_s.floor[50] < -150.0f);
    LS_EQ_UINT(g_s.looks[50], 0u);
}

/* ------------------------------------------------------------- constants */

LS_CASE(a_bin_over_on_most_of_its_looks_is_constant_and_not_announced_again)
{
    /* On from the first pass after a settled search. Constant is seven of the
       last eight looks over: not on its sixth, on its seventh. */
    fresh();
    settle(SETTLE);
    for (int p = 1; p <= 30; p++) {
        quiet(-60.0f);
        g_db[50] = weak(50);
        pass();
        if (p == 6) LS_CHECK(!g_s.hit[0].constant);
        if (p == 7) LS_CHECK(g_s.hit[0].constant);
    }
    LS_EQ_UINT(g_new, 1u);                          /* one line, ever */
    LS_EQ_UINT(g_again, 0u);
    LS_EQ_UINT(g_s.n_hits, 1u);
    LS_CHECK(g_s.hit[0].constant);
    LS_EQ_UINT(ls_search_n_constant(&g_s), 1u);
}

LS_CASE(a_constant_is_learned_into_its_floor_slowly)
{
    /* A +20 dB spur that comes on after warm-up: one line, flagged constant
       once seven of eight looks have seen it, and its floor creeping up on it
       at 1/128 a pass rather than staying put or giving way at once. */
    fresh();
    settle(SETTLE);
    for (int p = 1; p <= 40; p++) {
        quiet(-60.0f);
        g_db[50] = LOUD;
        pass();
        if (p == 6) LS_CHECK(!g_s.hit[0].constant);
        if (p == 7) LS_CHECK(g_s.hit[0].constant);
    }
    LS_EQ_UINT(g_new, 1u);
    LS_EQ_UINT(g_again, 0u);
    LS_CHECK(g_s.hit[0].constant);
    LS_CHECK(g_s.hit[0].path == LS_SEARCH_PATH_STRONG);
    LS_EQ_UINT(g_s.hit[0].count, 40u);
    LS_CHECK(g_s.floor[50] > -57.0f && g_s.floor[50] < -52.0f);
}

LS_CASE(a_full_table_of_constants_does_not_turn_over)
{
    fresh();
    settle(SETTLE);
    /* 140 spurs, 5 bins apart, all coming on together: more than the table
       holds. For the first few looks they are new signals and the table
       churns; once seven of eight looks have seen them they are constants. */
    for (int p = 1; p <= 12; p++) {
        quiet(-60.0f);
        for (int j = 0; j < 140; j++) g_db[2 + 5 * j] = LOUD;
        pass();
    }
    LS_EQ_UINT(g_s.n_hits, LS_SEARCH_MAX_HITS);
    LS_EQ_UINT(ls_search_n_constant(&g_s), (unsigned)LS_SEARCH_MAX_HITS);

    /* From here the table does not turn over: no new lines, constants that
       do not fit are counted and dropped. */
    unsigned lines = g_new, dropped = g_s.dropped;
    for (int p = 13; p <= 30; p++) {
        quiet(-60.0f);
        for (int j = 0; j < 140; j++) g_db[2 + 5 * j] = LOUD;
        pass();
    }
    LS_EQ_UINT(g_new, lines);
    LS_EQ_UINT(g_again, 0u);
    LS_CHECK(g_s.dropped > dropped);
    LS_EQ_UINT(g_s.n_hits, LS_SEARCH_MAX_HITS);

    /* A newcomer that is not constant takes the place of one of them. */
    quiet(-60.0f);
    for (int j = 0; j < 140; j++) g_db[2 + 5 * j] = LOUD;
    g_db[790] = above(790, LS_SEARCH_STRONG_DB + 2.0f);
    pass();
    LS_EQ_UINT(g_new, lines + 1u);
    LS_EQ_UINT(g_s.n_hits, LS_SEARCH_MAX_HITS);
    bool found = false;
    for (uint32_t i = 0; i < g_s.n_hits; i++)
        if (g_s.hit[i].freq_hz == centre(790)) found = true;
    LS_CHECK(found);
}

LS_CASE(a_full_table_drops_the_least_seen_and_never_overruns)
{
    fresh();
    settle(SETTLE);
    /* 140 signals that are not constant (on for four passes), 5 bins apart:
       more than the table holds. */
    for (int p = 1; p <= 4; p++) {
        quiet(-60.0f);
        for (int j = 0; j < 140; j++) g_db[2 + 5 * j] = weak(2 + 5 * j);
        pass();
    }
    LS_EQ_UINT(g_s.n_hits, LS_SEARCH_MAX_HITS);
    LS_EQ_UINT(ls_search_n_constant(&g_s), 0u);

    /* Quiet again, so every history clears. A newcomer takes the place of
       one that was seen least. */
    settle(10);
    for (int p = 1; p <= 3; p++) {
        quiet(-60.0f);
        g_db[790] = weak(790);
        pass();
    }
    LS_EQ_UINT(g_s.n_hits, LS_SEARCH_MAX_HITS);
    bool found = false;
    for (uint32_t i = 0; i < g_s.n_hits; i++)
        if (g_s.hit[i].freq_hz == centre(790)) found = true;
    LS_CHECK(found);
}

/* ---------------------------------------------------------- strong path */

LS_CASE(a_one_pass_burst_over_strong_db_is_a_hit_on_that_pass)
{
    fresh();
    settle(SETTLE);
    LS_EQ_UINT(g_new, 0u);

    quiet(-60.0f);
    g_db[200] = above(200, LS_SEARCH_STRONG_DB + 1.0f);
    pass();                                         /* one look, the first past warm-up */
    LS_EQ_UINT(g_new, 1u);
    LS_EQ_UINT(g_s.n_hits, 1u);

    const ls_search_hit_t *h = &g_s.hit[0];
    LS_CHECK(h->path == LS_SEARCH_PATH_STRONG);
    LS_CHECK(!h->constant);
    LS_EQ_UINT(h->freq_hz, centre(200));
    LS_EQ_UINT(h->count, 1u);
    LS_EQ_UINT(h->first_pass, SETTLE + 1u);
    LS_EQ_UINT(h->last_pass, SETTLE + 1u);
    LS_CHECK(h->max_db >= LS_SEARCH_STRONG_DB);

    quiet(-60.0f); pass();                          /* gone; it stays listed */
    LS_EQ_UINT(g_new, 1u);
    LS_EQ_UINT(g_s.hit[0].count, 1u);
    LS_EQ_STR(ls_search_path_name(h->path), "strong");
}

LS_CASE(a_one_pass_burst_over_hit_db_only_is_not_a_hit)
{
    fresh();
    settle(SETTLE);
    for (int p = 1; p <= 12; p++) {
        quiet(-60.0f);
        /* On passes 6, 8 and 10, on three different bins: always one look. */
        if (p == 6)  g_db[100] = above(100, LS_SEARCH_HIT_DB + 1.0f);
        if (p == 8)  g_db[200] = above(200, LS_SEARCH_HIT_DB + 1.0f);
        if (p == 10) g_db[300] = above(300, LS_SEARCH_HIT_DB + 1.0f);
        pass();
    }
    LS_EQ_UINT(g_new, 0u);
    LS_EQ_UINT(g_s.n_hits, 0u);

    /* The same level on two passes in a row is still the steady rule's. */
    fresh();
    settle(SETTLE);
    for (int p = 1; p <= 8; p++) {
        quiet(-60.0f);
        if (p == 6 || p == 7) g_db[100] = above(100, LS_SEARCH_HIT_DB + 1.0f);
        pass();
    }
    LS_EQ_UINT(g_new, 1u);
    LS_CHECK(g_s.hit[0].path == LS_SEARCH_PATH_STEADY);
    LS_EQ_STR(ls_search_path_name(g_s.hit[0].path), "steady");
}

LS_CASE(a_strong_bin_takes_its_skirts_with_it)
{
    /* The loud bin and the two beside it that are over HIT_DB but not
       STRONG_DB are one signal, centred on the loud one. */
    fresh();
    settle(SETTLE);
    for (int p = 1; p <= 3; p++) {
        quiet(-60.0f);
        g_db[99]  = above(99,  LS_SEARCH_HIT_DB + 1.0f);
        g_db[100] = above(100, LS_SEARCH_STRONG_DB + 8.0f);
        g_db[101] = above(101, LS_SEARCH_HIT_DB + 1.0f);
        pass();
    }
    LS_EQ_UINT(g_s.n_hits, 1u);
    LS_EQ_UINT(g_s.hit[0].freq_hz, centre(100));
    LS_CHECK(g_s.hit[0].path == LS_SEARCH_PATH_STRONG);
}

LS_CASE(noise_averaged_like_the_sweep_never_makes_a_strong_hit)
{
    /* 800 bins x 1000 passes = 800 000 looks, over ten passes' worth of a
       whole 136-174 plan. Noise is AVG_FRAMES exponential power frames
       averaged, as the sweep delivers it, not a bounded stand-in. */
    fresh();
    double sum = 0.0, sumsq = 0.0, worst = -1000.0;
    long n = 0;
    float v[N_BINS];
    for (int p = 0; p < 1000; p++) {
        for (int i = 0; i < N_BINS; i++) {
            v[i] = averaged_noise_db(-60.0f);
            g_db[i] = (int8_t)lroundf(v[i]);
            sum += v[i]; sumsq += (double)v[i] * v[i]; n++;
        }
        pass();
        if (p < LS_SEARCH_WARMUP + 5) continue;     /* a young floor wanders a bit more */
        for (int i = 0; i < N_BINS; i++) {
            double over = g_db[i] - g_s.floor[i];
            if (over > worst) worst = over;
        }
    }
    LS_EQ_UINT(g_new, 0u);
    LS_EQ_UINT(g_s.n_hits, 0u);

    /* The generator really is the 8-frame model: mean 0.28 dB under the mean
       power, sd 1.58 dB. */
    double mean = sum / (double)n;
    double sd = sqrt(sumsq / (double)n - mean * mean);
    LS_NEAR(mean, -60.277, 0.02);
    LS_NEAR(sd, 1.585, 0.03);

    /* And the worst bin of all of them is nowhere near either threshold. */
    LS_CHECK_MSG(worst < LS_SEARCH_HIT_DB - 2.0f, "worst bin %.2f dB over", worst);
}

/* ------------------------------------------------------------------ dump */

/* Two hex digits at `p`. */
static int hex2(const char *p)
{
    char t[3] = { p[0], p[1], 0 };
    return (int)strtol(t, NULL, 16);
}

LS_CASE(hits_closer_than_12_5_khz_are_one_hit)
{
    /* POCSAG on 152.600 shows as two bins with a dip between: 10 kHz apart.
       One signal, centred between them. */
    fresh();
    settle(SETTLE);
    for (int p = 1; p <= 4; p++) {
        quiet(-60.0f);
        g_db[50] = weak(50);
        g_db[52] = weak(52);
        pass();
    }
    LS_EQ_UINT(g_new, 1u);
    LS_EQ_UINT(g_s.n_hits, 1u);
    LS_EQ_UINT(g_s.hit[0].freq_hz, centre(51));

    /* Two bins that come on in different passes, 10 kHz apart, are still the
       one hit. */
    fresh();
    settle(SETTLE);
    for (int p = 1; p <= 4; p++) { quiet(-60.0f); g_db[50] = weak(50); pass(); }
    for (int p = 1; p <= 4; p++) { quiet(-60.0f); g_db[52] = weak(52); pass(); }
    LS_EQ_UINT(g_s.n_hits, 1u);
    LS_EQ_UINT(g_new, 1u);

    /* 15 kHz apart is two. */
    fresh();
    settle(SETTLE);
    for (int p = 1; p <= 4; p++) {
        quiet(-60.0f);
        g_db[50] = weak(50);
        g_db[53] = weak(53);
        pass();
    }
    LS_EQ_UINT(g_s.n_hits, 2u);
}

LS_CASE(constant_stays_constant_while_it_is_there)
{
    /* A spur riding its threshold as the floor creeps up: two looks over, one
       not, so never seven of eight. Once it has been constant it is, until it
       has been gone. */
    fresh();
    settle(SETTLE);
    for (int p = 1; p <= 12; p++) {
        quiet(-60.0f);
        g_db[50] = weak(50);
        pass();
    }
    LS_CHECK(g_s.hit[0].constant);
    for (int p = 1; p <= 20; p++) {
        quiet(-60.0f);
        if (p % 3) g_db[50] = weak(50);             /* two looks in three */
        pass();
        LS_CHECK(g_s.hit[0].constant);
    }
    LS_EQ_UINT(g_new, 1u);
    LS_EQ_UINT(g_again, 0u);

    /* Gone for a while, back as a signal: judged afresh, and news. */
    for (int p = 1; p <= 20; p++) { quiet(-60.0f); pass(); }
    for (int p = 1; p <= 3; p++) { quiet(-60.0f); g_db[50] = weak(50); pass(); }
    LS_CHECK(!g_s.hit[0].constant);
    LS_EQ_UINT(g_again, 1u);
}

LS_CASE(the_dump_is_one_line_per_tune_and_covers_every_bin)
{
    /* The geometry the board runs: 136-174 MHz, 7600 bins, 51 tunes. */
    static float   mem[(7600 * LS_SEARCH_BYTES_PER_BIN + 3) / 4];
    static int8_t  db[7600];
    static char    line[2048];
    ls_sweep_plan_t plan;
    LS_CHECK(ls_search_plan(136000000ull, 174000000ull, &plan));
    LS_EQ_UINT(plan.n_bins, 7600u);

    ls_search_t s;
    ls_search_init(&s, plan.start_hz, plan.bin_hz, plan.n_bins, mem);
    ls_rng_t rng;
    ls_rng_seed(&rng, 99);
    for (int p = 1; p <= LS_SEARCH_WARMUP + 3; p++) {
        for (uint32_t i = 0; i < plan.n_bins; i++)
            db[i] = (int8_t)lroundf(-60.0f + 4.0f * ls_rng_noise(&rng));
        if (p > LS_SEARCH_WARMUP) db[1234] = LOUD;
        ls_search_feed(&s, db, 100u + (unsigned)p, NULL, NULL);
    }
    LS_CHECK(s.n_hits >= 1u);

    uint32_t per_line = ls_search_dump_per_line(&plan);
    LS_EQ_UINT(per_line, 150u);
    size_t cap = ls_search_dump_cap(per_line);
    LS_CHECK(cap <= sizeof(line));

    uint32_t cursor = 0, lines = 0, bins = 0;
    for (;;) {
        size_t n = ls_search_dump_next(&s, db, per_line, &cursor, line, cap);
        if (!n) break;
        LS_CHECK(strncmp(line, "SEARCH-DUMP D ", 14) == 0);
        LS_EQ_UINT(n, strlen(line));
        unsigned pass_no = 0, count = 0;
        unsigned long hz = 0, step = 0;
        LS_EQ_INT(sscanf(line, "SEARCH-DUMP D %u %lu %lu %u F", &pass_no, &hz, &step, &count), 4);
        LS_EQ_UINT(pass_no, (unsigned)LS_SEARCH_WARMUP + 3u);
        LS_EQ_UINT(step, 5000u);
        LS_EQ_UINT(hz, plan.start_hz + 2500u + 5000u * bins);
        /* hex floors, hex last looks, digit history, each `count` long */
        const char *f = strstr(line, " F") + 2;
        const char *l = strstr(line, " L");
        const char *h = strstr(line, " H");
        LS_CHECK(f && l && h);
        LS_EQ_UINT((unsigned)(l - f), 2u * count);
        LS_EQ_UINT((unsigned)(h - (l + 2)), 2u * count);
        LS_EQ_UINT((unsigned)strlen(h + 2), count);
        bins += count;
        lines++;
    }
    LS_EQ_UINT(lines, plan.n_tunes);                /* one line per tune */
    LS_EQ_UINT(lines, 51u);
    LS_EQ_UINT(bins, plan.n_bins);                  /* and no bin left out */
    LS_EQ_UINT(cursor, plan.n_bins);

    /* A smaller range: still one line per tune. */
    LS_CHECK(ls_search_plan(154000000ull, 159000000ull, &plan));
    ls_search_init(&s, plan.start_hz, plan.bin_hz, plan.n_bins, mem);
    memset(db, -60, sizeof(db));
    ls_search_feed(&s, db, 1u, NULL, NULL);
    cursor = 0; lines = 0;
    while (ls_search_dump_next(&s, db, ls_search_dump_per_line(&plan), &cursor,
                               line, ls_search_dump_cap(150u)))
        lines++;
    LS_EQ_UINT(lines, plan.n_tunes);
}

LS_CASE(a_dump_line_carries_floor_last_look_and_history)
{
    fresh();
    for (int p = 1; p <= SETTLE + 3; p++) {
        memset(g_db, -60, sizeof(g_db));
        if (p > SETTLE) g_db[5] = -50;              /* over on three looks */
        pass();
    }
    int8_t last[N_BINS];
    memset(last, -50, sizeof(last));
    last[4] = LS_SWEEP_NO_DATA;

    char line[512];
    size_t n = ls_search_dump_line(&g_s, last, 3, 5, line, sizeof(line));
    LS_CHECK(n > 0);
    LS_EQ_UINT(n, strlen(line));

    /* SEARCH-DUMP D <pass> <hz of bin 3's centre> <step> <count> F.. L.. H.. */
    char head[80];
    snprintf(head, sizeof(head), "SEARCH-DUMP D %u %lu 5000 5 F",
             (unsigned)(SETTLE + 3),
             (unsigned long)(START_HZ + 3 * BIN_HZ + BIN_HZ / 2));
    LS_CHECK_MSG(strncmp(line, head, strlen(head)) == 0, "head [%s]", line);

    const char *f = line + strlen(head);                /* floor -60 -> (40*2)=0x50 */
    for (int k = 0; k < 5; k++) LS_EQ_INT(hex2(f + 2 * k), 0x50);
    const char *l = f + 10 + 2;                         /* " L" */
    LS_EQ_INT(hex2(l + 0), 0x64);                       /* -50 -> 100 */
    LS_EQ_INT(hex2(l + 2), 0x00);                       /* bin 4: no reading */
    LS_EQ_INT(hex2(l + 4), 0x64);
    const char *h = l + 10 + 2;                         /* " H" */
    LS_EQ_INT(h[0], '0');
    LS_EQ_INT(h[1], '0');
    LS_EQ_INT(h[2], '3');                               /* bin 5: three of eight */
    LS_EQ_INT(h[3], '0');
    LS_EQ_INT(h[4], '0');
    LS_EQ_INT(h[5], 0);

    /* Does not fit, or is not a range of the bins. */
    LS_EQ_UINT(ls_search_dump_line(&g_s, last, 3, 5, line, 40), 0u);
    LS_EQ_UINT(ls_search_dump_line(&g_s, last, N_BINS, 1, line, sizeof(line)), 0u);
    LS_EQ_UINT(ls_search_dump_line(&g_s, last, N_BINS - 2, 5, line, sizeof(line)), 0u);
    LS_EQ_UINT(ls_search_dump_line(&g_s, last, 0, 0, line, sizeof(line)), 0u);
}

/* ------------------------------------------------------------ the plan */

#define FFT_N 512

static int8_t g_out[LS_SWEEP_MAX_BINS];

/* Fold one FFT-shaped spectrum, `shape(k)` dB at bin k, into every tune of
   the plan, as ls_sweep_run does. */
static void fold_all(const ls_sweep_plan_t *p, float (*shape)(int k))
{
    static float fft[FFT_N];
    for (int k = 0; k < FFT_N; k++) fft[k] = shape(k);
    ls_sweep_reset(p, g_out);
    for (uint32_t t = 0; t < p->n_tunes; t++)
        ls_sweep_fold(p, ls_sweep_tune_center(p, t), fft, FFT_N, g_out);
}

static float flat_shape(int k) { (void)k; return -60.0f; }

/* A spike on the DC bin and the LS_SEARCH_DC_BINS either side of it. */
static float dc_shape(int k)
{
    return abs(k - FFT_N / 2) <= LS_SEARCH_DC_BINS ? -20.0f : -60.0f;
}

/* 20 dB up on everything past the trusted 75% of the band. */
static float edge_shape(int k) { return abs(k - FFT_N / 2) > 192 ? -20.0f : -60.0f; }

/* The dB IS the offset from the tune's centre: -100 + kHz / 10. */
static float offset_shape(int k)
{
    return -100.0f + (float)abs(k - FFT_N / 2) * (2400.0f / FFT_N) / 10.0f;
}

LS_CASE(the_search_plan_is_the_overlapped_one)
{
    ls_sweep_plan_t p;
    LS_CHECK(ls_search_plan(136000000ull, 174000000ull, &p));
    LS_EQ_UINT(p.n_bins, 7600u);
    LS_EQ_UINT(p.hop_hz, 750000u);
    LS_EQ_UINT(p.dc_guard_hz, 42187u);                  /* 9 FFT bins */
    LS_EQ_UINT(p.half_span_hz, 900000u);                /* the central 75% */
    LS_EQ_UINT(p.n_tunes, 51u);                         /* 45 in the classic plan */
    LS_EQ_UINT(ls_sweep_tune_center(&p, 0), 136375000ull);
    LS_EQ_UINT(ls_sweep_tune_center(&p, 50), 173875000ull);

    LS_CHECK(!ls_search_plan(174000000ull, 136000000ull, &p));
    LS_CHECK(!ls_sweep_plan_overlap(136000000ull, 174000000ull, 5000u, 2400000u,
                                    75, 42187u, 880000u, &p));  /* holes out of reach */
    LS_CHECK(!ls_sweep_plan_overlap(136000000ull, 174000000ull, 5000u, 2400000u,
                                    75, 42187u, 90000u, &p));   /* holes meet */
}

LS_CASE(the_overlapped_plan_stitches_with_no_gaps_for_any_range)
{
    static const struct { uint64_t lo, hi; } r[] = {
        { 136000000ull, 174000000ull }, { 154000000ull, 159000000ull },
        { 155000000ull, 155100000ull }, { 144000000ull, 148000000ull },
        { 162400000ull, 162700000ull }, { 118000000ull, 137000000ull },
        { 150000000ull, 150000001ull }, { 100000000ull, 300000000ull },
    };
    for (unsigned i = 0; i < sizeof(r) / sizeof(r[0]); i++) {
        ls_sweep_plan_t p;
        LS_CHECK(ls_search_plan(r[i].lo, r[i].hi, &p));
        LS_CHECK(p.n_tunes >= 2);
        fold_all(&p, flat_shape);
        LS_CHECK_MSG(ls_sweep_gaps(&p, g_out) == 0, "range %u has %u gaps", i,
                     (unsigned)ls_sweep_gaps(&p, g_out));
    }
}

LS_CASE(the_dc_spike_of_every_tune_is_masked_and_still_no_gaps)
{
    ls_sweep_plan_t p;
    LS_CHECK(ls_search_plan(136000000ull, 174000000ull, &p));
    fold_all(&p, dc_shape);
    LS_EQ_UINT(ls_sweep_gaps(&p, g_out), 0u);
    for (uint32_t i = 0; i < p.n_bins; i++)
        LS_CHECK_MSG(g_out[i] == -60, "bin %u carries the DC spike (%d)",
                     (unsigned)i, g_out[i]);
}

LS_CASE(nothing_past_the_trusted_part_of_a_tune_is_used)
{
    ls_sweep_plan_t p;
    LS_CHECK(ls_search_plan(136000000ull, 174000000ull, &p));
    fold_all(&p, edge_shape);
    LS_EQ_UINT(ls_sweep_gaps(&p, g_out), 0u);
    for (uint32_t i = 0; i < p.n_bins; i++)
        LS_CHECK_MSG(g_out[i] == -60, "bin %u comes from the band edge", (unsigned)i);
}

LS_CASE(each_bin_comes_from_the_central_part_of_the_tune_that_owns_it)
{
    ls_sweep_plan_t p;
    LS_CHECK(ls_search_plan(136000000ull, 174000000ull, &p));
    fold_all(&p, offset_shape);
    LS_EQ_UINT(ls_sweep_gaps(&p, g_out), 0u);

    /* The reading is its offset: at most a hop plus the hole from the
       neighbour (about 800 kHz), and for nearly all of them under 400. */
    uint32_t far = 0;
    for (uint32_t i = 0; i < p.n_bins; i++) {
        LS_CHECK_MSG(g_out[i] <= -100 + 81, "bin %u read at %d", (unsigned)i, g_out[i]);
        if (g_out[i] > -100 + 40) far++;
    }
    LS_CHECK_MSG(far * 100u < p.n_bins * 14u, "%u of %u bins are from beyond 400 kHz",
                 (unsigned)far, (unsigned)p.n_bins);
}

LS_CASE(every_bin_has_exactly_one_owner_and_it_is_never_in_its_own_hole)
{
    ls_sweep_plan_t p;
    LS_CHECK(ls_search_plan(136000000ull, 174000000ull, &p));
    for (uint32_t b = 0; b < p.n_bins; b++) {
        uint32_t o = ls_sweep_bin_owner(&p, b);
        LS_CHECK(o < p.n_tunes);
        int64_t fc = (int64_t)p.start_hz + (int64_t)b * p.bin_hz + p.bin_hz / 2;
        int64_t off = fc - (int64_t)ls_sweep_tune_center(&p, o);
        if (off < 0) off = -off;
        LS_CHECK_MSG(off > (int64_t)p.dc_guard_hz + p.bin_hz, "bin %u in the DC hole", (unsigned)b);
        LS_CHECK_MSG(off <= (int64_t)p.half_span_hz - 2 * (int64_t)p.bin_hz,
                     "bin %u too far out", (unsigned)b);
    }
}
