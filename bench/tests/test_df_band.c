#include "ls_test.h"
#include "ls_df_band.h"
#include <math.h>
#include <string.h>

static ls_dfb_t b;
static ls_rng_t rng;

static void quiet(float *x)
{
    for (int i = 0; i < LS_DFB_BINS; i++) x[i] = -105.0f + ls_rng_noise(&rng) * 5.0f;
}

LS_CASE(band_bins_span_the_band)
{
    ls_dfb_reset(&b, 902000000u, 928000000u);
    LS_EQ_UINT(ls_dfb_bin_hz(&b, 0), 902000000u);
    LS_EQ_UINT(ls_dfb_bin_hz(&b, LS_DFB_BINS - 1), 928000000u);
}

/* A transmitter that never stops is in the background from the first
   pass, and is still found: it stands over the band's floor. */
LS_CASE(a_steady_carrier_is_found_though_it_is_background)
{
    ls_rng_seed(&rng, 1);
    ls_dfb_reset(&b, 902000000u, 928000000u);
    float x[LS_DFB_BINS];
    ls_dfb_read_t r[8];
    int reads = 0;
    for (int p = 0; p < 40; p++) {
        quiet(x); x[20] = -80.0f + ls_rng_noise(&rng);
        reads += ls_dfb_pass(&b, x, p * 500000LL, r, 8);
    }
    LS_CHECK(b.track[0].live);
    LS_EQ_INT(b.track[0].bin, 20);
    LS_EQ_UINT(b.track[0].hz, ls_dfb_bin_hz(&b, 20));
    /* The first pass it stands out on only marks it; the second opens it. */
    LS_EQ_INT(reads, 40 - LS_DFB_WARM - 1);
    int live = 0;
    for (int t = 0; t < LS_DFB_TRACKS; t++) live += b.track[t].live;
    LS_EQ_INT(live, 1);
}

/* A burst standing a little over a noisy background is found; its gaps
   file nothing, and the noise alone opens no track. */
LS_CASE(bursts_are_found_and_noise_is_not)
{
    ls_rng_seed(&rng, 2);
    ls_dfb_reset(&b, 902000000u, 928000000u);
    float x[LS_DFB_BINS];
    ls_dfb_read_t r[8];
    int reads = 0, on = 0;
    for (int p = 0; p < 300; p++) {
        quiet(x);
        const bool burst = p > 20 && ls_rng_u32(&rng) % 3 == 0;
        if (burst) { x[45] = -96.0f; on++; }
        const int n = ls_dfb_pass(&b, x, p * 500000LL, r, 8);
        for (int i = 0; i < n; i++) { LS_EQ_UINT(r[i].hz, ls_dfb_bin_hz(&b, 45)); reads++; }
    }
    LS_CHECK_MSG(reads >= on * 9 / 10 && reads <= on, "reads %d of %d bursts", reads, on);
    LS_EQ_INT(b.opened, 1);
}

/* An emitter shadowed 8 dB by the body in some directions stays one track
   and keeps being read, and one drifting a step keeps its track. */
LS_CASE(a_shadowed_emitter_keeps_its_track)
{
    ls_rng_seed(&rng, 3);
    ls_dfb_reset(&b, 400000000u, 480000000u);
    float x[LS_DFB_BINS];
    ls_dfb_read_t r[8];
    for (int p = 0; p < 10; p++) { quiet(x); ls_dfb_pass(&b, x, p * 500000LL, r, 8); }
    int reads = 0;
    for (int p = 10; p < 130; p++) {
        quiet(x);
        const float shade = 4.0f * (1.0f + cosf(p * 0.1f));      /* 0..8 dB */
        x[30 + (p / 40) % 2] = -88.0f - shade;
        reads += ls_dfb_pass(&b, x, p * 500000LL, r, 8);
    }
    LS_EQ_INT(b.opened, 1);
    LS_CHECK_MSG(reads >= 115, "reads %d of 120", reads);
}

/* Two emitters, two tracks; one gone long enough frees its track. */
LS_CASE(tracks_are_freed_when_forgotten)
{
    ls_rng_seed(&rng, 4);
    ls_dfb_reset(&b, 902000000u, 928000000u);
    float x[LS_DFB_BINS];
    ls_dfb_read_t r[8];
    int64_t us = 0;
    for (int p = 0; p < 20; p++, us += 500000) { quiet(x); x[10] = -85; x[50] = -85; ls_dfb_pass(&b, x, us, r, 8); }
    LS_EQ_INT(b.opened, 2);
    for (int p = 0; p < 300; p++, us += 500000) { quiet(x); x[10] = -85; ls_dfb_pass(&b, x, us, r, 8); }
    int live = 0;
    for (int t = 0; t < LS_DFB_TRACKS; t++) live += b.track[t].live;
    LS_EQ_INT(live, 1);
}

/* A transmitter a metre away lifts the floor for megahertz round it, and
   the skirt wobbles: that is one emitter, at the loud step. */
LS_CASE(a_strong_neighbours_skirt_is_one_emitter)
{
    ls_rng_seed(&rng, 5);
    ls_dfb_reset(&b, 902000000u, 928000000u);
    float x[LS_DFB_BINS];
    ls_dfb_read_t r[8];
    int64_t us = 0;
    for (int p = 0; p < 10; p++, us += 150000) { quiet(x); ls_dfb_pass(&b, x, us, r, 8); }
    for (int p = 0; p < 200; p++, us += 150000) {
        quiet(x);
        for (int i = 18; i <= 44; i++) x[i] = -74.0f + ls_rng_noise(&rng) * 6.0f;
        x[30] = -60.0f;
        if (p % 7 == 0) x[25] = -95.0f;              /* a dip in the skirt now and then */
        ls_dfb_pass(&b, x, us, r, 8);
    }
    int live = 0, at = -1;
    for (int t = 0; t < LS_DFB_TRACKS; t++) if (b.track[t].live) { live++; at = b.track[t].bin; }
    LS_EQ_INT(live, 1);
    LS_EQ_INT(at, 30);
    LS_CHECK_MSG(b.opened <= 3, "opened %lu", (unsigned long)b.opened);
}
