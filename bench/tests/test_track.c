/* LS_TEST_SOURCES: ${FW}/components/lakeshark/core/ls_track.c */

#include "ls_test.h"
#include "ls_track.h"

#include <string.h>

#define LAT_E7  432000000
#define LON_E7 (-714000000)

LS_CASE(a_degree_of_longitude_is_shorter_than_one_of_latitude)
{
    /* The whole reason the cosine is there. At 43 north a degree of
       longitude is about 73 percent of a degree of latitude, so an east-west
       step must measure SHORTER than the same step north-south. Without the
       correction an east-west walk reads about 37 percent long and the
       movement threshold fires early going one way and late going the
       other. */
    const float north = ls_track_distance_m(LAT_E7, LON_E7,
                                            LAT_E7 + 1000000, LON_E7);
    const float east  = ls_track_distance_m(LAT_E7, LON_E7,
                                            LAT_E7, LON_E7 + 1000000);
    LS_CHECK(east < north);
    /* cos(43 deg) is 0.731. Allow a little either side of it. */
    LS_CHECK(east > north * 0.70f && east < north * 0.76f);
}

LS_CASE(a_tenth_of_a_degree_of_latitude_is_about_eleven_kilometres)
{
    /* An absolute check, so a sign error or a factor of ten cannot hide
       behind a ratio. 0.1 degree is 11.132 km by definition of the metre. */
    const float d = ls_track_distance_m(LAT_E7, LON_E7,
                                        LAT_E7 + 1000000, LON_E7);
    LS_CHECK(d > 11000.0f && d < 11300.0f);
}

LS_CASE(distance_is_symmetric_and_zero_to_itself)
{
    LS_CHECK(ls_track_distance_m(LAT_E7, LON_E7, LAT_E7, LON_E7) == 0.0f);
    const float a = ls_track_distance_m(LAT_E7, LON_E7,
                                        LAT_E7 + 300, LON_E7 - 900);
    const float b = ls_track_distance_m(LAT_E7 + 300, LON_E7 - 900,
                                        LAT_E7, LON_E7);
    LS_CHECK(a > 0.0f);
    LS_CHECK(a - b < 0.01f && b - a < 0.01f);
}

LS_CASE(a_track_starts_where_the_operator_did)
{

    LS_CHECK(ls_track_should_log(0.0f, 0, 10.0f, 30));
}

LS_CASE(standing_still_does_not_fill_the_ring)
{
    /* Six hundred fixes standing in one place must not push out the walk
       that came before. Below the move threshold and inside the gap, nothing
       is kept. */
    for (uint32_t s = 1; s < 30; s++)
        LS_CHECK(!ls_track_should_log(0.4f, s, 10.0f, 30));
}

LS_CASE(moving_far_enough_is_kept_and_so_is_waiting_long_enough)
{
    LS_CHECK(ls_track_should_log(10.0f, 1, 10.0f, 30));
    LS_CHECK(ls_track_should_log(99.0f, 1, 10.0f, 30));

    LS_CHECK(ls_track_should_log(0.0f, 30, 10.0f, 30));
    LS_CHECK(ls_track_should_log(0.0f, 31, 10.0f, 30));
}

LS_CASE(a_zero_threshold_turns_its_own_rule_off_rather_than_firing_always)
{
    /* min_move 0 must not mean "every fix has moved far enough" - that is
       the reading that fills the ring in ten minutes. */
    LS_CHECK(!ls_track_should_log(0.5f, 5, 0.0f, 30));
    LS_CHECK(!ls_track_should_log(500.0f, 5, 0.0f, 0));
    LS_CHECK(ls_track_should_log(500.0f, 5, 10.0f, 0));
}

/* ----------------------------------------------------------------- gpx -- */

LS_CASE(a_point_carries_seven_decimals_and_both_signs)
{
    char buf[256];
    ls_track_pt_t p = { 0 };
    p.lat_e7 = LAT_E7;
    p.lon_e7 = LON_E7;          /* negative: western hemisphere */
    p.alt_m  = 103;
    p.sats   = 9;

    LS_CHECK(ls_track_gpx_point(buf, sizeof(buf), &p) > 0);
    /* Seven decimals. Six is 11 cm and shows as stair-stepping on a drawn
       track; this is the resolution every interchange format uses. */
    LS_CHECK(strstr(buf, "lat=\"43.2000000\"") != NULL);
    LS_CHECK(strstr(buf, "lon=\"-71.4000000\"") != NULL);
    LS_CHECK(strstr(buf, "<ele>103</ele>") != NULL);
    LS_CHECK(strstr(buf, "<sat>9</sat>") != NULL);
}

LS_CASE(a_point_with_no_real_date_carries_no_time_at_all)
{
    /* The trap this file exists to avoid. This board's RTC may not
       be set, in which case a stamp is an uptime - and writing that out as a
       date would put the whole walk in January 1970. GPX allows a trkpt with
       no time and every reader handles it; a wrong date is not handled by
       anything, it is believed. */
    char buf[256];
    ls_track_pt_t p = { 0 };
    p.lat_e7 = LAT_E7; p.lon_e7 = LON_E7;
    p.t = 1440;                 /* twenty-four minutes of uptime */
    p.flags = 0;                /* and NOT an epoch              */

    LS_CHECK(ls_track_gpx_point(buf, sizeof(buf), &p) > 0);
    LS_CHECK(strstr(buf, "<time>") == NULL);
    LS_CHECK(strstr(buf, "1970") == NULL);

    /* And with the flag, a real ISO 8601 instant in UTC. */
    p.t = 1757560000u;          /* 2025-09-11 in the epoch */
    p.flags = LS_TRACK_F_EPOCH;
    LS_CHECK(ls_track_gpx_point(buf, sizeof(buf), &p) > 0);
    LS_CHECK(strstr(buf, "<time>20") != NULL);
    LS_CHECK(strstr(buf, "Z</time>") != NULL);
}

LS_CASE(a_buffer_too_small_writes_nothing_rather_than_half_a_tag)
{
    /* Half a trkpt in the middle of a file is a file that opens as nothing.
       Refusing is the only safe failure. */
    char buf[16];
    ls_track_pt_t p = { 0 };
    p.lat_e7 = LAT_E7; p.lon_e7 = LON_E7;
    LS_EQ_INT(0, ls_track_gpx_point(buf, sizeof(buf), &p));
    LS_EQ_INT(0, (int)strlen(buf));

    LS_EQ_INT(0, ls_track_gpx_header(buf, sizeof(buf), "x"));
    LS_EQ_INT(0, (int)strlen(buf));
}

LS_CASE(the_document_opens_and_closes_as_gpx_1_1)
{
    char head[256], foot[64];
    LS_CHECK(ls_track_gpx_header(head, sizeof(head), "walk to the lake") > 0);
    LS_CHECK(strstr(head, "<?xml") != NULL);
    LS_CHECK(strstr(head, "version=\"1.1\"") != NULL);
    LS_CHECK(strstr(head, "topografix.com/GPX/1/1") != NULL);
    LS_CHECK(strstr(head, "<name>walk to the lake</name>") != NULL);

    /* The header must NOT open the track, because GPX 1.1 fixes the
       order of a document's children - metadata, wpt, rte, trk - and a
       header that opened <trk> would leave nowhere legal for a waypoint. */
    LS_CHECK(strstr(head, "<trkseg>") == NULL);
    LS_CHECK(strstr(head, "<trk>") == NULL);

    LS_CHECK(ls_track_gpx_footer(foot, sizeof(foot)) > 0);
    LS_CHECK(strstr(foot, "</gpx>") != NULL);
}

LS_CASE(a_whole_document_comes_out_in_the_order_the_schema_wants)
{
    /* Assembled the way ls_track_export assembles it, and checked as
       one string: the properties that matter are between the pieces, so
       testing them one at a time would pass on a document no reader will
       validate. */
    char doc[1024];
    size_t used = 0;
    char part[256];

    used += (size_t)ls_track_gpx_header(doc + used, sizeof(doc) - used, "walk");

    ls_track_pt_t w = { 0 };
    w.lat_e7 = LAT_E7; w.lon_e7 = LON_E7;
    int n = ls_track_gpx_waypoint(part, sizeof(part), &w, "TBAY", "-39 dBm");
    LS_CHECK(n > 0);
    memcpy(doc + used, part, (size_t)n); used += (size_t)n;

    used += (size_t)ls_track_gpx_trk_open(doc + used, sizeof(doc) - used, "walk");

    ls_track_pt_t p = { 0 };
    p.lat_e7 = LAT_E7 + 100; p.lon_e7 = LON_E7 + 100; p.alt_m = 90;
    n = ls_track_gpx_point(part, sizeof(part), &p);
    LS_CHECK(n > 0);
    memcpy(doc + used, part, (size_t)n); used += (size_t)n;

    used += (size_t)ls_track_gpx_trk_close(doc + used, sizeof(doc) - used);
    used += (size_t)ls_track_gpx_footer(doc + used, sizeof(doc) - used);
    doc[used] = 0;

    const char *xml  = strstr(doc, "<?xml");
    const char *meta = strstr(doc, "<metadata>");
    const char *wpt  = strstr(doc, "<wpt ");
    const char *trk  = strstr(doc, "<trk>");
    const char *seg  = strstr(doc, "<trkseg>");
    const char *pt   = strstr(doc, "<trkpt ");
    const char *cseg = strstr(doc, "</trkseg>");
    const char *ctrk = strstr(doc, "</trk>");
    const char *cgpx = strstr(doc, "</gpx>");

    LS_CHECK(xml && meta && wpt && trk && seg && pt && cseg && ctrk && cgpx);
    /* metadata, then waypoints, then the track. That is the schema. */
    LS_CHECK(xml < meta);
    LS_CHECK(meta < wpt);
    LS_CHECK(wpt < trk);
    LS_CHECK(trk < seg);
    LS_CHECK(seg < pt);
    LS_CHECK(pt < cseg);
    LS_CHECK(cseg < ctrk);
    LS_CHECK(ctrk < cgpx);
    /* Every element that opens, closes. */
    LS_CHECK(strstr(doc, "</wpt>") != NULL);
}

LS_CASE(a_waypoint_names_the_node_and_carries_its_place)
{
    char buf[256];
    ls_track_pt_t p = { 0 };
    p.lat_e7 = LAT_E7; p.lon_e7 = LON_E7;

    LS_CHECK(ls_track_gpx_waypoint(buf, sizeof(buf), &p,
                                   "TBAY", "-39 dBm, 14:22") > 0);
    LS_CHECK(strstr(buf, "lat=\"43.2000000\"") != NULL);
    LS_CHECK(strstr(buf, "lon=\"-71.4000000\"") != NULL);
    LS_CHECK(strstr(buf, "<name>TBAY</name>") != NULL);
    LS_CHECK(strstr(buf, "<desc>-39 dBm, 14:22</desc>") != NULL);

    LS_CHECK(ls_track_gpx_waypoint(buf, sizeof(buf), &p, NULL, NULL) > 0);
    LS_CHECK(strstr(buf, "<name>node</name>") != NULL);

    char tiny[16];
    LS_EQ_INT(0, ls_track_gpx_waypoint(tiny, sizeof(tiny), &p, "TBAY", ""));
    LS_EQ_INT(0, (int)strlen(tiny));
}

LS_CASE(nulls_do_not_crash_the_formatter)
{
    char buf[64];
    LS_EQ_INT(0, ls_track_gpx_point(buf, sizeof(buf), NULL));
    LS_EQ_INT(0, ls_track_gpx_point(NULL, 10, NULL));
    LS_EQ_INT(0, ls_track_gpx_header(NULL, 10, "x"));
    /* A nameless track still opens - the name is decoration, the document
       is not. Its own buffer, because the header is about 170 bytes and the
       64 above is the too-small case two lines up. */
    char room[256];
    LS_CHECK(ls_track_gpx_header(room, sizeof(room), NULL) > 0);
}
