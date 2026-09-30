#include "ls_test.h"
#include "ls_df.h"
#include "ls_df_fit.h"
#include <math.h>
#include <string.h>

static ls_df_sweep_t s;
static ls_rng_t rng;

static float diff(float a, float b) { return fabsf(fmodf(a - b + 540.0f, 360.0f) - 180.0f); }

/* Body shadow: level falls as the transmitter goes behind, 10 dB at the back. */
static float body(float rel) { return -70.0f + 5.0f * cosf(rel * 0.0174533f); }
/* A board whose peak sits 25 degrees right of where it faces, with a back lobe. */
static float lopsided(float rel)
{
    const float r = (rel - 25.0f) * 0.0174533f;
    return -70.0f + 6.0f * cosf(r) + 2.5f * cosf(2 * r + 0.6f);
}

/* Fading: a Rayleigh envelope in dB has a long tail downward; a sum of
   uniforms skewed down is close enough, plus an occasional spike. */
static float fade(void)
{
    float f = ls_rng_noise(&rng) * 8.0f - fabsf(ls_rng_noise(&rng)) * 6.0f;
    if ((ls_rng_u32(&rng) % 60) == 0) f += 12.0f;
    return f;
}

/* `laps` turns at 30 deg/s, three reads a second (a LoRa beacon's rate). */
static void turn_it(float truth, float (*pattern)(float), int laps, float start)
{
    ls_df_clear(&s);
    int64_t us = 0;
    for (float h = start; h < start + 360.0f * laps; h += 10.0f, us += 333000)
        ls_df_add(&s, h, pattern(h - truth) + fade(), us);
}

/* Over many turns, FIT's error is well under the held peak's. */
LS_CASE(fit_beats_the_held_peak_in_fading)
{
    ls_rng_seed(&rng, 3);
    float err_peak = 0, err_fit = 0;
    int n_peak = 0, n_fit = 0, inside = 0;
    for (int trial = 0; trial < 60; trial++) {
        const float truth = (float)((trial * 47) % 360);
        turn_it(truth, body, 2, (float)(trial * 13 % 360));
        ls_df_estimate_t e;
        if (ls_df_estimate(&s, LS_DF_PEAK, &e)) { err_peak += diff(e.bearing, truth) * diff(e.bearing, truth); n_peak++; }
        ls_df_fit_t f;
        if (ls_df_fit(&s, LS_DF_PEAK, NULL, &f)) {
            const float d = diff(f.bearing, truth);
            err_fit += d * d; n_fit++;
            if (d <= 2.0f * f.sigma) inside++;
        }
    }
    const float rms_peak = sqrtf(err_peak / (n_peak ? n_peak : 1)), rms_fit = sqrtf(err_fit / (n_fit ? n_fit : 1));
    ls_note("held peak rms %.1f deg (%d), fit rms %.1f deg (%d), %d within 2 sigma", rms_peak, n_peak, rms_fit, n_fit, inside);
    LS_CHECK(n_fit >= 55);
    LS_CHECK_MSG(rms_fit < 0.6f * rms_peak, "fit %.1f vs peak %.1f", rms_fit, rms_peak);
    LS_CHECK_MSG(rms_fit < 15.0f, "fit rms %.1f", rms_fit);
    LS_CHECK_MSG(inside >= n_fit * 85 / 100, "sigma too small: %d of %d inside 2 sigma", inside, n_fit);
}

/* NULL aims opposite the fitted low point. */
LS_CASE(fit_null_points_away_from_the_shadow)
{
    ls_rng_seed(&rng, 5);
    turn_it(200, body, 2, 0);
    ls_df_fit_t f;
    LS_CHECK(ls_df_fit(&s, LS_DF_NULL, NULL, &f));
    LS_CHECK_MSG(diff(f.bearing, 200) < 15, "got %.1f", f.bearing);
}

/* Half a turn is not enough for NULL, and a flat circle is no bearing. */
LS_CASE(fit_refuses_what_it_cannot_know)
{
    ls_rng_seed(&rng, 9);
    ls_df_clear(&s);
    for (float h = 0; h < 150; h += 5) ls_df_add(&s, h, body(h - 40), 0);
    ls_df_fit_t f;
    LS_CHECK(!ls_df_fit(&s, LS_DF_PEAK, NULL, &f));
    ls_df_clear(&s);
    for (float h = 0; h < 720; h += 5) ls_df_add(&s, h, -80.0f + ls_rng_noise(&rng) * 1.0f, 0);
    LS_CHECK(!ls_df_fit(&s, LS_DF_PEAK, NULL, &f));
}

/* A figure eight says front or back, not which. */
LS_CASE(a_figure_eight_is_ambiguous)
{
    ls_df_clear(&s);
    for (float h = 0; h < 360; h += 5) ls_df_add(&s, h, -70.0f + 6.0f * cosf(2 * (h - 30) * 0.0174533f), 0);
    ls_df_fit_t f;
    LS_CHECK(ls_df_fit(&s, LS_DF_PEAK, NULL, &f));
    LS_CHECK(f.ambiguous);
    LS_CHECK_MSG(fminf(diff(f.bearing, 30), diff(f.bearing, 210)) < 3, "got %.1f", f.bearing);
    LS_CHECK_MSG(diff(f.alt, f.bearing) > 179, "alt %.1f", f.alt);
}

/* A lopsided board: the harmonic fit is 25 degrees off every time. Learnt
   from three circles round a beacon, the pattern takes that out at any
   other bearing. */
LS_CASE(a_learnt_pattern_takes_out_the_boards_own_bias)
{
    ls_rng_seed(&rng, 21);
    ls_df_pattern_t p;
    ls_df_pattern_clear(&p);
    for (int c = 0; c < 3; c++) {
        turn_it(100, lopsided, 1, c * 40.0f);
        LS_CHECK(ls_df_pattern_learn(&p, &s, 100));
    }
    LS_EQ_INT(p.circles, 3);
    int8_t packed[LS_DF_PAT_BINS];
    ls_df_pattern_pack(&p, packed);
    ls_df_pattern_t q;
    ls_df_pattern_unpack(&q, packed, p.circles);
    float err_fit = 0, err_pat = 0;
    int n = 0;
    for (int trial = 0; trial < 30; trial++) {
        const float truth = (float)((trial * 71 + 15) % 360);
        turn_it(truth, lopsided, 2, (float)(trial * 29 % 360));
        ls_df_fit_t a, b;
        if (!ls_df_fit(&s, LS_DF_PEAK, NULL, &a) || !ls_df_fit(&s, LS_DF_PEAK, &q, &b)) continue;
        err_fit += diff(a.bearing, truth) * diff(a.bearing, truth);
        err_pat += diff(b.bearing, truth) * diff(b.bearing, truth);
        n++;
        LS_CHECK_MSG(b.match > 0.5f, "match %.2f", b.match);
    }
    const float rf = sqrtf(err_fit / n), rp = sqrtf(err_pat / n);
    ls_note("lopsided board: harmonic rms %.1f deg, learnt pattern rms %.1f deg (%d)", rf, rp, n);
    LS_CHECK(n >= 25);
    LS_CHECK_MSG(rf > 10.0f, "the bias should show without the pattern: %.1f", rf);
    LS_CHECK_MSG(rp < 0.6f * rf, "with the pattern %.1f vs %.1f", rp, rf);
}

LS_CASE(typical_is_the_median_of_the_latest_reads)
{
    ls_df_clear(&s);
    const float reads[] = { -60, -90, -61, -59, -62, -30, -58 };
    for (int i = 0; i < 7; i++) ls_df_add(&s, 0, reads[i], i);
    /* The last five: -61 -59 -62 -30 -58, median -59. */
    LS_NEAR(ls_df_typical(&s, 0), -59, 1e-4);
    LS_CHECK(isnan(ls_df_typical(&s, 10)));
}
