#include "ls_test.h"
#include "ls_df.h"
#include "ls_compass_live.h"
#include <math.h>
#include <string.h>

static ls_df_sweep_t s;

static double diff(double a, double b) { return fabs(fmod(a - b + 540, 360) - 180); }

/* A slow turn: `turn` degrees from `start`, one sample per degree, the level
   a function of how far the antenna faces from the transmitter. */
static void turn(float start, float degrees, float (*pattern)(float off), float truth)
{
    for (float h = start; h < start + degrees; h += 1.0f)
        ls_df_add(&s, h, pattern((float)diff(h, truth)), (int64_t)(h * 1000));
}

static float lobe(float off) { return -60.0f - 12.0f * (1.0f - cosf(off * 0.0174533f)) / 2.0f; }
static float body(float off) { float f = 180.0f - off; return -70.0f - 9.0f * expf(-(f * f) / (2 * 25.0f * 25.0f)); }
static float flat(float off) { (void)off; return -80.0f; }

LS_CASE(a_full_turn_finds_the_lobe)
{
    for (float truth = 0; truth < 360; truth += 37) {
        ls_df_clear(&s);
        turn(0, 360, lobe, truth);
        ls_df_estimate_t e;
        LS_CHECK(ls_df_estimate(&s, LS_DF_PEAK, &e));
        LS_CHECK_MSG(diff(e.bearing, truth) < 4, "truth %.0f got %.1f", truth, e.bearing);
        LS_CHECK(e.spread > 10 && e.spread < 180);
        LS_EQ_INT(e.coverage, 360);
        LS_NEAR(e.contrast, 12, 0.6);
    }
}

LS_CASE(the_body_null_points_away_from_the_shadow)
{
    for (float truth = 10; truth < 360; truth += 53) {
        ls_df_clear(&s);
        turn(0, 360, body, truth);
        ls_df_estimate_t e;
        LS_CHECK(ls_df_estimate(&s, LS_DF_NULL, &e));
        LS_CHECK_MSG(diff(e.bearing, truth) < 5, "truth %.0f got %.1f", truth, e.bearing);
    }
}

LS_CASE(no_answer_without_enough_circle_or_any_contrast)
{
    ls_df_estimate_t e;
    ls_df_clear(&s);
    turn(0, 120, lobe, 60);                   /* a third of a turn */
    LS_CHECK(!ls_df_estimate(&s, LS_DF_PEAK, &e));
    LS_EQ_INT(e.coverage, 125);            /* bins are centred on 5 degree marks */
    ls_df_clear(&s);
    turn(0, 360, flat, 0);                    /* nothing to point at */
    LS_CHECK(!ls_df_estimate(&s, LS_DF_PEAK, &e));
    ls_df_clear(&s);
    turn(0, 200, body, 100);                  /* a null needs most of the circle */
    LS_CHECK(!ls_df_estimate(&s, LS_DF_NULL, &e));
}

LS_CASE(a_bin_keeps_the_strongest_level_and_old_bins_are_forgotten)
{
    ls_df_clear(&s);
    ls_df_add(&s, 90, -80, 0);
    ls_df_add(&s, 91, -60, 1000);
    ls_df_add(&s, 89, -75, 2000);
    LS_NEAR(s.level[18], -60, 1e-6);
    LS_EQ_INT(s.count[18], 3);
    ls_df_add(&s, 180, -70, 50000000);
    ls_df_age(&s, 60000000, 30000000);        /* 30 s window */
    LS_EQ_INT(s.count[18], 0);
    LS_EQ_INT(s.count[36], 1);
    LS_NEAR(s.top, -70, 1e-6);
}

LS_CASE(a_held_peak_waits_then_falls_to_what_is_heard_now)
{
    const ls_df_decay_t d = { .hold_s = 5, .decay_db_s = 2 };
    ls_df_clear(&s);
    ls_df_add(&s, 90, -50, 0);                /* a loud moment */
    ls_df_add(&s, 90, -80, 100000);           /* then the usual */
    ls_df_add(&s, 200, -75, 100000);
    ls_df_decay(&s, 0, &d);
    ls_df_decay(&s, 4000000, &d);
    LS_NEAR(s.level[18], -50, 1e-4);          /* still inside the hold */
    ls_df_decay(&s, 8000000, &d);             /* 3 s past it: 6 dB down */
    LS_NEAR(s.level[18], -56, 0.05);
    ls_df_decay(&s, 60000000, &d);            /* long after: back to the latest */
    LS_NEAR(s.level[18], -80, 1e-4);
    LS_NEAR(s.top, -75, 1e-4);                /* so the other direction is on top now */
    ls_df_estimate_t e;
    for (float h = 0; h < 360; h += 5) if (!s.count[(int)(h / 5)]) ls_df_add(&s, h, -90, 60000000);
    LS_CHECK(ls_df_estimate(&s, LS_DF_PEAK, &e));
    LS_CHECK_MSG(diff(e.bearing, 200) < 6, "got %.1f", e.bearing);
}

LS_CASE(no_decay_holds_forever_and_a_fresh_peak_restarts_the_hold)
{
    ls_df_clear(&s);
    ls_df_add(&s, 10, -60, 0);
    const ls_df_decay_t never = { .hold_s = 0, .decay_db_s = 0 };
    ls_df_decay(&s, 0, &never);
    ls_df_decay(&s, 600000000, &never);
    LS_NEAR(s.level[2], -60, 1e-6);
    const ls_df_decay_t d = { .hold_s = 2, .decay_db_s = 10 };
    ls_df_clear(&s);
    ls_df_add(&s, 10, -60, 0);
    ls_df_decay(&s, 0, &d);
    ls_df_add(&s, 10, -58, 1900000);          /* heard again, louder */
    ls_df_decay(&s, 3000000, &d);             /* 1.1 s after the new peak */
    LS_NEAR(s.level[2], -58, 1e-4);
}

static float two_lobes(float h)
{
    const float a = (float)diff(h, 60), b = (float)diff(h, 250);
    return -95.0f + 20.0f * expf(-(a * a) / (2 * 15.0f * 15.0f)) + 11.0f * expf(-(b * b) / (2 * 15.0f * 15.0f));
}

LS_CASE(peaks_finds_the_main_and_a_lesser_lobe)
{
    ls_df_clear(&s);
    for (float h = 0; h < 360; h += 1) ls_df_add(&s, h, two_lobes(h), (int64_t)(h * 1000));
    ls_df_peak_t p[4];
    const int n = ls_df_peaks(&s, 6.0f, p, 4);
    LS_EQ_INT(n, 2);
    LS_CHECK_MSG(diff(p[0].bearing, 60) <= 5, "main at %.0f", p[0].bearing);
    LS_CHECK_MSG(diff(p[1].bearing, 250) <= 5, "lesser at %.0f", p[1].bearing);
    LS_CHECK(p[0].level > p[1].level);
    LS_CHECK(p[1].prominence > 8 && p[1].prominence < 12);
    /* Asked for taller, the lesser lobe is left out. */
    LS_EQ_INT(ls_df_peaks(&s, 15.0f, p, 4), 1);
    /* A flat circle has one top and no lobes worth the name. */
    ls_df_clear(&s);
    for (float h = 0; h < 360; h += 5) ls_df_add(&s, h, -80, 0);
    LS_EQ_INT(ls_df_peaks(&s, 3.0f, p, 4), 0);
}

/* Observers around a transmitter, bearings computed exactly. */
static ls_df_bearing_t seen_from(double lat, double lon, double tlat, double tlon, float spread)
{
    ls_df_bearing_t b = { .lat = lat, .lon = lon, .spread = spread };
    double brg, m;
    ls_compass_nav(lat, lon, tlat, tlon, &brg, &m);
    b.bearing = (float)brg;
    return b;
}

LS_CASE(three_bearings_cross_at_the_transmitter)
{
    const double tlat = 43.4500, tlon = -71.6500;
    ls_df_bearing_t b[3] = {
        seen_from(43.4400, -71.6600, tlat, tlon, 5),
        seen_from(43.4420, -71.6380, tlat, tlon, 5),
        seen_from(43.4580, -71.6620, tlat, tlon, 5),
    };
    ls_df_fix_t f;
    LS_CHECK(ls_df_triangulate(b, 3, &f));
    double brg, m;
    ls_compass_nav(f.lat, f.lon, tlat, tlon, &brg, &m);
    LS_CHECK_MSG(m < 15, "fix is %.1f m off", m);
    LS_CHECK(f.radius_m > 1 && f.radius_m < 500);
    LS_EQ_INT(f.used, 3);
}

LS_CASE(an_error_in_one_bearing_moves_the_fix_but_not_far)
{
    const double tlat = 43.4500, tlon = -71.6500;
    ls_df_bearing_t b[3] = {
        seen_from(43.4400, -71.6600, tlat, tlon, 8),
        seen_from(43.4420, -71.6380, tlat, tlon, 8),
        seen_from(43.4580, -71.6620, tlat, tlon, 8),
    };
    b[1].bearing += 6;
    ls_df_fix_t f;
    LS_CHECK(ls_df_triangulate(b, 3, &f));
    double brg, m;
    ls_compass_nav(f.lat, f.lon, tlat, tlon, &brg, &m);
    LS_CHECK_MSG(m < 250, "fix is %.1f m off", m);
}

LS_CASE(parallel_or_diverging_bearings_are_not_a_fix)
{
    ls_df_bearing_t b[2] = { { .lat = 43.44, .lon = -71.66, .bearing = 10, .spread = 5 },
                             { .lat = 43.44, .lon = -71.65, .bearing = 15, .spread = 5 } };
    ls_df_fix_t f;
    LS_CHECK(!ls_df_triangulate(b, 2, &f));             /* 5 degrees apart */
    b[0].bearing = 300; b[1].bearing = 60;              /* crossing behind both */
    b[0].lat = 43.44; b[1].lat = 43.44;
    LS_CHECK(!ls_df_triangulate(b, 2, &f));
    LS_CHECK(!ls_df_triangulate(b, 1, &f));
}

static uint8_t iq[2 * 2048];

static void tone(double freq_hz, double amp, uint32_t rate)
{
    for (int n = 0; n < 2048; n++) {
        const double ph = 2 * M_PI * freq_hz * n / rate;
        iq[2 * n] = (uint8_t)lround(127.5 + amp * 127.5 * cos(ph));
        iq[2 * n + 1] = (uint8_t)lround(127.5 + amp * 127.5 * sin(ph));
    }
}

LS_CASE(band_power_sees_a_tone_in_the_band_and_not_outside_it)
{
    const uint32_t rate = 1024000;
    tone(250000, 0.5, rate);
    const float in = ls_df_iq_band_db(iq, 2048, rate, 250000, 12500);
    const float out = ls_df_iq_band_db(iq, 2048, rate, 100000, 12500);
    LS_CHECK_MSG(in > -8 && in < -4, "in band %.1f dBFS", in);       /* half scale: -6 */
    LS_CHECK_MSG(out < in - 30, "out of band %.1f vs %.1f", out, in);
    tone(250000, 0.05, rate);
    const float weak = ls_df_iq_band_db(iq, 2048, rate, 250000, 12500);
    LS_CHECK_MSG(fabsf((in - weak) - 20) < 2.5f, "10x amplitude is 20 dB, got %.1f", in - weak);
    tone(-180000, 0.5, rate);
    LS_CHECK(ls_df_iq_band_db(iq, 2048, rate, -180000, 12500) > -8);
    LS_CHECK(isnan(ls_df_iq_band_db(iq, 10, rate, 0, 1000)));
}
