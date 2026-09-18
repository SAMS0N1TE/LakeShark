/* LS_TEST_SOURCES: ${FW}/components/lakeshark/apps/p25/p25_geo.c */
/* Which control channel the receiver should be on, given where it is.

   Every case here is the drive this was built for: Franklin NH down to the
   Cape. The decision is pure - position and a clock in, an index out - so
   the whole of it is provable here, and what is left for the board is only
   whether the tuner does as it is told. */

#include "ls_test.h"
#include "p25_geo.h"

#include <math.h>
#include <string.h>

/* Real places, so a distance that comes out wrong is obviously wrong rather
   than plausibly wrong. */
#define FRANKLIN_NH_LAT    43.4406
#define FRANKLIN_NH_LON   -71.6498
#define CONCORD_NH_LAT     43.2081
#define CONCORD_NH_LON    -71.5376
#define BOSTON_MA_LAT      42.3601
#define BOSTON_MA_LON     -71.0589
#define W_DENNIS_MA_LAT    41.6620
#define W_DENNIS_MA_LON   -70.1700

static void site(p25_profile_t *p, unsigned i, uint64_t hz,
                 double lat, double lon, uint32_t radius_m)
{
    p->control_channels[i] = hz;
    p->control_lat_e7[i] = (int32_t)(lat * 1e7);
    p->control_lon_e7[i] = (int32_t)(lon * 1e7);
    p->control_radius_m[i] = radius_m;
    p->control_has_geo[i] = true;
    if (i + 1 > p->control_count) p->control_count = (uint8_t)(i + 1);
}

/* Franklin, Concord, Boston, West Dennis - in the order they are driven. */
static void route(p25_profile_t *p)
{
    memset(p, 0, sizeof(*p));
    p->format_version = 2;
    site(p, 0, 851012500u, FRANKLIN_NH_LAT, FRANKLIN_NH_LON, 30000);
    site(p, 1, 851287500u, CONCORD_NH_LAT,  CONCORD_NH_LON,  30000);
    site(p, 2, 852237500u, BOSTON_MA_LAT,   BOSTON_MA_LON,   40000);
    site(p, 3, 853712500u, W_DENNIS_MA_LAT, W_DENNIS_MA_LON, 35000);
}

LS_CASE(the_distance_matches_the_map)
{
    /* Franklin to Concord is about 26 km; Franklin to West Dennis about
       250 km. Loose bounds on purpose - this is catching a radians/degrees
       slip or a swapped argument, not surveying. */
    const double near = p25_geo_distance_m(FRANKLIN_NH_LAT, FRANKLIN_NH_LON,
                                           CONCORD_NH_LAT, CONCORD_NH_LON);
    LS_CHECK_MSG(near > 24000 && near < 29000,
                 "Franklin to Concord came out %.0f m", near);

    const double far = p25_geo_distance_m(FRANKLIN_NH_LAT, FRANKLIN_NH_LON,
                                          W_DENNIS_MA_LAT, W_DENNIS_MA_LON);
    LS_CHECK_MSG(far > 230000 && far < 270000,
                 "Franklin to West Dennis came out %.0f m", far);

    /* Symmetric, and zero to itself. */
    LS_CHECK(fabs(p25_geo_distance_m(BOSTON_MA_LAT, BOSTON_MA_LON,
                                     W_DENNIS_MA_LAT, W_DENNIS_MA_LON) -
                  p25_geo_distance_m(W_DENNIS_MA_LAT, W_DENNIS_MA_LON,
                                     BOSTON_MA_LAT, BOSTON_MA_LON)) < 1.0);
    LS_CHECK(p25_geo_distance_m(BOSTON_MA_LAT, BOSTON_MA_LON,
                                BOSTON_MA_LAT, BOSTON_MA_LON) < 1.0);
    /* A longitude sign error puts Boston in Asia. */
    LS_CHECK(p25_geo_distance_m(BOSTON_MA_LAT, BOSTON_MA_LON,
                                BOSTON_MA_LAT, -BOSTON_MA_LON) > 5000000);
}

LS_CASE(the_drive_selects_each_site_as_it_is_reached)
{
    p25_profile_t p; route(&p);
    p25_geo_t g; memset(&g, 0, sizeof(g));

    int64_t now = 1000000;
    LS_EQ_INT(0, p25_geo_select(&g, &p, true, FRANKLIN_NH_LAT, FRANKLIN_NH_LON,
                                now, now));
    now += 3600000000LL;
    LS_EQ_INT(1, p25_geo_select(&g, &p, true, CONCORD_NH_LAT, CONCORD_NH_LON,
                                now, now));
    now += 3600000000LL;
    LS_EQ_INT(2, p25_geo_select(&g, &p, true, BOSTON_MA_LAT, BOSTON_MA_LON,
                                now, now));
    now += 3600000000LL;
    LS_EQ_INT(3, p25_geo_select(&g, &p, true, W_DENNIS_MA_LAT, W_DENNIS_MA_LON,
                                now, now));
}

LS_CASE(standing_still_asks_for_no_retune)
{
    /* -1 means "change nothing", and it is the answer most of the time. A
       selector that returned its choice every tick would retune the radio
       once per GPS fix for the whole drive. */
    p25_profile_t p; route(&p);
    p25_geo_t g; memset(&g, 0, sizeof(g));

    int64_t now = 1000000;
    LS_EQ_INT(0, p25_geo_select(&g, &p, true, FRANKLIN_NH_LAT, FRANKLIN_NH_LON,
                                now, now));
    for (int i = 0; i < 20; i++) {
        now += 1000000;
        LS_EQ_INT(-1, p25_geo_select(&g, &p, true, FRANKLIN_NH_LAT,
                                     FRANKLIN_NH_LON, now, now));
    }
}

LS_CASE(a_boundary_does_not_make_the_receiver_oscillate)
{
    /* Two sites whose circles overlap, and a position in the overlap that
       is nearer the second. Without hysteresis, drifting back and forth
       across the midpoint switches sites every fix, and every switch costs
       a control-channel re-acquisition. */
    p25_profile_t p;
    memset(&p, 0, sizeof(p));
    p.format_version = 2;
    site(&p, 0, 851012500u, 43.0000, -71.5000, 30000);
    site(&p, 1, 851287500u, 43.2000, -71.5000, 30000);

    p25_geo_t g; memset(&g, 0, sizeof(g));
    int64_t now = 1000000;

    /* Start firmly on the first. */
    LS_EQ_INT(0, p25_geo_select(&g, &p, true, 43.0000, -71.5000, now, now));

    /* Creep past the midpoint, where the second is nearer but the first is
       still well inside its own radius plus the margin. */
    for (int i = 0; i < 10; i++) {
        now += 1000000;
        const double lat = 43.1010 + i * 0.0002;
        LS_CHECK_MSG(p25_geo_select(&g, &p, true, lat, -71.5000, now, now) == -1,
                     "switched at lat %.4f - that is the oscillation this "
                     "case exists to prevent", lat);
    }

    /* Far enough that the first is genuinely out of reach: now it moves.
       43.20 is NOT far enough and the first version of this case asserted it
       was - at 43.20 the first site is still only 22 km away, comfortably
       inside its own 30 km radius, so holding there is correct. 43.35 is
       about 39 km from the first site and 17 km from the second. */
    now += 1000000;
    LS_EQ_INT(1, p25_geo_select(&g, &p, true, 43.3500, -71.5000, now, now));
}

LS_CASE(a_gap_between_sites_keeps_the_last_one_rather_than_hunting)
{
    /* Route coverage is not continuous. Between islands the honest answer
       is "stay where you are" - a site just out of reach still hears better
       than one two counties back, and retuning into nothing costs the
       acquisition for no gain. */
    p25_profile_t p; route(&p);
    p25_geo_t g; memset(&g, 0, sizeof(g));

    int64_t now = 1000000;
    LS_EQ_INT(0, p25_geo_select(&g, &p, true, FRANKLIN_NH_LAT, FRANKLIN_NH_LON,
                                now, now));
    /* The Atlantic, well outside every radius. */
    now += 1000000;
    LS_EQ_INT(-1, p25_geo_select(&g, &p, true, 41.0, -69.0, now, now));
}

LS_CASE(a_stale_or_absent_fix_changes_nothing)
{
    p25_profile_t p; route(&p);
    p25_geo_t g; memset(&g, 0, sizeof(g));
    const int64_t now = 100000000;

    /* No fix at all. */
    LS_EQ_INT(-1, p25_geo_select(&g, &p, false, FRANKLIN_NH_LAT,
                                 FRANKLIN_NH_LON, now, now));
    /* Older than the freshness limit. */
    LS_EQ_INT(-1, p25_geo_select(&g, &p, true, FRANKLIN_NH_LAT, FRANKLIN_NH_LON,
                                 now - P25_GEO_FIX_MAX_AGE_US - 1, now));
    /* From the future, which is what a GPS clock sync looks like to a naive
       age check - and it reads as fresh forever. */
    LS_EQ_INT(-1, p25_geo_select(&g, &p, true, FRANKLIN_NH_LAT, FRANKLIN_NH_LON,
                                 now + 1000000, now));
    /* Nonsense coordinates. */
    LS_EQ_INT(-1, p25_geo_select(&g, &p, true, 91.0, 0.0, now, now));
    LS_EQ_INT(-1, p25_geo_select(&g, &p, true, 0.0, 181.0, now, now));
    LS_EQ_INT(-1, p25_geo_select(&g, &p, true, NAN, 0.0, now, now));
    LS_EQ_INT(-1, p25_geo_select(&g, &p, true, 0.0, INFINITY, now, now));
}

LS_CASE(a_profile_without_coordinates_is_left_alone)
{
    /* Single-site profiles are the normal case and are not broken. This
       has nothing to do with them and must not touch their tuning. */
    p25_profile_t p;
    memset(&p, 0, sizeof(p));
    p.format_version = 1;
    p.control_count = 3;
    p.control_channels[0] = 851012500u;
    p.control_channels[1] = 851287500u;
    p.control_channels[2] = 852237500u;

    LS_CHECK(!p25_geo_profile_has_sites(&p));

    p25_geo_t g; memset(&g, 0, sizeof(g));
    const int64_t now = 1000000;
    LS_EQ_INT(-1, p25_geo_select(&g, &p, true, FRANKLIN_NH_LAT,
                                 FRANKLIN_NH_LON, now, now));
}

LS_CASE(a_site_with_an_implausible_radius_is_ignored_not_trusted)
{
    /* Metres typed as kilometres. Twenty-five is not a coverage radius, and
       a site that can never be entered would silently never be chosen -
       so it is excluded from "has sites" too, rather than making an
       all-unusable profile look followable. */
    p25_profile_t p;
    memset(&p, 0, sizeof(p));
    p.format_version = 2;
    site(&p, 0, 851012500u, FRANKLIN_NH_LAT, FRANKLIN_NH_LON, 25);
    LS_CHECK(!p25_geo_profile_has_sites(&p));

    site(&p, 1, 851287500u, CONCORD_NH_LAT, CONCORD_NH_LON, 30000);
    LS_CHECK(p25_geo_profile_has_sites(&p));

    p25_geo_t g; memset(&g, 0, sizeof(g));
    const int64_t now = 1000000;
    /* Standing on the bad site picks the good one, not the one underfoot. */
    LS_EQ_INT(1, p25_geo_select(&g, &p, true, CONCORD_NH_LAT, CONCORD_NH_LON,
                                now, now));
}

LS_CASE(the_answer_survives_a_tunnel_and_then_expires)
{
    p25_profile_t p; route(&p);
    p25_geo_t g; memset(&g, 0, sizeof(g));

    int64_t now = 1000000;
    LS_EQ_INT(0, p25_geo_select(&g, &p, true, FRANKLIN_NH_LAT, FRANKLIN_NH_LON,
                                now, now));
    LS_CHECK(p25_geo_ready(&g, now));
    /* Still good most of the way through the hold. */
    LS_CHECK(p25_geo_ready(&g, now + P25_GEO_HOLD_US - 1));
    /* And not after it - a decision made a minute ago is a decision made a
       mile back. */
    LS_CHECK(!p25_geo_ready(&g, now + P25_GEO_HOLD_US + 1));
}

LS_CASE(null_arguments_are_refused_rather_than_dereferenced)
{
    p25_profile_t p; route(&p);
    p25_geo_t g; memset(&g, 0, sizeof(g));
    const int64_t now = 1000000;
    LS_EQ_INT(-1, p25_geo_select(NULL, &p, true, 43.0, -71.0, now, now));
    LS_EQ_INT(-1, p25_geo_select(&g, NULL, true, 43.0, -71.0, now, now));
    LS_CHECK(!p25_geo_profile_has_sites(NULL));
    LS_CHECK(!p25_geo_ready(NULL, now));
}
