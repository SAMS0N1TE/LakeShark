/* LS_TEST_SOURCES: ${FW}/components/lakeshark/core/scan_import.c ${FW}/components/lakeshark/core/scan_geo.c */
/* The hand-authored Cape Cod Bay channel list, put through the real importer.

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

#ifndef SCAN_LIST_CAPE_PATH
#error "SCAN_LIST_CAPE_PATH must name the channel list"
#endif

/* Mirrors import_worker() in scan_import_file.c, which is the code that
   actually runs on the device: header line, '#' comments, blank lines, and
   a rejection of duplicate (freq, mode, zone) triples. That file cannot be
   linked on the host - it is all ESP heap and NVS - so the rules it enforces
   are restated here, and any drift between the two shows up as this test
   passing while the device refuses the file. */
static int load(scan_channel_t *out, int max, char *why, size_t why_len)
{
    FILE *f = fopen(SCAN_LIST_CAPE_PATH, "rb");
    if (!f) {
        snprintf(why, why_len, "cannot open %s", SCAN_LIST_CAPE_PATH);
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


LS_CASE(the_cape_list_imports)
{
    char why[192] = "";
    const int n = rows(why, sizeof(why));
    if (n <= 0) return;
    LS_CHECK_MSG(n >= 20, "only %d channels in the list", n);
}

LS_CASE(every_channel_carries_a_place_and_a_sane_radius)
{
    char why[192] = "";
    const int n = rows(why, sizeof(why));
    if (n <= 0) return;
    for (int i = 0; i < n; ++i) {
        LS_CHECK_MSG(g_rows[i].radius_m >= 10000,
                     "%s has a %u m radius - metres or kilometres?",
                     g_rows[i].name, (unsigned)g_rows[i].radius_m);
        LS_CHECK_MSG(g_rows[i].flags & SCAN_FLAG_ENABLED,
                     "%s imported disabled", g_rows[i].name);
    }
}

LS_CASE(every_channel_is_actually_on_the_cape)
{
    /* A transposed sign or a digit dropped from a coordinate puts a circle
       in the Atlantic, and with LOCATION on the channel simply never scans. */
    char why[192] = "";
    const int n = rows(why, sizeof(why));
    if (n <= 0) return;
    for (int i = 0; i < n; ++i) {
        LS_CHECK_MSG(g_rows[i].lat_e7 > 414000000 && g_rows[i].lat_e7 < 421000000,
                     "%s sits at latitude %.4f", g_rows[i].name,
                     g_rows[i].lat_e7 / 1e7);
        LS_CHECK_MSG(g_rows[i].lon_e7 < -698000000 && g_rows[i].lon_e7 > -707000000,
                     "%s sits at longitude %.4f", g_rows[i].name,
                     g_rows[i].lon_e7 / 1e7);
    }
}

LS_CASE(every_channel_is_inside_the_receiver_span)
{
    /* 33 MHz low band is the reason this one matters: it is only 9 MHz above
       the bottom of the tuner, and a list that asks for something the radio
       cannot reach is a row that never opens squelch. */
    char why[192] = "";
    const int n = rows(why, sizeof(why));
    if (n <= 0) return;
    for (int i = 0; i < n; ++i)
        LS_CHECK_MSG(g_rows[i].freq_hz >= 24000000 && g_rows[i].freq_hz <= 1766000000,
                     "%s at %llu Hz is outside the tuner",
                     g_rows[i].name, (unsigned long long)g_rows[i].freq_hz);
}

LS_CASE(driving_yarmouth_to_provincetown_admits_channels_the_whole_way)
{
    char why[192] = "";
    const int n = rows(why, sizeof(why));
    if (n <= 0) return;

    static const struct { const char *where; double lat, lon; } LEG[] = {
        { "Yarmouth",     41.6690, -70.2028 },
        { "Dennis",       41.7353, -70.1936 },
        { "Brewster",     41.7601, -70.0819 },
        { "Orleans",      41.7898, -69.9895 },
        { "Eastham",      41.8298, -69.9740 },
        { "Wellfleet",    41.9376, -70.0328 },
        { "Truro",        41.9954, -70.0492 },
        { "Provincetown", 42.0587, -70.1787 },
    };

    scan_geo_t geo;
    memset(&geo, 0, sizeof(geo));
    int64_t now = 1000000;

    for (unsigned leg = 0; leg < sizeof(LEG) / sizeof(LEG[0]); ++leg) {
        now += 600000000LL;
        scan_geo_update(&geo, g_rows, n, true, LEG[leg].lat, LEG[leg].lon,
                        now, now);
        int admitted = 0;
        for (int i = 0; i < n; ++i)
            if (scan_geo_admits(&geo, i, now)) admitted++;
        LS_CHECK_MSG(admitted > 0,
                     "nothing is in range at %s - that leg scans silence",
                     LEG[leg].where);
    }
}

LS_CASE(both_ends_have_a_local_channel_not_just_countywide)
{
    /* Yarmouth is where the board is tonight and Provincetown is where it is
       going. County-wide circles alone would pass the previous case while the
       list held nothing about either place. */
    char why[192] = "";
    const int n = rows(why, sizeof(why));
    if (n <= 0) return;

    static const struct { const char *where; double lat, lon; } END[] = {
        { "Yarmouth",     41.6690, -70.2028 },
        { "Provincetown", 42.0587, -70.1787 },
    };

    for (unsigned e = 0; e < sizeof(END) / sizeof(END[0]); ++e) {
        scan_geo_t geo;
        memset(&geo, 0, sizeof(geo));
        const int64_t now = 2000000;
        scan_geo_update(&geo, g_rows, n, true, END[e].lat, END[e].lon, now, now);
        int local = 0;
        for (int i = 0; i < n; ++i)
            if (scan_geo_admits(&geo, i, now) && g_rows[i].radius_m <= 45000)
                local++;
        LS_CHECK_MSG(local > 0, "no local channel covers %s", END[e].where);
    }
}

LS_CASE(every_town_on_the_route_has_its_fire_dispatch)
{
    /* Town fire alarm on low band is the one thing a conventional scanner
       gets the whole way out, because the police are talkgroups on a trunk.
       If a town is missing here the list has quietly lost its main point. */
    char why[192] = "";
    const int n = rows(why, sizeof(why));
    if (n <= 0) return;

    static const char *const TOWN[] = {
        "YARMOUTH FIRE", "DENNIS FIRE", "BREWSTER FIRE", "HARWICH FIRE",
        "CHATHAM FIRE", "ORLEANS FIRE", "EASTHAM FIRE", "WELLFLEET FIRE",
        "TRURO FIRE", "PTOWN FIRE",
    };

    for (unsigned t = 0; t < sizeof(TOWN) / sizeof(TOWN[0]); ++t) {
        int found = -1;
        for (int i = 0; i < n; ++i)
            if (!strcmp(g_rows[i].name, TOWN[t])) { found = i; break; }
        LS_CHECK_MSG(found >= 0, "%s is not in the list", TOWN[t]);
        if (found < 0) continue;
        LS_CHECK_MSG(g_rows[found].freq_hz >= 33000000 &&
                     g_rows[found].freq_hz < 34000000,
                     "%s is at %llu Hz, not on low band", TOWN[t],
                     (unsigned long long)g_rows[found].freq_hz);
    }
}

LS_CASE(the_list_spans_every_zone_it_documents)
{
    char why[192] = "";
    const int n = rows(why, sizeof(why));
    if (n <= 0) return;
    bool seen[6] = { false };
    for (int i = 0; i < n; ++i)
        if (g_rows[i].zone < 6) seen[g_rows[i].zone] = true;
    for (int z = 0; z < 6; ++z)
        LS_CHECK_MSG(seen[z], "zone %d is documented in the header but empty", z);
}
