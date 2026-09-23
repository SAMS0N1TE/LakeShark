/* LS_TEST_SOURCES: ${FW}/components/lakeshark/core/scan_import.c ${FW}/components/lakeshark/core/scan_geo.c */
/* The hand-authored NH/MA channel list, put through the real importer.

   Same argument as the route profile test: a scan list is only worth having
   if it loads, and the cheapest place to find out that it does not is here
   rather than at the roadside. The device's importer is all-or-nothing - one
   bad row and 'ch load' rejects the file and keeps the old list - so a typo
   in row forty costs the whole list, silently, at the moment it is wanted.

   This reads the actual file from disk, runs each line through the firmware's
   own scan_import_line(), and then drives the route through the firmware's
   own geo filter. "The file loads" and "the file does what it is for" are two
   different assertions, and the second one is the interesting one: a channel
   whose circle nothing on the drive falls inside would simply never be
   scanned with LOCATION on, and nothing would say so. */

#include "ls_test.h"
#include "scan_import.h"
#include "scan_geo.h"

#include <stdio.h>
#include <string.h>

#ifndef SCAN_LIST_NH_MA_PATH
#error "SCAN_LIST_NH_MA_PATH must name the channel list"
#endif

/* Mirrors import_worker() in scan_import_file.c, which is the code that
   actually runs on the device: header line, '#' comments, blank lines, and
   a rejection of duplicate (freq, mode, zone) triples. That file cannot be
   linked on the host - it is all ESP heap and NVS - so the rules it enforces
   are restated here, and any drift between the two shows up as this test
   passing while the device refuses the file. */
static int load(scan_channel_t *out, int max, char *why, size_t why_len)
{
    FILE *f = fopen(SCAN_LIST_NH_MA_PATH, "rb");
    if (!f) {
        snprintf(why, why_len, "cannot open %s", SCAN_LIST_NH_MA_PATH);
        return -1;
    }

    char line[192];
    int count = 0, number = 0;
    while (fgets(line, sizeof(line), f)) {
        ++number;
        if (number == 1) {
            if (strcmp(line, "LSCAN1\n") && strcmp(line, "LSCAN1\r\n")) {
                snprintf(why, why_len, "line 1 is not the LSCAN1 header");
                fclose(f);
                return -1;
            }
            continue;
        }
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;

        /* A line that filled the buffer without a newline is one the device
           would reject outright, and it is worth saying so by name rather
           than as a parse failure. */
        if (!strchr(line, '\n') && !feof(f)) {
            snprintf(why, why_len, "line %d is longer than the 192 byte limit",
                     number);
            fclose(f);
            return -1;
        }
        if (count == max) {
            snprintf(why, why_len, "more than %d channels", max);
            fclose(f);
            return -1;
        }
        if (!scan_import_line(line, &out[count])) {
            line[strcspn(line, "\r\n")] = 0;
            snprintf(why, why_len, "line %d rejected: %s", number, line);
            fclose(f);
            return -1;
        }
        for (int i = 0; i < count; ++i) {
            if (out[i].freq_hz == out[count].freq_hz &&
                out[i].mode == out[count].mode &&
                out[i].zone == out[count].zone) {
                snprintf(why, why_len,
                         "line %d duplicates %s - same frequency, mode and zone",
                         number, out[i].name);
                fclose(f);
                return -1;
            }
        }
        ++count;
    }
    const bool bad = ferror(f) != 0;
    fclose(f);
    if (bad) { snprintf(why, why_len, "read error"); return -1; }
    return count;
}

#define MAX_ROWS 256
static scan_channel_t g_rows[MAX_ROWS];

static int rows(char *why, size_t why_len)
{
    const int n = load(g_rows, MAX_ROWS, why, why_len);
    LS_CHECK_MSG(n > 0, "channel list rejected: %s", why);
    return n;
}

LS_CASE(the_nh_ma_list_imports)
{
    char why[192] = "";
    const int n = rows(why, sizeof(why));
    if (n <= 0) return;

    /* Low bar deliberately: this is checking the file is not a stub, not
       auditing how many channels it ought to have. */
    LS_CHECK_MSG(n >= 20, "only %d channels in the list", n);
}

LS_CASE(every_channel_carries_a_place_and_a_sane_radius)
{
    /* radius_m == 0 means scan_geo_update() never marks the channel
       eligible, so with LOCATION on it is dead - present in the list, shown
       on screen, and never scanned. A row that forgot its circle is almost
       certainly an edit, not a decision. */
    char why[192] = "";
    const int n = rows(why, sizeof(why));
    if (n <= 0) return;

    for (int i = 0; i < n; ++i) {
        LS_CHECK_MSG(g_rows[i].radius_m > 0,
                     "%s has no radius - it would never scan with LOCATION on",
                     g_rows[i].name);
        LS_CHECK_MSG(g_rows[i].radius_m >= 10000,
                     "%s has a %u m radius - metres or kilometres?",
                     g_rows[i].name, (unsigned)g_rows[i].radius_m);
        LS_CHECK_MSG(g_rows[i].flags & SCAN_FLAG_ENABLED,
                     "%s imported disabled", g_rows[i].name);
    }
}

LS_CASE(every_channel_is_actually_in_new_england)
{
    /* A sign slip on a longitude puts a New Hampshire channel in Asia, and
       the geo filter would then simply never admit it - silently, for the
       whole drive. The box is generous; it is catching a typo, not
       surveying. */
    char why[192] = "";
    const int n = rows(why, sizeof(why));
    if (n <= 0) return;

    for (int i = 0; i < n; ++i) {
        const double lat = g_rows[i].lat_e7 * 1e-7;
        const double lon = g_rows[i].lon_e7 * 1e-7;
        LS_CHECK_MSG(lat > 41.0 && lat < 45.0,
                     "%s latitude %.4f is not in NH or MA", g_rows[i].name, lat);
        LS_CHECK_MSG(lon > -72.5 && lon < -69.5,
                     "%s longitude %.4f is not in NH or MA", g_rows[i].name, lon);
    }
}

LS_CASE(every_channel_is_inside_the_receiver_span)
{
    /* The device's own tuner range. A frequency outside it is a channel the
       scanner will stop on and hear nothing from, forever. */
    char why[192] = "";
    const int n = rows(why, sizeof(why));
    if (n <= 0) return;

    for (int i = 0; i < n; ++i) {
        LS_CHECK_MSG(g_rows[i].freq_hz >= 24000000u &&
                     g_rows[i].freq_hz <= 1766000000u,
                     "%s is %.4f MHz, outside the 24-1766 MHz tuner span",
                     g_rows[i].name, g_rows[i].freq_hz * 1e-6);
    }
}

LS_CASE(driving_the_route_admits_channels_the_whole_way)
{
    /* The point of the file. Positions down I-93 / I-495 / Route 25, in the
       order they are driven. At every one of them the geo filter should
       admit something - a leg of the drive where the list goes completely
       dead is a hole in the coverage, and the only way to find it short of
       driving it is here. */
    char why[192] = "";
    const int n = rows(why, sizeof(why));
    if (n <= 0) return;

    static const struct { const char *where; double lat, lon; } LEG[] = {
        { "Franklin NH",    43.4406, -71.6498 },
        { "Concord NH",     43.2081, -71.5376 },
        { "Manchester NH",  42.9956, -71.4548 },
        { "Nashua NH",      42.7654, -71.4676 },
        { "Bedford MA",     42.4700, -71.2890 },
        { "Framingham MA",  42.2790, -71.4160 },
        { "Norfolk MA",     42.1180, -71.3250 },
        { "Bridgewater MA", 41.9900, -70.9750 },
        { "Bourne MA",      41.7440, -70.6160 },
        { "West Dennis MA", 41.6620, -70.1700 },
    };

    scan_geo_t geo;
    memset(&geo, 0, sizeof(geo));
    int64_t now = 1000000;

    for (unsigned leg = 0; leg < sizeof(LEG) / sizeof(LEG[0]); ++leg) {
        now += 600000000LL;             /* ten minutes of driving */
        scan_geo_update(&geo, g_rows, n, true, LEG[leg].lat, LEG[leg].lon,
                        now, now);

        int admitted = 0;
        for (int i = 0; i < n; ++i)
            if (scan_geo_admits(&geo, i, now)) admitted++;

        LS_CHECK_MSG(admitted > 0,
                     "nothing in the list is in range at %s - that leg of the "
                     "drive scans silence", LEG[leg].where);
    }
}

LS_CASE(the_home_and_destination_ends_both_have_local_coverage)
{
    /* Route-wide channels alone would satisfy the previous case while the
       list held nothing local at either end. Franklin is where the board
       lives and West Dennis is where it is going; both should admit a
       channel whose own circle is small enough to be about that place
       rather than about the whole drive. */
    char why[192] = "";
    const int n = rows(why, sizeof(why));
    if (n <= 0) return;

    static const struct { const char *where; double lat, lon; } END[] = {
        { "Franklin NH",    43.4406, -71.6498 },
        { "West Dennis MA", 41.6620, -70.1700 },
    };

    for (unsigned e = 0; e < sizeof(END) / sizeof(END[0]); ++e) {
        scan_geo_t geo;
        memset(&geo, 0, sizeof(geo));
        const int64_t now = 2000000;
        scan_geo_update(&geo, g_rows, n, true, END[e].lat, END[e].lon,
                        now, now);

        int local = 0;
        for (int i = 0; i < n; ++i)
            if (scan_geo_admits(&geo, i, now) && g_rows[i].radius_m <= 60000)
                local++;

        LS_CHECK_MSG(local > 0, "no local channel covers %s", END[e].where);
    }
}

LS_CASE(priority_is_reserved_for_dispatch)
{
    /* Priority channels are checked between every other channel, so marking
       everything priority is the same as marking nothing. Keeping the count
       low is the whole mechanism. */
    char why[192] = "";
    const int n = rows(why, sizeof(why));
    if (n <= 0) return;

    int priority = 0;
    for (int i = 0; i < n; ++i)
        if (g_rows[i].flags & SCAN_FLAG_PRIORITY) priority++;

    LS_CHECK_MSG(priority > 0, "no priority channels at all");
    LS_CHECK_MSG(priority * 4 <= n,
                 "%d of %d channels are priority - that is not a priority list",
                 priority, n);
}

LS_CASE(the_list_spans_more_than_one_zone)
{
    /* The zones are how the list is switched between regions on the device.
       A list that landed everything in zone 0 would still import and still
       scan, and the zone selector would do nothing. */
    char why[192] = "";
    const int n = rows(why, sizeof(why));
    if (n <= 0) return;

    bool seen[SCAN_MAX_ZONES];
    memset(seen, 0, sizeof(seen));
    for (int i = 0; i < n; ++i) {
        LS_CHECK_MSG(g_rows[i].zone < SCAN_MAX_ZONES,
                     "%s is in zone %u", g_rows[i].name,
                     (unsigned)g_rows[i].zone);
        if (g_rows[i].zone < SCAN_MAX_ZONES) seen[g_rows[i].zone] = true;
    }

    int used = 0;
    for (int z = 0; z < SCAN_MAX_ZONES; ++z) if (seen[z]) used++;
    LS_CHECK_MSG(used >= 4, "only %d zone(s) used", used);
}
