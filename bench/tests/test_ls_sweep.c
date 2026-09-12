/* LS_TEST_SOURCES: ${FW}/components/lakeshark/dsp/ls_sweep_core.c */
/* LS_TEST_INCLUDE: ${FW}/components/lakeshark/dsp */
/* Sweep planning and stitching, on the host. */

#include "ls_test.h"
#include "ls_sweep_core.h"

#include <stdint.h>
#include <string.h>

#define RATE_2M4  2400000u   /* what the RTL runs at for a sweep */
#define FFT_N     512        /* SPEC_FFT_N */

static int8_t g_bins[LS_SWEEP_MAX_BINS];

/* A flat spectrum at `db` across every FFT bin. */
static void flat(float *out, int n, float db)
{
    for (int i = 0; i < n; i++) out[i] = db;
}

/* ------------------------------------------------------------- planning */

LS_CASE(plan_geometry_is_what_the_numbers_say)
{
    /* 88-108 MHz at 25 kHz bins: broadcast FM, the band used as the hardware
       acceptance test because its occupants are known and published. */
    ls_sweep_plan_t p;
    LS_CHECK(ls_sweep_plan(88000000ull, 108000000ull, 25000u, RATE_2M4, &p));

    LS_EQ_UINT(p.n_bins, 800u);            /* 20 MHz / 25 kHz */
    LS_EQ_UINT(p.half_span_hz, 900000u);   /* 37.5% of 2.4 MHz, either side */
    LS_EQ_UINT(p.strip_hz, 860000u);       /* minus the 40 kHz DC guard */
    LS_EQ_UINT(p.n_tunes, 24u);            /* ceil(20 MHz / 860 kHz) */
}

LS_CASE(plan_rejects_input_that_cannot_be_swept)
{
    ls_sweep_plan_t p;

    LS_CHECK(!ls_sweep_plan(108000000ull, 88000000ull, 25000u, RATE_2M4, &p));
    LS_CHECK(!ls_sweep_plan(88000000ull, 88000000ull, 25000u, RATE_2M4, &p));
    LS_CHECK(!ls_sweep_plan(88000000ull, 108000000ull, 0u, RATE_2M4, &p));
    LS_CHECK(!ls_sweep_plan(88000000ull, 108000000ull, 25000u, 0u, &p));

    LS_CHECK(!ls_sweep_plan(88000000ull, 108000000ull, 3000000u, RATE_2M4, &p));

    /* Too many bins to hold. 24 MHz .. 1.7 GHz at 1 kHz is 1.6 million. */
    LS_CHECK(!ls_sweep_plan(24000000ull, 1700000000ull, 1000u, RATE_2M4, &p));
}

LS_CASE(tune_centers_tile_the_span_without_overlap_or_hole)
{
    ls_sweep_plan_t p;
    LS_CHECK(ls_sweep_plan(88000000ull, 108000000ull, 25000u, RATE_2M4, &p));

    uint64_t prev_top = p.start_hz;
    for (uint32_t i = 0; i < p.n_tunes; i++) {
        uint64_t c = ls_sweep_tune_center(&p, i);

        uint64_t bot = c + LS_SWEEP_DC_GUARD_HZ;
        uint64_t top = bot + p.strip_hz;

        /* Each strip starts exactly where the last one ended. A gap is a blind
           slice of spectrum; an overlap wastes a retune, which is the whole
           cost of a sweep. */
        LS_EQ_UINT((unsigned)bot, (unsigned)prev_top);
        prev_top = top;
    }

    /* And the strips together reach past the requested stop. */
    LS_CHECK(prev_top >= p.stop_hz);
}

/* -------------------------------------------------------------- mapping */

LS_CASE(bin_index_puts_a_tone_where_it_actually_is)
{
    ls_sweep_plan_t p;
    LS_CHECK(ls_sweep_plan(88000000ull, 108000000ull, 25000u, RATE_2M4, &p));

    uint64_t c = ls_sweep_tune_center(&p, 0);

    int checked = 0;
    for (int k = 0; k < FFT_N; k++) {
        int idx = ls_sweep_bin_index(&p, c, k, FFT_N);
        if (idx < 0) continue;

        int64_t hz = ls_sweep_bin_hz(&p, c, k, FFT_N);
        uint64_t lo = p.start_hz + (uint64_t)p.bin_hz * (uint32_t)idx;
        uint64_t hi = lo + p.bin_hz;

        LS_CHECK((uint64_t)hz >= lo && (uint64_t)hz < hi);
        checked++;
    }
    LS_CHECK(checked > 100);
}

LS_CASE(dc_guard_drops_the_receivers_own_spike)
{
    /* The RTL puts a DC offset spike at the tune centre. Folding it in
       would deposit a fake carrier at every tune centre - an evenly spaced
       comb that reads as real signals and would have RF diff reporting the
       receiver to itself. */
    ls_sweep_plan_t p;
    LS_CHECK(ls_sweep_plan(88000000ull, 108000000ull, 25000u, RATE_2M4, &p));

    uint64_t c = ls_sweep_tune_center(&p, 0);

    /* Nothing at or below the guard survives, on either side of centre. */
    for (int k = 0; k < FFT_N; k++) {
        int64_t hz = ls_sweep_bin_hz(&p, c, k, FFT_N);
        if (hz - (int64_t)c < (int64_t)LS_SWEEP_DC_GUARD_HZ)
            LS_EQ_INT(ls_sweep_bin_index(&p, c, k, FFT_N), -1);
    }

    /* And the guard does not eat real spectrum: the first bin above it is
       kept. */
    int kept = 0;
    for (int k = 0; k < FFT_N; k++)
        if (ls_sweep_bin_index(&p, c, k, FFT_N) >= 0) kept++;
    LS_CHECK(kept > 100);
}

LS_CASE(passband_edges_are_left_to_the_neighbouring_tune)
{
    /* Bins beyond the trusted region read low because of the analogue
       roll-off, and everything below centre belongs to a lower tune. Both must
       be discarded here and supplied by the tune that has them well inside its
       own strip. */
    ls_sweep_plan_t p;
    LS_CHECK(ls_sweep_plan(88000000ull, 108000000ull, 25000u, RATE_2M4, &p));

    uint64_t c = ls_sweep_tune_center(&p, 1);   /* not the first strip */

    LS_EQ_INT(ls_sweep_bin_index(&p, c, 0, FFT_N), -1);          /* below */
    LS_EQ_INT(ls_sweep_bin_index(&p, c, FFT_N - 1, FFT_N), -1);  /* rolled off */

    /* Something comfortably inside the strip is kept. */
    LS_CHECK(ls_sweep_bin_index(&p, c, FFT_N / 2 + 40, FFT_N) >= 0);
}

/* -------------------------------------------------------------- folding */

LS_CASE(a_full_sweep_leaves_no_unmeasured_bin)
{
    /* The stitching invariant, and the reason this file exists.

       Fold every tune with a flat spectrum and every output bin must have
       been written. A hole means the tune step and the usable window disagree
       - a strip of spectrum nothing ever looks at. On hardware that is
       invisible: the plot is continuous, the noise floor is believable, and
       the signal living in the gap simply never appears. */
    ls_sweep_plan_t p;
    LS_CHECK(ls_sweep_plan(88000000ull, 108000000ull, 25000u, RATE_2M4, &p));

    float db[FFT_N];
    flat(db, FFT_N, -70.0f);

    ls_sweep_reset(&p, g_bins);
    for (uint32_t i = 0; i < p.n_tunes; i++)
        ls_sweep_fold(&p, ls_sweep_tune_center(&p, i), db, FFT_N, g_bins);

    LS_EQ_UINT(ls_sweep_gaps(&p, g_bins), 0u);
}

LS_CASE(no_holes_across_a_range_of_spans_and_bin_widths)
{
    /* The same invariant where it is most likely to break: spans that are not
       a whole number of tiles, and bin widths that do not divide the span. */
    static const uint64_t starts[] = { 88000000ull, 300000000ull, 433000000ull };
    static const uint64_t spans[]  = { 1000000ull, 4300000ull, 20000000ull,
                                       55500000ull };
    static const uint32_t bins[]   = { 5000u, 12500u, 25000u, 100000u };

    float db[FFT_N];
    flat(db, FFT_N, -80.0f);

    for (size_t a = 0; a < sizeof(starts) / sizeof(starts[0]); a++) {
        for (size_t b = 0; b < sizeof(spans) / sizeof(spans[0]); b++) {
            for (size_t c = 0; c < sizeof(bins) / sizeof(bins[0]); c++) {
                ls_sweep_plan_t p;
                if (!ls_sweep_plan(starts[a], starts[a] + spans[b],
                                   bins[c], RATE_2M4, &p)) continue;

                ls_sweep_reset(&p, g_bins);
                for (uint32_t i = 0; i < p.n_tunes; i++)
                    ls_sweep_fold(&p, ls_sweep_tune_center(&p, i), db,
                                  FFT_N, g_bins);

                LS_CHECK_MSG(ls_sweep_gaps(&p, g_bins) == 0u,
                            "holes at start=%llu span=%llu bin=%u: %u of %u",
                            (unsigned long long)starts[a],
                            (unsigned long long)spans[b], bins[c],
                            (unsigned)ls_sweep_gaps(&p, g_bins),
                            (unsigned)p.n_bins);
            }
        }
    }
}

LS_CASE(fold_keeps_the_peak_not_the_last_writer)
{
    ls_sweep_plan_t p;
    LS_CHECK(ls_sweep_plan(88000000ull, 90000000ull, 25000u, RATE_2M4, &p));

    float loud[FFT_N], quiet[FFT_N];
    flat(loud,  FFT_N, -20.0f);
    flat(quiet, FFT_N, -95.0f);

    uint64_t c = ls_sweep_tune_center(&p, 0);

    ls_sweep_reset(&p, g_bins);
    ls_sweep_fold(&p, c, loud,  FFT_N, g_bins);
    ls_sweep_fold(&p, c, quiet, FFT_N, g_bins);

    /* A narrow carrier heard once must survive a later quiet dwell. Averaging
       here is how a finder loses the thing it exists to find. */
    int idx = ls_sweep_bin_index(&p, c, FFT_N / 2 + 20, FFT_N);
    LS_CHECK(idx >= 0);
    LS_EQ_INT(g_bins[idx], -20);
}

LS_CASE(a_synthetic_carrier_is_reported_at_its_own_frequency)
{
    /* End to end on the arithmetic: put a peak in one FFT bin, fold, and find
       it in the output at the frequency that bin represents. */
    ls_sweep_plan_t p;
    LS_CHECK(ls_sweep_plan(88000000ull, 108000000ull, 25000u, RATE_2M4, &p));

    float db[FFT_N];
    flat(db, FFT_N, -100.0f);

    const int tune = 3;
    const int k    = FFT_N / 2 + 40;      /* clear of the DC guard */
    uint64_t c     = ls_sweep_tune_center(&p, tune);
    db[k] = -12.0f;

    ls_sweep_reset(&p, g_bins);
    for (uint32_t i = 0; i < p.n_tunes; i++) {
        const float *src = (i == (uint32_t)tune) ? db : NULL;
        float floor_only[FFT_N];
        if (!src) { flat(floor_only, FFT_N, -100.0f); src = floor_only; }
        ls_sweep_fold(&p, ls_sweep_tune_center(&p, i), src, FFT_N, g_bins);
    }

    int64_t want_hz = ls_sweep_bin_hz(&p, c, k, FFT_N);

    int peak = -1;
    for (uint32_t i = 0; i < p.n_bins; i++)
        if (peak < 0 || g_bins[i] > g_bins[peak]) peak = (int)i;

    LS_CHECK(peak >= 0);
    LS_EQ_INT(g_bins[peak], -12);

    int64_t got_hz = (int64_t)ls_sweep_out_hz(&p, (uint32_t)peak);
    int64_t err    = got_hz - want_hz;
    if (err < 0) err = -err;
    LS_CHECK_MSG(err <= (int64_t)p.bin_hz,
                "carrier reported %lld Hz away from truth",
                (long long)err);
}

LS_CASE(out_hz_and_bin_index_are_inverses)
{
    ls_sweep_plan_t p;
    LS_CHECK(ls_sweep_plan(433000000ull, 435000000ull, 12500u, RATE_2M4, &p));

    for (uint32_t i = 0; i < p.n_bins; i++) {
        uint64_t hz  = ls_sweep_out_hz(&p, i);
        uint64_t rel = hz - p.start_hz;
        LS_EQ_UINT((unsigned)(rel / p.bin_hz), (unsigned)i);
    }
}

LS_CASE(no_data_sentinel_survives_a_genuinely_dead_bin)
{
    /* -128 means "no tune reached this". A real reading must never quantise
       onto it, or a hole and a very quiet bin become indistinguishable. */
    ls_sweep_plan_t p;
    LS_CHECK(ls_sweep_plan(88000000ull, 90000000ull, 25000u, RATE_2M4, &p));

    float db[FFT_N];
    flat(db, FFT_N, -1000.0f);          /* absurdly quiet */

    ls_sweep_reset(&p, g_bins);
    ls_sweep_fold(&p, ls_sweep_tune_center(&p, 0), db, FFT_N, g_bins);

    int idx = ls_sweep_bin_index(&p, ls_sweep_tune_center(&p, 0),
                                 FFT_N / 2 + 20, FFT_N);
    LS_CHECK(idx >= 0);
    LS_EQ_INT(g_bins[idx], -127);
    LS_CHECK(g_bins[idx] != LS_SWEEP_NO_DATA);
}
