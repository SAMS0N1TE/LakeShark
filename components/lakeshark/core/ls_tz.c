/* See ls_tz.h. */
#include "ls_tz.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ls_time.h"

/* ------------------------------------------------------------- the zones */

enum {
    Z_AUTO, Z_UTC, Z_EASTERN, Z_CENTRAL, Z_MOUNTAIN, Z_ARIZONA, Z_PACIFIC, Z_ALASKA, Z_HAWAII,
    Z_ATLANTIC, Z_NEWFOUNDLAND, Z_SASK, Z_MEXICO, Z_ARGENTINA, Z_UK, Z_WEST_EU, Z_CENTRAL_EU,
    Z_EAST_EU, Z_INDIA, Z_JAPAN, Z_SYDNEY, Z_COUNT
};

typedef struct { const char *name, *posix; } zone_t;

/* US and Canada change on the second Sunday in March and the first in
   November at 2:00; Europe on the last Sundays of March and October at
   1:00 UTC; Sydney the first Sundays of October and April. */
static const zone_t ZONES[Z_COUNT] = {
    [Z_AUTO]         = { "AUTO",          NULL },
    [Z_UTC]          = { "UTC",           "UTC0" },
    [Z_EASTERN]      = { "US Eastern",    "EST5EDT,M3.2.0,M11.1.0" },
    [Z_CENTRAL]      = { "US Central",    "CST6CDT,M3.2.0,M11.1.0" },
    [Z_MOUNTAIN]     = { "US Mountain",   "MST7MDT,M3.2.0,M11.1.0" },
    [Z_ARIZONA]      = { "Arizona",       "MST7" },
    [Z_PACIFIC]      = { "US Pacific",    "PST8PDT,M3.2.0,M11.1.0" },
    [Z_ALASKA]       = { "Alaska",        "AKST9AKDT,M3.2.0,M11.1.0" },
    [Z_HAWAII]       = { "Hawaii",        "HST10" },
    [Z_ATLANTIC]     = { "Atlantic",      "AST4ADT,M3.2.0,M11.1.0" },
    [Z_NEWFOUNDLAND] = { "Newfoundland",  "NST3:30NDT,M3.2.0,M11.1.0" },
    [Z_SASK]         = { "Saskatchewan",  "CST6" },
    [Z_MEXICO]       = { "Mexico City",   "CST6" },
    [Z_ARGENTINA]    = { "Argentina",     "ART3" },
    [Z_UK]           = { "UK / Ireland",  "GMT0BST,M3.5.0/1,M10.5.0" },
    [Z_WEST_EU]      = { "Portugal",      "WET0WEST,M3.5.0/1,M10.5.0" },
    [Z_CENTRAL_EU]   = { "Central Europe", "CET-1CEST,M3.5.0,M10.5.0/3" },
    [Z_EAST_EU]      = { "Eastern Europe", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    [Z_INDIA]        = { "India",         "IST-5:30" },
    [Z_JAPAN]        = { "Japan / Korea", "JST-9" },
    [Z_SYDNEY]       = { "Sydney",        "AEST-10AEDT,M10.1.0,M4.1.0/3" },
};

int ls_tz_count(void) { return Z_COUNT; }
const char *ls_tz_name(int i) { return i >= 0 && i < Z_COUNT ? ZONES[i].name : "?"; }
const char *ls_tz_posix(int i) { return i > 0 && i < Z_COUNT ? ZONES[i].posix : NULL; }

/* ------------------------------------------------------------- AUTO

   Lines of longitude by band of latitude, close to the real zone lines
   through the towns either side of them, not the lines themselves. */

static int north_america(double lat, double lon)
{
    if (lat >= 18 && lat <= 23 && lon >= -161 && lon <= -154) return Z_HAWAII;
    if (lat >= 51 && lat <= 72 && (lon < -141 || lon > 172)) return Z_ALASKA;
    if (lat >= 54.6 && lat < 60 && lon < -130 && lon >= -141) return Z_ALASKA;   /* the panhandle */
    if (lat < 14.5 || lat > 84 || lon < -141 || lon > -52) return -1;
    /* Mexico south of the Rio Grande's end: one zone, no daylight time */
    if (lat < 25.9 && lon > -117.5 && lon < -86.7) return Z_MEXICO;
    if (lat < 24) return -1;
    if (lon > -60 && lat > 46.5) return Z_NEWFOUNDLAND;
    if (lon > -67 && lat > 43.3) return Z_ATLANTIC;

    const double ec = lat < 30.9 ? -85.0       /* the Florida panhandle */
                    : lat < 35.0 ? -85.2       /* Georgia and Alabama */
                    : lat < 36.7 ? -85.1       /* Tennessee */
                    : lat < 39.1 ? -86.0       /* Kentucky */
                    : lat < 41.76 ? -87.0      /* Indiana */
                    : lat < 47.0 ? -87.2       /* Michigan, Wisconsin, Chicago */
                    : -90.0;                   /* Upper Peninsula, Ontario */
    if (lon >= ec) return Z_EASTERN;

    const double cm = lat >= 49.0 ? -101.4     /* Manitoba */
                    : lat >= 45.94 ? -101.0    /* North Dakota */
                    : lat >= 43.0 ? -100.6     /* South Dakota */
                    : lat >= 40.0 ? -101.25    /* Nebraska */
                    : lat >= 37.0 ? -101.5     /* Kansas */
                    : lat >= 32.0 ? -103.04    /* Oklahoma and Texas panhandles */
                    : -104.9;                  /* El Paso is Mountain */
    if (lon >= cm) return Z_CENTRAL;
    if (lat >= 49.0 && lon >= -110.0) return Z_SASK;

    const double mp = lat >= 53.0 ? -120.0     /* Alberta and BC */
                    : lat >= 49.0 ? -118.0     /* the East Kootenay is Mountain */
                    : lat >= 45.6 ? -115.5     /* the Idaho panhandle is Pacific */
                    : lat >= 42.0 ? -117.0     /* Oregon and Idaho */
                    : lat >= 37.0 ? -114.05    /* Nevada and Utah */
                    : -114.75;                 /* the Colorado river */
    if (lon >= mp) {
        if (lat >= 31.3 && lat < 37.0 && lon <= -109.045) return Z_ARIZONA;
        return Z_MOUNTAIN;
    }
    return Z_PACIFIC;
}

static int europe(double lat, double lon)
{
    if (lat < 27 || lat > 71.5 || lon < -18.5 || lon > 40) return -1;
    /* the Canaries keep Portugal's time */
    if (lat < 29.5 && lon < -13.3) return Z_WEST_EU;
    if (lat < 35.5) return -1;
    if (lat >= 49.8 && lat <= 61 && lon >= -10.7 && lon <= (lat < 51.1 ? 1.4 : 1.8)) return Z_UK;
    if (lat >= 36.9 && lat <= 42.2 && lon >= -9.6 &&
        lon < (lat >= 41.0 ? -6.2 : lat >= 39.5 ? -6.9 : -7.3)) return Z_WEST_EU;
    /* Finland, the Baltics, Greece, and Bulgaria to Ukraine */
    if (lat >= 59.7 && lat < 69.0 && lon >= (lat >= 65.8 ? 23.5 - (lat - 65.8) * 1.2 : 19.5) && lon < 29.5)
        return Z_EAST_EU;
    if (lat >= 53.9 && lat < 59.7 && lon >= 20.9 && lon < 28.2) return Z_EAST_EU;
    if (lat >= 34.8 && lat < 41.2 && lon >= (lat < 40.8 ? 19.3 : 22.9) && lon < 28.3) return Z_EAST_EU;
    if (lat >= 41.2 && lat < 48.3 && lon >= 22.4 && lon < 40) return Z_EAST_EU;
    if (lat >= 48.3 && lat < 52.4 && lon >= 23.8 && lon < 40) return Z_EAST_EU;
    if (lon <= 24.2) return Z_CENTRAL_EU;
    return -1;
}

int ls_tz_pick(double lat, double lon, char *posix, size_t pn, char *label, size_t ln)
{
    int z = north_america(lat, lon);
    if (z < 0) z = europe(lat, lon);
    if (z < 0 && lat >= -55.2 && lat <= -21.7 && lon <= -53.6 &&
        lon > (lat < -38 ? -71.5 : -69.0)) z = Z_ARGENTINA;
    if (z < 0 && lat >= 24 && lat <= 46 && lon >= 122.9 && lon <= 146) z = Z_JAPAN;
    if (z < 0 && lat >= 6.5 && lat <= 35.5 && lon >= 68.1 && lon <= 92.2 &&
        !(lon < 75 && lat > 23.7) && !(lon > 88.7 && lat > 20.7 && lat < 26.6)) z = Z_INDIA;
    if (z < 0 && lat >= -44 && lat <= -28.2 && lon >= 141 && lon <= 154) z = Z_SYDNEY;
    if (z >= 0) {
        if (posix && pn) snprintf(posix, pn, "%s", ZONES[z].posix);
        if (label && ln) snprintf(label, ln, "%s", ZONES[z].name);
        return z;
    }
    /* anywhere else: the hour band, without daylight time */
    int h = (int)lround(lon / 15.0);
    if (h > 14) h = 14;
    if (h < -12) h = -12;
    if (posix && pn) snprintf(posix, pn, h ? "UTC%+d" : "UTC0", -h);
    if (label && ln) snprintf(label, ln, h ? "UTC%+d" : "UTC", h);
    return -1;
}

/* ------------------------------------------------------------- POSIX TZ */

static const char *rd_name(const char *p, char *out, size_t n)
{
    size_t k = 0;
    if (*p == '<') {
        p++;
        while (*p && *p != '>') { if (k + 1 < n) out[k++] = *p; p++; }
        if (*p != '>') return NULL;
        p++;
    } else {
        while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) { if (k + 1 < n) out[k++] = *p; p++; }
    }
    if (out) out[k < n ? k : n - 1] = 0;
    return k >= 3 ? p : NULL;
}

/* [+-]hh[:mm[:ss]] as seconds; POSIX sign: positive is west. */
static const char *rd_off(const char *p, long *s)
{
    int sign = 1;
    if (*p == '+' || *p == '-') { if (*p == '-') sign = -1; p++; }
    if (*p < '0' || *p > '9') return NULL;
    long v = strtol(p, (char **)&p, 10) * 3600;
    if (*p == ':') { p++; v += strtol(p, (char **)&p, 10) * 60; }
    if (*p == ':') { p++; v += strtol(p, (char **)&p, 10); }
    *s = sign * v;
    return p;
}

typedef struct { int m, w, d; long at; } rule_t;

static const char *rd_rule(const char *p, rule_t *r)
{
    if (*p != 'M') return NULL;                  /* only the M form is used here */
    p++;
    r->m = (int)strtol(p, (char **)&p, 10);
    if (*p++ != '.') return NULL;
    r->w = (int)strtol(p, (char **)&p, 10);
    if (*p++ != '.') return NULL;
    r->d = (int)strtol(p, (char **)&p, 10);
    r->at = 7200;
    if (*p == '/') { p++; if (!(p = rd_off(p, &r->at))) return NULL; }
    if (r->m < 1 || r->m > 12 || r->w < 1 || r->w > 5 || r->d < 0 || r->d > 6) return NULL;
    return p;
}

typedef struct {
    char std[12], dst[12];
    long std_w, dst_w;           /* seconds west */
    bool has_dst;
    rule_t on, off;
} tzspec_t;

static bool parse(const char *s, tzspec_t *t)
{
    memset(t, 0, sizeof(*t));
    if (!s) return false;
    const char *p = rd_name(s, t->std, sizeof(t->std));
    if (!p || !(p = rd_off(p, &t->std_w))) return false;
    if (!*p) return true;
    if (!(p = rd_name(p, t->dst, sizeof(t->dst)))) return false;
    t->dst_w = t->std_w - 3600;
    if (*p && *p != ',') { if (!(p = rd_off(p, &t->dst_w))) return false; }
    if (*p != ',') return false;
    if (!(p = rd_rule(p + 1, &t->on)) || *p != ',' || !(p = rd_rule(p + 1, &t->off)) || *p) return false;
    t->has_dst = true;
    return true;
}

/* Days since 1970-01-01, Howard Hinnant's days_from_civil. */
static long days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const long yoe = y - era * 400;
    const long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static int month_days(int y, int m)
{
    static const int D[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return D[m - 1] + (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0));
}

/* The UTC instant of a rule in year y, given the offset in force before it. */
static long long rule_utc(int y, const rule_t *r, long west_before)
{
    const long first = days_from_civil(y, r->m, 1);
    const int dow1 = (int)(((first % 7) + 11) % 7);          /* 1970-01-01 was a Thursday */
    int day = 1 + (r->d - dow1 + 7) % 7 + (r->w - 1) * 7;
    while (day > month_days(y, r->m)) day -= 7;
    return (long long)(first + day - 1) * 86400 + r->at + west_before;
}

static int year_of(long long utc)
{
    const long z0 = (long)(utc >= 0 ? utc / 86400 : (utc - 86399) / 86400) + 719468;
    const long era = (z0 >= 0 ? z0 : z0 - 146096) / 146097;
    const long doe = z0 - era * 146097;
    const long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const long mp = (5 * doy + 2) / 153;
    return (int)(yoe + era * 400 + (mp >= 10 ? 1 : 0));
}

bool ls_tz_offset_at(const char *posix, time_t utc, int *east_s, bool *dst)
{
    tzspec_t t;
    if (!parse(posix, &t)) return false;
    bool in = false;
    if (t.has_dst) {
        const int y = year_of((long long)utc);
        const long long on = rule_utc(y, &t.on, t.std_w), off = rule_utc(y, &t.off, t.dst_w);
        const long long u = (long long)utc;
        in = on < off ? (u >= on && u < off) : (u >= on || u < off);
    }
    if (east_s) *east_s = (int)-(in ? t.dst_w : t.std_w);
    if (dst) *dst = in;
    return true;
}

void ls_tz_abbr_of(const char *posix, bool dst, char *out, size_t n)
{
    tzspec_t t;
    if (!out || !n) return;
    if (!parse(posix, &t)) { snprintf(out, n, "UTC"); return; }
    /* the fallback zones are named UTC with an offset: say which */
    if (!strcmp(t.std, "UTC") && t.std_w) {
        const long e = -t.std_w;
        snprintf(out, n, "UTC%+ld", e / 3600);
        return;
    }
    snprintf(out, n, "%s", dst && t.has_dst ? t.dst : t.std);
}

size_t ls_tz_clock_text(char *out, size_t n, int hour, int minute, bool h24)
{
    if (!out || !n) return 0;
    int k;
    if (h24) k = snprintf(out, n, "%02d:%02d", hour, minute);
    else {
        const int h = hour % 12 ? hour % 12 : 12;
        k = snprintf(out, n, "%d:%02d%c", h, minute, hour < 12 ? 'a' : 'p');
    }
    return k < 0 ? 0 : (size_t)k >= n ? n - 1 : (size_t)k;
}

/* ------------------------------------------------------------- the board's zone */

static int  s_choice = LS_TZ_AUTO;          /* loaded once, kept here */
static bool s_loaded;
static bool s_24h;
static char s_posix[40] = "UTC0";
static char s_label[24] = "UTC";
static bool s_auto_known;                   /* AUTO has had a position */
static bool s_applied;                      /* ls_tz_apply has run once */

#if !LS_TIME_HOST
#include "esp_timer.h"
#include "settings.h"
#include "ls_location.h"
#include "ls_rtc.h"
#include "esp_log.h"
static const char *TAG = "ls_tz";
static void load(void)
{
    if (s_loaded) return;
    s_loaded = true;
    s_choice = settings_get_tz_zone();
    if (s_choice < 0 || s_choice >= Z_COUNT) s_choice = LS_TZ_AUTO;
    s_24h = settings_get_clock_24h();
}
static bool here(double *lat, double *lon) { return ls_location_here(lat, lon) != LS_LOC_SRC_NONE; }
static void store_choice(int i) { settings_set_tz_zone(i); }
static void store_24h(bool on) { settings_set_clock_24h(on); }
#else
/* The bench: no settings store and no receiver; a test or the simulator
   sets the zone, and LSSIM_TZ_POS="lat,lon" stands in for a fix. */
static void load(void) { s_loaded = true; }
static bool here(double *lat, double *lon)
{
    const char *e = getenv("LSSIM_TZ_POS");
    return e && sscanf(e, "%lf,%lf", lat, lon) == 2;
}
static void store_choice(int i) { (void)i; }
static void store_24h(bool on) { (void)on; }
#endif

int ls_tz_choice(void) { load(); return s_choice; }
bool ls_tz_24h(void) { load(); return s_24h; }

void ls_tz_set_choice(int i)
{
    load();
    if (i < 0 || i >= Z_COUNT) i = LS_TZ_AUTO;
    s_choice = i;
    store_choice(i);
    ls_tz_apply();
}

void ls_tz_set_24h(bool on) { load(); s_24h = on; store_24h(on); }

const char *ls_tz_current_posix(void) { return s_posix; }
const char *ls_tz_current_label(void) { return s_label; }
bool ls_tz_is_utc(void) { return !strcmp(s_posix, "UTC0"); }

void ls_tz_show(char *out, size_t n)
{
    if (!s_applied) ls_tz_apply();
    if (s_choice != LS_TZ_AUTO) snprintf(out, n, "%s", ZONES[s_choice].name);
    else if (!s_auto_known) snprintf(out, n, "AUTO: UTC, no fix yet");
    else snprintf(out, n, "AUTO: %s", s_label);
}

void ls_tz_apply(void)
{
    load();
    s_applied = true;
    char posix[40], label[24];
    if (s_choice != LS_TZ_AUTO) {
        snprintf(posix, sizeof(posix), "%s", ZONES[s_choice].posix);
        snprintf(label, sizeof(label), "%s", ZONES[s_choice].name);
    } else {
        double lat, lon;
        if (here(&lat, &lon)) {
            ls_tz_pick(lat, lon, posix, sizeof(posix), label, sizeof(label));
            s_auto_known = true;
        } else if (s_auto_known) {
            return;                         /* keep the zone the last position gave */
        } else {
            snprintf(posix, sizeof(posix), "UTC0");
            snprintf(label, sizeof(label), "UTC");
        }
    }
    snprintf(s_label, sizeof(s_label), "%s", label);
    if (!strcmp(posix, s_posix)) return;
    snprintf(s_posix, sizeof(s_posix), "%s", posix);
#if !LS_TIME_HOST
    setenv("TZ", s_posix, 1);
    tzset();
    ESP_LOGI(TAG, "time zone %s (%s)", s_label, s_posix);
#endif
}

bool ls_tz_local(time_t t, struct tm *out)
{
    if (!out) return false;
#if !LS_TIME_HOST
    return localtime_r(&t, out) != NULL;
#else
    int east = 0;
    bool dst = false;
    ls_tz_offset_at(s_posix, t, &east, &dst);
    const time_t l = t + east;
#if defined(_WIN32)
    if (gmtime_s(out, &l)) return false;
#else
    if (!gmtime_r(&l, out)) return false;
#endif
    out->tm_isdst = dst;
    return true;
#endif
}

void ls_tz_abbr(const struct tm *local, char *out, size_t n)
{
    ls_tz_abbr_of(s_posix, local && local->tm_isdst > 0, out, n);
}

void ls_tz_status_clock(char *out, size_t n)
{
    if (!out || !n) return;
    struct tm tm;
    if (!ls_time_is_synced() || !ls_tz_local(time(NULL), &tm)) { snprintf(out, n, "--:--"); return; }
    ls_tz_clock_text(out, n, tm.tm_hour, tm.tm_min, ls_tz_24h());
}

void ls_tz_tick(void)
{
#if !LS_TIME_HOST
    static int64_t next_us, rtc_us;
    static bool first = true;
    const int64_t now = esp_timer_get_time();
    if (first || now >= next_us) {
        first = false;
        next_us = now + 30LL * 1000000;
        ls_tz_apply();
    }
    /* A clock from the network or the GPS goes back into the RTC: it was
       only ever read, so after a reboot with neither the board had no time,
       or the oscillator-stopped RTC's none. Again every six hours. */
    const int src = ls_time_source();
    if (src >= 2 && (!rtc_us || now - rtc_us > 6LL * 3600 * 1000000)) {
        rtc_us = now;
        const esp_err_t e = ls_rtc_set(time(NULL));
        if (e == ESP_OK) ESP_LOGI(TAG, "RTC set from %s", src == 2 ? "network time" : "GPS");
    }
#else
    if (!s_applied) ls_tz_apply();
#endif
}
