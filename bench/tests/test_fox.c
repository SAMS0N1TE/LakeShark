/* FOX HUNT: a bearing out of a turn, and a transmitter out of bearings. */
#include "ls_test.h"
#include "ls_df.h"
#include "ls_df_fit.h"
#include "ls_fox.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static ls_df_sweep_t s;

static double diff(double a, double b) { return fabs(fmod(a - b + 540, 360) - 180); }

static uint32_t seed = 12345;
static float noise(float db)
{
    /* Sum of four uniforms: near enough Gaussian, repeatable. */
    float v = 0;
    for (int i = 0; i < 4; i++) { seed = seed * 1664525u + 1013904223u; v += (seed >> 8) / 16777216.0f - 0.5f; }
    return v * db * 1.7f;
}

/* A hand-held whip with the body behind it: a broad lobe toward the
   transmitter and a shadow away from it, 14 dB deep. */
static float held(float off) { return -70.0f - 14.0f * (1.0f - cosf(off * 0.0174533f)) / 2.0f; }

/* Turn `turns` circles at a degree a sample, with fading. */
static void rotate(float truth, float start, float degrees, float fade_db, float (*pattern)(float))
{
    int64_t t = 0;
    for (float h = start; h < start + degrees; h += 1.0f, t += 20000) {
        const float heading = fmodf(h, 360.0f);
        ls_df_add(&s, heading, pattern((float)diff(heading, truth)) + noise(fade_db), t);
    }
}

LS_CASE(a_noisy_turn_still_points_at_the_transmitter)
{
    for (float truth = 5; truth < 360; truth += 41) {
        ls_df_clear(&s);
        rotate(truth, 30, 360, 2.0f, held);
        ls_df_fit_t f;
        LS_CHECK(ls_df_fit(&s, LS_DF_FIT, NULL, &f));
        LS_CHECK_MSG(f.valid && diff(f.bearing, truth) < 8, "truth %.0f fit %.1f", truth, f.bearing);
        LS_CHECK(f.sigma > 0 && f.sigma < 10);
        ls_df_estimate_t e;
        LS_CHECK(ls_df_estimate(&s, LS_DF_PEAK, &e));
        LS_CHECK_MSG(diff(e.bearing, truth) < 25, "truth %.0f peak %.1f", truth, e.bearing);
    }
}

LS_CASE(the_null_method_points_away_from_the_shadow)
{
    for (float truth = 20; truth < 360; truth += 67) {
        ls_df_clear(&s);
        rotate(truth, 0, 360, 1.0f, held);
        ls_df_fit_t f;
        LS_CHECK(ls_df_fit(&s, LS_DF_NULL, NULL, &f));
        LS_CHECK_MSG(diff(f.bearing, truth) < 10, "truth %.0f null %.1f", truth, f.bearing);
    }
}

LS_CASE(a_half_turn_is_less_sure_than_a_full_one)
{
    ls_df_fit_t full, half;
    ls_df_clear(&s); seed = 7; rotate(120, 0, 360, 2.0f, held);
    LS_CHECK(ls_df_fit(&s, LS_DF_FIT, NULL, &full));
    ls_df_clear(&s); seed = 7; rotate(120, 40, 160, 2.0f, held);
    const bool ok = ls_df_fit(&s, LS_DF_FIT, NULL, &half);
    LS_CHECK(!ok || half.gap > full.gap);
    LS_CHECK(!ok || half.sigma >= full.sigma);
}

/* -------------------------------------------------------------- the fix -- */

#define M_LAT 111132.0
static double mlon(double lat) { return M_LAT * cos(lat * M_PI / 180); }

static ls_df_bearing_t seen(double lat, double lon, double tlat, double tlon, float err, float spread)
{
    ls_df_bearing_t b;
    memset(&b, 0, sizeof(b));
    b.lat = lat; b.lon = lon; b.spread = spread;
    const double dx = (tlon - lon) * mlon(lat), dy = (tlat - lat) * M_LAT;
    b.bearing = (float)fmod(atan2(dx, dy) * 180 / M_PI + 360 + err, 360);
    return b;
}

static double metres(double lat0, double lon0, double lat1, double lon1)
{
    return hypot((lon1 - lon0) * mlon(lat0), (lat1 - lat0) * M_LAT);
}

LS_CASE(three_exact_bearings_cross_at_the_transmitter)
{
    const double tlat = 43.4510, tlon = -71.6430;
    ls_df_bearing_t b[3] = {
        seen(43.4400, -71.6600, tlat, tlon, 0, 6),
        seen(43.4600, -71.6650, tlat, tlon, 0, 6),
        seen(43.4380, -71.6300, tlat, tlon, 0, 6),
    };
    ls_fox_fix_t f;
    LS_CHECK(ls_fox_fix(b, 3, &f));
    LS_CHECK_MSG(metres(f.lat, f.lon, tlat, tlon) < 1.0, "miss %.2f m", metres(f.lat, f.lon, tlat, tlon));
    LS_EQ_INT(f.used, 3);
    LS_EQ_INT(f.behind, 0);
    LS_CHECK(f.rms_m < 0.5f);
    /* Exact lines: the ellipse is the a priori one, about range x 3 deg. */
    LS_CHECK(f.major_m > 10 && f.major_m < 400);
    LS_CHECK(f.minor_m > 0 && f.minor_m <= f.major_m);
    LS_NEAR(f.range_m, metres(43.4380, -71.6300, tlat, tlon), 2.0);
}

LS_CASE(two_bearings_give_an_ellipse_along_the_shallow_crossing)
{
    /* Two places close together on an east-west line, both looking north
       at a transmitter 2 km away: the lines cross at a shallow angle, so
       the fix is far less sure north-south than east-west. */
    const double tlat = 43.40 + 2000 / M_LAT, tlon = -71.50;
    ls_df_bearing_t b[2] = {
        seen(43.40, -71.50 - 300 / mlon(43.40), tlat, tlon, 0, 4),
        seen(43.40, -71.50 + 300 / mlon(43.40), tlat, tlon, 0, 4),
    };
    ls_fox_fix_t f;
    LS_CHECK(ls_fox_fix(b, 2, &f));
    LS_CHECK(metres(f.lat, f.lon, tlat, tlon) < 1.0);
    LS_CHECK_MSG(f.major_m > 3 * f.minor_m, "major %.0f minor %.0f", f.major_m, f.minor_m);
    LS_CHECK_MSG(f.major_deg < 10 || f.major_deg > 170, "major axis at %.1f, expected north-south", f.major_deg);
}

LS_CASE(noisy_bearings_land_inside_their_ellipse)
{
    const double tlat = 43.4510, tlon = -71.6430;
    int inside = 0, trials = 40;
    seed = 99;
    for (int t = 0; t < trials; t++) {
        ls_df_bearing_t b[5];
        static const double P[5][2] = { { 43.4400, -71.6600 }, { 43.4600, -71.6650 }, { 43.4380, -71.6300 },
                                        { 43.4650, -71.6350 }, { 43.4450, -71.6500 } };
        for (int i = 0; i < 5; i++) b[i] = seen(P[i][0], P[i][1], tlat, tlon, noise(3.0f), 6.0f);
        ls_fox_fix_t f;
        LS_CHECK(ls_fox_fix(b, 5, &f));
        const double miss = metres(f.lat, f.lon, tlat, tlon);
        LS_CHECK_MSG(miss < 400, "miss %.0f m", miss);
        if (miss < 3.0 * f.major_m) inside++;
    }
    /* A three sigma major axis holds nearly every noisy fix. */
    LS_CHECK_MSG(inside >= trials * 8 / 10, "%d of %d inside", inside, trials);
}

LS_CASE(a_line_pointing_away_is_left_out)
{
    const double tlat = 43.4510, tlon = -71.6430;
    ls_df_bearing_t b[4] = {
        seen(43.4400, -71.6600, tlat, tlon, 0, 6),
        seen(43.4600, -71.6650, tlat, tlon, 0, 6),
        seen(43.4380, -71.6300, tlat, tlon, 0, 6),
        seen(43.4700, -71.6200, tlat, tlon, 180, 6),   /* the back of a figure eight */
    };
    ls_fox_fix_t f;
    LS_CHECK(ls_fox_fix(b, 4, &f));
    LS_EQ_INT(f.behind, 1);
    LS_EQ_INT(f.used, 3);
    LS_CHECK(metres(f.lat, f.lon, tlat, tlon) < 2.0);
}

LS_CASE(parallel_or_too_few_bearings_have_no_fix)
{
    ls_fox_fix_t f;
    ls_df_bearing_t b[2] = { { .lat = 43.44, .lon = -71.66, .bearing = 10, .spread = 5 },
                             { .lat = 43.45, .lon = -71.65, .bearing = 18, .spread = 5 } };
    LS_CHECK(!ls_fox_fix(b, 2, &f));
    LS_CHECK(!f.valid);
    LS_CHECK(!ls_fox_fix(b, 1, &f));
    b[1].bearing = 190;                                   /* the same line, backwards */
    LS_CHECK(!ls_fox_fix(b, 2, &f));
}

LS_CASE(the_hunt_round_trips_as_csv)
{
    ls_df_bearing_t b = { .lat = 43.4400123, .lon = -71.6600456, .bearing = 213.4f, .spread = 7.5f, .level = -81.5f };
    snprintf(b.source, sizeof(b.source), "RTL, 146.565");
    char line[160];
    ls_fox_csv_line(&b, 1790000123u, line, sizeof(line));
    LS_EQ_STR(line, "1790000123,43.4400123,-71.6600456,213.4,7.5,-81.5,RTL  146.565\n");
    ls_df_bearing_t back; uint32_t utc = 0;
    LS_CHECK(ls_fox_csv_parse(line, &back, &utc));
    LS_EQ_UINT(utc, 1790000123u);
    LS_NEAR(back.lat, b.lat, 1e-7);
    LS_NEAR(back.lon, b.lon, 1e-7);
    LS_NEAR(back.bearing, 213.4, 1e-3);
    LS_NEAR(back.spread, 7.5, 1e-3);
    LS_NEAR(back.level, -81.5, 1e-3);
    LS_EQ_STR(back.source, "RTL  146.565");
    ls_fox_csv_header(line, sizeof(line));
    LS_CHECK(!ls_fox_csv_parse(line, &back, &utc));
    LS_CHECK(!ls_fox_csv_parse("# a comment", &back, &utc));
    LS_CHECK(!ls_fox_csv_parse("1,95,0,10,5,0,x", &back, &utc));
    LS_CHECK(!ls_fox_csv_parse("garbage", &back, &utc));

    static ls_fox_hunt_t h;
    ls_fox_hunt_clear(&h);
    for (int i = 0; i < LS_FOX_MAX + 3; i++) { b.bearing = (float)i; ls_fox_hunt_add(&h, &b, (uint32_t)i); }
    LS_EQ_INT(h.n, LS_FOX_MAX);
    LS_NEAR(h.b[0].bearing, 3, 1e-6);
    LS_EQ_UINT(h.utc[LS_FOX_MAX - 1], (uint32_t)(LS_FOX_MAX + 2));
    LS_CHECK(ls_fox_hunt_drop_last(&h));
    LS_EQ_INT(h.n, LS_FOX_MAX - 1);
}

LS_CASE(a_projected_line_ends_where_it_should)
{
    double lat, lon;
    ls_fox_project(43.0, -71.0, 90, 1000, &lat, &lon);
    LS_NEAR(lat, 43.0, 1e-9);
    LS_NEAR(metres(43.0, -71.0, lat, lon), 1000, 0.5);
    ls_fox_project(43.0, -71.0, 180, 500, &lat, &lon);
    LS_NEAR((43.0 - lat) * M_LAT, 500, 0.01);
}
