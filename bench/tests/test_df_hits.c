#include "ls_test.h"
#include "ls_df_hits.h"
#include <math.h>

static ls_df_log_t g;
static const ls_df_hit_cfg_t CFG = { .threshold_db = 6, .gap_s = 1.5f };

static void quiet(int track, int n, int64_t *t)
{
    for (int i = 0; i < n; i++, *t += 100000) ls_df_log_feed(&g, &CFG, track, 915000000, -100.0f + (i & 1), 10, 0, *t);
}

LS_CASE(a_burst_over_the_floor_is_one_hit_with_its_peak_heading)
{
    ls_df_log_clear(&g);
    int64_t t = 0;
    quiet(0, 20, &t);
    LS_EQ_INT(ls_df_log_count(&g), 0);
    ls_df_log_feed(&g, &CFG, 0, 915000000, -90, 40, 0, t); t += 100000;
    ls_df_log_feed(&g, &CFG, 0, 915000000, -80, 55, 0, t); t += 100000;
    ls_df_log_feed(&g, &CFG, 0, 915000000, -85, 70, 0, t); t += 100000;
    LS_EQ_INT(ls_df_log_count(&g), 1);
    const ls_df_hit_t *h = ls_df_log_at(&g, 0);
    LS_EQ_INT(h->count, 3);
    LS_NEAR(h->peak, -80, 0.01);
    LS_NEAR(h->heading, 55, 0.01);
    LS_CHECK(h->snr > 15 && h->snr < 25);
    /* The loud readings did not drag the floor up with them. */
    LS_CHECK(ls_df_log_noise(&g, 0) < -97);
}

LS_CASE(a_gap_ends_a_hit_and_the_next_burst_is_a_new_row)
{
    ls_df_log_clear(&g);
    int64_t t = 0;
    quiet(0, 20, &t);
    ls_df_log_feed(&g, &CFG, 0, 915000000, -80, 10, 0, t);
    t += 3000000;
    ls_df_log_feed(&g, &CFG, 0, 915000000, -82, 200, LS_DF_HIT_OVERLOAD, t);
    LS_EQ_INT(ls_df_log_count(&g), 2);
    LS_NEAR(ls_df_log_at(&g, 0)->heading, 200, 0.01);         /* newest first */
    LS_EQ_INT(ls_df_log_at(&g, 0)->flags, LS_DF_HIT_OVERLOAD);
    LS_EQ_INT(ls_df_log_at(&g, 1)->flags, 0);
}

LS_CASE(tracks_keep_their_own_floor_and_hits)
{
    ls_df_log_clear(&g);
    int64_t t = 0;
    quiet(0, 20, &t);
    /* Track 3 sits 30 dB higher: its own floor, so level alone is no hit. */
    for (int i = 0; i < 20; i++, t += 100000) ls_df_log_feed(&g, &CFG, 3, 462562500, -70, 0, 0, t);
    LS_EQ_INT(ls_df_log_count(&g), 0);
    ls_df_log_feed(&g, &CFG, 3, 462562500, -55, 90, 0, t);
    ls_df_log_feed(&g, &CFG, 0, 915000000, -85, 180, 0, t);
    LS_EQ_INT(ls_df_log_count(&g), 2);
    LS_EQ_INT(ls_df_log_at(&g, 1)->track, 3);
    LS_EQ_INT(ls_df_log_at(&g, 0)->track, 0);
}

LS_CASE(nothing_is_a_hit_before_the_floor_settles)
{
    ls_df_log_clear(&g);
    LS_CHECK(!ls_df_log_feed(&g, &CFG, 1, 0, -100, 0, 0, 0));
    LS_CHECK(!ls_df_log_feed(&g, &CFG, 1, 0, -60, 0, 0, 100000));
    LS_EQ_INT(ls_df_log_count(&g), 0);
}

LS_CASE(the_ring_keeps_the_newest)
{
    ls_df_log_clear(&g);
    int64_t t = 0;
    quiet(2, 10, &t);
    for (int i = 0; i < LS_DF_HIT_MAX + 10; i++) {
        quiet(2, 20, &t);                                /* 2 s of quiet: each its own hit */
        ls_df_log_feed(&g, &CFG, 2, 0, -60, (float)i, 0, t);
    }
    LS_EQ_INT(ls_df_log_count(&g), LS_DF_HIT_MAX);
    LS_NEAR(ls_df_log_at(&g, 0)->heading, LS_DF_HIT_MAX + 9, 0.01);
    LS_NEAR(ls_df_log_at(&g, LS_DF_HIT_MAX - 1)->heading, 10, 0.01);
    LS_CHECK(ls_df_log_at(&g, LS_DF_HIT_MAX) == NULL);
}


/* The Flipper's DF beacon: on 250 ms, off 50 ms, read about every 110 ms for
   two minutes. The reads that land in its gaps keep the floor where it is,
   so the beacon is one long hit the whole time and never its own noise. */
LS_CASE(a_keyed_beacon_never_becomes_its_own_floor)
{
    ls_df_log_clear(&g);
    int64_t t = 0;
    quiet(1, 20, &t);
    for (int i = 0; i < 1200; i++, t += 110000) {
        const bool on = t % 300000 < 250000;
        ls_df_log_feed(&g, &CFG, 1, 433920000, on ? -60.0f : -100.0f + (i % 3), 180, 0, t);
    }
    LS_CHECK_MSG(ls_df_log_noise(&g, 1) < -95, "floor %.1f", ls_df_log_noise(&g, 1));
    LS_EQ_INT(ls_df_log_count(&g), 1);
    const ls_df_hit_t *h = ls_df_log_at(&g, 0);
    LS_CHECK(h->snr > 35);
    LS_CHECK(h->count > 900);
}

/* A carrier that never stops, heard alike from every side, is no
   direction at all: after the window it is the floor, and not a hit. */
LS_CASE(a_steady_carrier_everywhere_settles_into_the_floor)
{
    ls_df_log_clear(&g);
    int64_t t = 0;
    quiet(4, 20, &t);
    for (int i = 0; i < 600; i++, t += 100000) ls_df_log_feed(&g, &CFG, 4, 0, -70, 0, 0, t);
    LS_CHECK_MSG(ls_df_log_noise(&g, 4) > -72, "floor %.1f", ls_df_log_noise(&g, 4));
    LS_CHECK(!ls_df_log_feed(&g, &CFG, 4, 0, -70, 0, 0, t));
}
