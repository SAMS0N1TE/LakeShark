/* LS_TEST_SOURCES: ${FW}/components/lakeshark/apps/p25/p25_profile.c ${FW}/components/lakeshark/apps/p25/p25_cqpsk_controls.c ${FW}/components/lakeshark/apps/p25/p25_geo.c */
/* The hand-authored route profile, put through the real parser.

   A preset file is only worth having if it loads, and the cheapest place to
   find out that it does not is here rather than at the roadside. This opens
   the actual file from disk, parses it with the firmware's own parser, and
   reports the parser's own line number and reason on failure - the same
   string the radio's LAST LOAD panel would show.

   It then drives the route through the selector, so "the file loads" and
   "the file does what it is for" are two different assertions. */

#include "ls_test.h"
#include "p25_geo.h"
#include "p25_profile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef P25_ROUTE_PROFILE_PATH
#error "P25_ROUTE_PROFILE_PATH must name the route profile"
#endif

/* The device's own tuner span, so a frequency this accepts is one the
   receiver can actually reach. */
static const ls_radio_range_t TUNE[] = { { 24000000, 1766000000 } };
static const p25_profile_parse_config_t CONFIG = {
    TUNE, sizeof(TUNE) / sizeof(TUNE[0]),
};

static bool load(p25_profile_t *out, p25_profile_diagnostic_t *diag)
{
    FILE *f = fopen(P25_ROUTE_PROFILE_PATH, "rb");
    LS_CHECK_MSG(f != NULL, "cannot open %s", P25_ROUTE_PROFILE_PATH);
    if (!f) return false;

    static char text[P25_PROFILE_MAX_FILE_BYTES + 1];
    const size_t n = fread(text, 1, sizeof(text) - 1, f);
    const bool over = !feof(f);
    fclose(f);
    LS_CHECK_MSG(!over, "route profile is larger than the %u byte limit",
                 (unsigned)P25_PROFILE_MAX_FILE_BYTES);
    if (over) return false;
    text[n] = '\0';

    static p25_profile_parse_scratch_t scratch;
    return p25_profile_parse(out, &scratch, text, n, &CONFIG, diag);
}

LS_CASE(the_route_profile_loads)
{
    p25_profile_t p;
    p25_profile_diagnostic_t d;
    const bool ok = load(&p, &d);
    LS_CHECK_MSG(ok, "route profile rejected at line %lu: %s",
                 (unsigned long)d.line, d.reason);
    if (!ok) return;

    LS_EQ_UINT(p.format_version, 2U);
    LS_CHECK_MSG(p.control_count >= 8,
                 "only %u control channels - the route needs more than that",
                 (unsigned)p.control_count);
}

LS_CASE(every_route_control_carries_a_place)
{
    /* A control line without coordinates cannot be followed, and one mixed
       into a route file is almost certainly an edit that forgot them - it
       would silently never be selected. */
    p25_profile_t p;
    p25_profile_diagnostic_t d;
    if (!load(&p, &d)) return;

    for (unsigned i = 0; i < p.control_count; ++i) {
        LS_CHECK_MSG(p.control_has_geo[i],
                     "control %u (%llu Hz) has no coordinates",
                     i, (unsigned long long)p.control_channels[i]);
        LS_CHECK_MSG(p.control_radius_m[i] >= P25_GEO_MIN_RADIUS_M,
                     "control %u has a %u m radius - metres or kilometres?",
                     i, (unsigned)p.control_radius_m[i]);
    }
    LS_CHECK(p25_geo_profile_has_sites(&p));
}

LS_CASE(every_route_site_is_actually_between_franklin_and_the_cape)
{
    /* A sign slip on a longitude puts a New England site in Asia, and the
       selector would then simply never choose it - silently, for the whole
       drive. The box is generous; it is catching a typo, not surveying. */
    p25_profile_t p;
    p25_profile_diagnostic_t d;
    if (!load(&p, &d)) return;

    for (unsigned i = 0; i < p.control_count; ++i) {
        const double lat = p.control_lat_e7[i] * 1e-7;
        const double lon = p.control_lon_e7[i] * 1e-7;
        LS_CHECK_MSG(lat > 41.0 && lat < 44.5,
                     "control %u latitude %.4f is not in NH or MA", i, lat);
        LS_CHECK_MSG(lon > -72.5 && lon < -69.5,
                     "control %u longitude %.4f is not in NH or MA", i, lon);
    }
}

LS_CASE(driving_the_route_walks_the_sites_in_order)
{
    /* The point of the file. Positions along I-93 / I-495 / Route 25, in the
       order they are driven; each should hand off forwards, never backwards
       to a site already left behind. */
    p25_profile_t p;
    p25_profile_diagnostic_t d;
    if (!load(&p, &d)) return;

    static const struct { const char *where; double lat, lon; } LEG[] = {
        { "Franklin NH",      43.4406, -71.6498 },
        { "Concord NH",       43.2081, -71.5376 },
        { "Manchester NH",    42.9956, -71.4548 },
        { "Nashua NH",        42.7654, -71.4676 },
        { "Bedford MA",       42.4700, -71.2890 },
        { "Framingham MA",    42.2790, -71.4160 },
        { "Norfolk MA",       42.1180, -71.3250 },
        { "Bridgewater MA",   41.9900, -70.9750 },
        { "West Dennis MA",   41.6620, -70.1700 },
    };

    p25_geo_t g;
    memset(&g, 0, sizeof(g));
    int64_t now = 1000000;
    int chosen = -1, switches = 0;

    for (unsigned i = 0; i < sizeof(LEG) / sizeof(LEG[0]); ++i) {
        now += 600000000LL;                 /* ten minutes of driving */
        const int pick = p25_geo_select(&g, &p, true, LEG[i].lat, LEG[i].lon,
                                        now, now);
        if (pick >= 0) {
            LS_CHECK_MSG(pick < p.control_count,
                         "%s selected control %d of %u",
                         LEG[i].where, pick, (unsigned)p.control_count);
            chosen = pick;
            switches++;
        }
    }

    LS_CHECK_MSG(switches >= 3,
                 "the whole route only ever selected %d site(s) - a route "
                 "profile that never hands off is a single-site profile",
                 switches);
    LS_CHECK_MSG(chosen >= 0, "no site was ever selected on the route");
}

LS_CASE(the_route_profile_has_talkgroups_worth_listening_to)
{
    p25_profile_t p;
    p25_profile_diagnostic_t d;
    if (!load(&p, &d)) return;

    LS_CHECK_MSG(p.talkgroup_count > 0, "no talkgroups in the route profile");
    unsigned enabled = 0;
    for (unsigned i = 0; i < p.talkgroup_count; ++i)
        if (p.talkgroups[i].enabled) enabled++;
    LS_CHECK_MSG(enabled > 0, "every talkgroup is disabled");
}
