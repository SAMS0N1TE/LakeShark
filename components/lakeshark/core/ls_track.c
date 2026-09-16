/* See ls_track.h. Pure arithmetic and pure formatting: everything
   here can be checked without a receiver, a card or a sky, which is most of
   what makes a track trustworthy. */
#include "ls_track.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Metres per degree of latitude. Constant enough: the earth is 0.3 percent
   out of round and this is used for a threshold comparison. */
#define M_PER_DEG_LAT 111320.0

uint32_t ls_track_time(unsigned year, unsigned month, unsigned day,
                       unsigned hour, unsigned minute, unsigned second,
                       uint32_t uptime, uint8_t *flags)
{
    static const unsigned days_in_month[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    *flags = 0;
    if (year < 2020 || year > 2105 || month < 1 || month > 12 ||
        day < 1 || hour > 23 || minute > 59 || second > 59) return uptime;
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (day > days_in_month[month - 1] + (month == 2 && leap)) return uptime;
    uint64_t days = 0;
    for (unsigned y = 1970; y < year; ++y)
        days += 365 + (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    for (unsigned m = 1; m < month; ++m)
        days += days_in_month[m - 1] + (m == 2 && leap);
    days += day - 1;
    *flags = LS_TRACK_F_EPOCH;
    return (uint32_t)(days * 86400 + hour * 3600 + minute * 60 + second);
}

bool ls_track_fix_usable(bool fix, double lat, double lon, float altitude,
                          int64_t stamp_us, int64_t now_us)
{
    return fix && isfinite(lat) && lat >= -90 && lat <= 90 &&
        isfinite(lon) && lon >= -180 && lon <= 180 && isfinite(altitude) &&
        altitude >= INT16_MIN && altitude <= INT16_MAX &&
        stamp_us > 0 && now_us >= stamp_us && now_us - stamp_us <= 10000000;
}

float ls_track_distance_m(int32_t lat_a_e7, int32_t lon_a_e7,
                          int32_t lat_b_e7, int32_t lon_b_e7)
{
    const double la = lat_a_e7 / 1e7, lo = lon_a_e7 / 1e7;
    const double lb = lat_b_e7 / 1e7, lz = lon_b_e7 / 1e7;

    /* A degree of longitude shrinks with the cosine of the latitude - it is
       111 km at the equator and 78 at 45 degrees north, which is where this
       board is. Ignoring it would make an east-west walk read as 40 percent
       longer than it was, and the movement threshold would fire early in one
       direction and late in the other. */
    const double mid = (la + lb) * 0.5 * (M_PI / 180.0);
    const double dy = (lb - la) * M_PER_DEG_LAT;
    const double dx = (lz - lo) * M_PER_DEG_LAT * cos(mid);

    return (float)sqrt(dx * dx + dy * dy);
}

bool ls_track_should_log(float moved_m, uint32_t since_s,
                         float min_move_m, uint32_t max_gap_s)
{

    if (moved_m <= 0.0f && since_s == 0) return true;

    if (min_move_m > 0.0f && moved_m >= min_move_m) return true;
    if (max_gap_s > 0 && since_s >= max_gap_s) return true;
    return false;
}

static int fmt_deg(char *out, size_t cap, int32_t e7)
{
    return snprintf(out, cap, "%s%ld.%07ld",
                    e7 < 0 ? "-" : "",
                    (long)(e7 < 0 ? -e7 : e7) / 10000000L,
                    (long)(e7 < 0 ? -e7 : e7) % 10000000L);
}

int ls_track_gpx_point(char *out, size_t cap, const ls_track_pt_t *p)
{
    if (!out || !cap || !p) return 0;

    char lat[24], lon[24];
    fmt_deg(lat, sizeof(lat), p->lat_e7);
    fmt_deg(lon, sizeof(lon), p->lon_e7);

    char when[48];
    when[0] = 0;
    if (p->flags & LS_TRACK_F_EPOCH) {

        const time_t tt = (time_t)p->t;
        struct tm tmv;
#if defined(_WIN32)
        gmtime_s(&tmv, &tt);
#else
        gmtime_r(&tt, &tmv);
#endif
        char iso[32];
        strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tmv);
        snprintf(when, sizeof(when), "<time>%s</time>", iso);
    }

    char sats[32];
    sats[0] = 0;
    if (p->sats) snprintf(sats, sizeof(sats), "<sat>%u</sat>", (unsigned)p->sats);

    const int n = snprintf(out, cap,
                           "<trkpt lat=\"%s\" lon=\"%s\">"
                           "<ele>%d</ele>%s%s</trkpt>\n",
                           lat, lon, (int)p->alt_m, when, sats);
    if (n < 0 || (size_t)n >= cap) { out[0] = 0; return 0; }
    return n;
}

int ls_track_gpx_waypoint(char *out, size_t cap, const ls_track_pt_t *p,
                          const char *name, const char *desc)
{
    if (!out || !cap || !p) return 0;

    char lat[24], lon[24];
    fmt_deg(lat, sizeof(lat), p->lat_e7);
    fmt_deg(lon, sizeof(lon), p->lon_e7);

    const int n = snprintf(out, cap,
                           "<wpt lat=\"%s\" lon=\"%s\">"
                           "<name>%s</name><desc>%s</desc></wpt>\n",
                           lat, lon,
                           name && *name ? name : "node",
                           desc ? desc : "");
    if (n < 0 || (size_t)n >= cap) { out[0] = 0; return 0; }
    return n;
}

int ls_track_gpx_header(char *out, size_t cap, const char *name)
{
    if (!out || !cap) return 0;
    const int n = snprintf(out, cap,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<gpx version=\"1.1\" creator=\"LakeShark\" "
        "xmlns=\"http://www.topografix.com/GPX/1/1\">\n"
        "<metadata><name>%s</name></metadata>\n",
        name && *name ? name : "LakeShark track");
    if (n < 0 || (size_t)n >= cap) { out[0] = 0; return 0; }
    return n;
}

int ls_track_gpx_trk_open(char *out, size_t cap, const char *name)
{
    if (!out || !cap) return 0;
    const int n = snprintf(out, cap, "<trk><name>%s</name><trkseg>\n",
                           name && *name ? name : "track");
    if (n < 0 || (size_t)n >= cap) { out[0] = 0; return 0; }
    return n;
}

int ls_track_gpx_trk_close(char *out, size_t cap)
{
    if (!out || !cap) return 0;
    const int n = snprintf(out, cap, "</trkseg></trk>\n");
    if (n < 0 || (size_t)n >= cap) { out[0] = 0; return 0; }
    return n;
}

int ls_track_gpx_footer(char *out, size_t cap)
{
    if (!out || !cap) return 0;
    const int n = snprintf(out, cap, "</gpx>\n");
    if (n < 0 || (size_t)n >= cap) { out[0] = 0; return 0; }
    return n;
}
