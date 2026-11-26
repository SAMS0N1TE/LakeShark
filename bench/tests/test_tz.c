#include "ls_test.h"
#include "ls_tz.h"
#include "ls_time.h"

#include <string.h>

static time_t utc(int y, int mo, int d, int h, int mi, int s)
{
    y -= mo <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const long yoe = y - era * 400;
    const long doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (time_t)((era * 146097 + doe - 719468) * 86400LL + h * 3600 + mi * 60 + s);
}

static const char *pick(double lat, double lon)
{
    static char label[24];
    char posix[40];
    ls_tz_pick(lat, lon, posix, sizeof(posix), label, sizeof(label));
    return label;
}

static int east(const char *posix, time_t t)
{
    int e = 99999;
    LS_CHECK(ls_tz_offset_at(posix, t, &e, NULL));
    return e;
}

LS_CASE(auto_picks_the_us_zones_by_their_towns)
{
    LS_EQ_STR(pick(40.71, -74.00), "US Eastern");    /* New York */
    LS_EQ_STR(pick(25.76, -80.19), "US Eastern");    /* Miami */
    LS_EQ_STR(pick(26.64, -81.87), "US Eastern");    /* Fort Myers */
    LS_EQ_STR(pick(30.44, -84.28), "US Eastern");    /* Tallahassee */
    LS_EQ_STR(pick(30.42, -87.22), "US Central");    /* Pensacola */
    LS_EQ_STR(pick(39.77, -86.16), "US Eastern");    /* Indianapolis */
    LS_EQ_STR(pick(38.25, -85.76), "US Eastern");    /* Louisville */
    LS_EQ_STR(pick(35.96, -83.92), "US Eastern");    /* Knoxville */
    LS_EQ_STR(pick(36.16, -86.78), "US Central");    /* Nashville */
    LS_EQ_STR(pick(42.33, -83.05), "US Eastern");    /* Detroit */
    LS_EQ_STR(pick(41.88, -87.63), "US Central");    /* Chicago */
    LS_EQ_STR(pick(32.78, -96.80), "US Central");    /* Dallas */
    LS_EQ_STR(pick(31.76, -106.49), "US Mountain");  /* El Paso */
    LS_EQ_STR(pick(44.08, -103.23), "US Mountain");  /* Rapid City */
    LS_EQ_STR(pick(39.74, -104.99), "US Mountain");  /* Denver */
    LS_EQ_STR(pick(43.62, -116.20), "US Mountain");  /* Boise */
    LS_EQ_STR(pick(33.45, -112.07), "Arizona");      /* Phoenix */
    LS_EQ_STR(pick(36.17, -115.14), "US Pacific");   /* Las Vegas */
    LS_EQ_STR(pick(34.05, -118.24), "US Pacific");   /* Los Angeles */
    LS_EQ_STR(pick(47.61, -122.33), "US Pacific");   /* Seattle */
    LS_EQ_STR(pick(61.22, -149.90), "Alaska");       /* Anchorage */
    LS_EQ_STR(pick(21.31, -157.86), "Hawaii");       /* Honolulu */
}

LS_CASE(auto_picks_canada_mexico_and_south_america)
{
    LS_EQ_STR(pick(43.65, -79.38), "US Eastern");    /* Toronto: the same rules */
    LS_EQ_STR(pick(44.65, -63.57), "Atlantic");      /* Halifax */
    LS_EQ_STR(pick(47.56, -52.71), "Newfoundland");  /* St John's */
    LS_EQ_STR(pick(50.45, -104.61), "Saskatchewan"); /* Regina */
    LS_EQ_STR(pick(51.05, -114.07), "US Mountain");  /* Calgary */
    LS_EQ_STR(pick(49.28, -123.12), "US Pacific");   /* Vancouver */
    LS_EQ_STR(pick(19.43, -99.13), "Mexico City");
    LS_EQ_STR(pick(-34.60, -58.38), "Argentina");    /* Buenos Aires */
    LS_EQ_STR(pick(-32.89, -68.84), "Argentina");    /* Mendoza */
    LS_EQ_STR(pick(-54.80, -68.30), "Argentina");    /* Ushuaia */
    LS_CHECK(strcmp(pick(-33.45, -70.67), "Argentina") != 0);   /* Santiago is Chile */
}

LS_CASE(auto_picks_europe_and_the_rest)
{
    LS_EQ_STR(pick(51.51, -0.13), "UK / Ireland");
    LS_EQ_STR(pick(53.35, -6.26), "UK / Ireland");   /* Dublin */
    LS_EQ_STR(pick(38.72, -9.14), "Portugal");       /* Lisbon */
    LS_EQ_STR(pick(40.42, -3.70), "Central Europe"); /* Madrid */
    LS_EQ_STR(pick(48.86, 2.35), "Central Europe");  /* Paris */
    LS_EQ_STR(pick(50.95, 1.85), "Central Europe");  /* Calais */
    LS_EQ_STR(pick(52.52, 13.40), "Central Europe"); /* Berlin */
    LS_EQ_STR(pick(52.23, 21.01), "Central Europe"); /* Warsaw */
    LS_EQ_STR(pick(59.33, 18.07), "Central Europe"); /* Stockholm */
    LS_EQ_STR(pick(60.17, 24.94), "Eastern Europe"); /* Helsinki */
    LS_EQ_STR(pick(37.98, 23.73), "Eastern Europe"); /* Athens */
    LS_EQ_STR(pick(44.43, 26.10), "Eastern Europe"); /* Bucharest */
    LS_EQ_STR(pick(50.45, 30.52), "Eastern Europe"); /* Kyiv */
    LS_EQ_STR(pick(35.68, 139.69), "Japan / Korea");
    LS_EQ_STR(pick(28.61, 77.21), "India");
    LS_EQ_STR(pick(-33.87, 151.21), "Sydney");
    /* anywhere without a named zone: the hour band, as a number */
    LS_EQ_STR(pick(-27.47, 153.03), "UTC+10");       /* Brisbane keeps no summer time */
    LS_EQ_STR(pick(0.0, -160.0), "UTC-11");
    char posix[40], label[24];
    LS_EQ_INT(ls_tz_pick(0.0, -160.0, posix, sizeof(posix), label, sizeof(label)), -1);
    LS_EQ_INT(east(posix, utc(2026, 7, 1, 0, 0, 0)), -11 * 3600);
}

LS_CASE(us_daylight_time_starts_and_ends_on_the_right_second)
{
    const char *et = "EST5EDT,M3.2.0,M11.1.0";
    LS_EQ_INT(east(et, utc(2026, 1, 15, 12, 0, 0)), -5 * 3600);
    LS_EQ_INT(east(et, utc(2026, 7, 1, 12, 0, 0)), -4 * 3600);
    /* 2026: 2:00 EST on 8 March, 2:00 EDT on 1 November */
    LS_EQ_INT(east(et, utc(2026, 3, 8, 6, 59, 59)), -5 * 3600);
    LS_EQ_INT(east(et, utc(2026, 3, 8, 7, 0, 0)), -4 * 3600);
    LS_EQ_INT(east(et, utc(2026, 11, 1, 5, 59, 59)), -4 * 3600);
    LS_EQ_INT(east(et, utc(2026, 11, 1, 6, 0, 0)), -5 * 3600);
    /* 2027: 14 March and 7 November */
    LS_EQ_INT(east(et, utc(2027, 3, 14, 7, 0, 0)), -4 * 3600);
    LS_EQ_INT(east(et, utc(2027, 3, 14, 6, 59, 0)), -5 * 3600);
    LS_EQ_INT(east(et, utc(2027, 11, 7, 6, 0, 0)), -5 * 3600);
    LS_EQ_INT(east(ls_tz_posix(6), utc(2026, 7, 1, 0, 0, 0)), -7 * 3600);   /* PDT */
}

LS_CASE(europe_changes_at_one_utc_everywhere)
{
    const char *cet = "CET-1CEST,M3.5.0,M10.5.0/3", *uk = "GMT0BST,M3.5.0/1,M10.5.0",
               *eet = "EET-2EEST,M3.5.0/3,M10.5.0/4";
    /* 2026: 29 March and 25 October, 01:00 UTC */
    LS_EQ_INT(east(cet, utc(2026, 3, 29, 0, 59, 59)), 3600);
    LS_EQ_INT(east(cet, utc(2026, 3, 29, 1, 0, 0)), 7200);
    LS_EQ_INT(east(cet, utc(2026, 10, 25, 0, 59, 59)), 7200);
    LS_EQ_INT(east(cet, utc(2026, 10, 25, 1, 0, 0)), 3600);
    LS_EQ_INT(east(uk, utc(2026, 3, 29, 1, 0, 0)), 3600);
    LS_EQ_INT(east(uk, utc(2026, 3, 29, 0, 59, 0)), 0);
    LS_EQ_INT(east(uk, utc(2026, 10, 25, 1, 0, 0)), 0);
    LS_EQ_INT(east(eet, utc(2026, 3, 29, 1, 0, 0)), 3 * 3600);
    LS_EQ_INT(east(eet, utc(2026, 10, 25, 0, 59, 0)), 3 * 3600);
    LS_EQ_INT(east(eet, utc(2026, 10, 25, 1, 0, 0)), 2 * 3600);
}

LS_CASE(the_southern_summer_and_the_odd_offsets)
{
    const char *syd = "AEST-10AEDT,M10.1.0,M4.1.0/3";
    LS_EQ_INT(east(syd, utc(2026, 1, 15, 0, 0, 0)), 11 * 3600);
    LS_EQ_INT(east(syd, utc(2026, 7, 15, 0, 0, 0)), 10 * 3600);
    /* ends 3:00 AEDT 5 April 2026 (16:00Z on the 4th), starts 2:00 AEST 4 October (16:00Z on the 3rd) */
    LS_EQ_INT(east(syd, utc(2026, 4, 4, 15, 59, 0)), 11 * 3600);
    LS_EQ_INT(east(syd, utc(2026, 4, 4, 16, 0, 0)), 10 * 3600);
    LS_EQ_INT(east(syd, utc(2026, 10, 3, 15, 59, 0)), 10 * 3600);
    LS_EQ_INT(east(syd, utc(2026, 10, 3, 16, 0, 0)), 11 * 3600);
    LS_EQ_INT(east("ART3", utc(2026, 1, 1, 0, 0, 0)), -3 * 3600);
    LS_EQ_INT(east("ART3", utc(2026, 7, 1, 0, 0, 0)), -3 * 3600);
    LS_EQ_INT(east("IST-5:30", utc(2026, 7, 1, 0, 0, 0)), 5 * 3600 + 1800);
    LS_EQ_INT(east("NST3:30NDT,M3.2.0,M11.1.0", utc(2026, 1, 1, 0, 0, 0)), -(3 * 3600 + 1800));
    LS_EQ_INT(east("<-03>3", utc(2026, 1, 1, 0, 0, 0)), -3 * 3600);
    LS_EQ_INT(east("UTC0", utc(2026, 1, 1, 0, 0, 0)), 0);
}

LS_CASE(every_named_zone_parses_and_bad_strings_do_not)
{
    LS_CHECK(ls_tz_posix(LS_TZ_AUTO) == NULL);
    for (int i = 1; i < ls_tz_count(); i++) {
        int e;
        LS_CHECK(ls_tz_offset_at(ls_tz_posix(i), utc(2026, 6, 1, 0, 0, 0), &e, NULL));
        LS_CHECK(e >= -12 * 3600 && e <= 14 * 3600);
        LS_CHECK(strlen(ls_tz_name(i)) <= 15);
    }
    LS_CHECK(!ls_tz_offset_at("", 0, NULL, NULL));
    LS_CHECK(!ls_tz_offset_at("EST", 0, NULL, NULL));
    LS_CHECK(!ls_tz_offset_at("EST5EDT,M3.2", 0, NULL, NULL));
    LS_CHECK(!ls_tz_offset_at(NULL, 0, NULL, NULL));
}

LS_CASE(abbreviations_say_which_time_it_is)
{
    char a[12];
    ls_tz_abbr_of("EST5EDT,M3.2.0,M11.1.0", false, a, sizeof(a)); LS_EQ_STR(a, "EST");
    ls_tz_abbr_of("EST5EDT,M3.2.0,M11.1.0", true, a, sizeof(a));  LS_EQ_STR(a, "EDT");
    ls_tz_abbr_of("ART3", true, a, sizeof(a));                    LS_EQ_STR(a, "ART");
    ls_tz_abbr_of("UTC-10", false, a, sizeof(a));                 LS_EQ_STR(a, "UTC+10");
    ls_tz_abbr_of("UTC0", false, a, sizeof(a));                   LS_EQ_STR(a, "UTC");
}

LS_CASE(the_status_clock_is_a_phone_clock_in_six_columns)
{
    char c[12];
    ls_tz_clock_text(c, sizeof(c), 16, 34, false); LS_EQ_STR(c, "4:34p");
    ls_tz_clock_text(c, sizeof(c), 0, 5, false);   LS_EQ_STR(c, "12:05a");
    ls_tz_clock_text(c, sizeof(c), 12, 0, false);  LS_EQ_STR(c, "12:00p");
    ls_tz_clock_text(c, sizeof(c), 23, 59, false); LS_EQ_STR(c, "11:59p");
    ls_tz_clock_text(c, sizeof(c), 16, 34, true);  LS_EQ_STR(c, "16:34");
    ls_tz_clock_text(c, sizeof(c), 4, 7, true);    LS_EQ_STR(c, "04:07");
    for (int h = 0; h < 24; h++) {
        ls_tz_clock_text(c, sizeof(c), h, 59, false);
        LS_CHECK(strlen(c) <= 6);
    }
}

LS_CASE(a_chosen_zone_is_what_local_time_and_the_status_clock_use)
{
    ls_tz_set_choice(2);                                   /* US Eastern */
    LS_EQ_STR(ls_tz_current_label(), "US Eastern");
    struct tm tm;
    /* 04:34Z on 25 Nov 2026, the owner's screenshot: 11:34 PM the day before in Florida */
    LS_CHECK(ls_tz_local(utc(2026, 11, 25, 4, 34, 0), &tm));
    LS_EQ_INT(tm.tm_hour, 23);
    LS_EQ_INT(tm.tm_mday, 24);
    char a[12];
    ls_tz_abbr(&tm, a, sizeof(a));
    LS_EQ_STR(a, "EST");
    char s[32];
    ls_tz_show(s, sizeof(s));
    LS_EQ_STR(s, "US Eastern");

    ls_time_test_set_synced(false);
    ls_tz_status_clock(s, sizeof(s));
    LS_EQ_STR(s, "--:--");                                 /* never a time before there is one */

    ls_tz_set_choice(LS_TZ_AUTO);
    ls_tz_show(s, sizeof(s));
    LS_EQ_STR(s, "AUTO: UTC, no fix yet");
    LS_CHECK(ls_tz_is_utc());
    ls_tz_set_choice(99);                                  /* out of range: AUTO */
    LS_EQ_INT(ls_tz_choice(), LS_TZ_AUTO);
}

LS_CASE(gps_time_sets_a_clock_nothing_else_set)
{
    ls_time_test_set_synced(false);
    ls_time_offer_gps(utc(2026, 11, 25, 4, 34, 0), 300);
    LS_CHECK(ls_time_is_synced());
    LS_EQ_INT(ls_time_source(), 3);
    ls_time_offer_gps(1000, 0);                            /* 1970: not a time */
    LS_EQ_INT(ls_time_source(), 3);
}
