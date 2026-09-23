/* LS_TEST_SOURCES: ${FW}/components/lakeshark/core/scan_import.c ${FW}/components/lakeshark/core/scan_geo.c */
/* The combined Franklin-to-Provincetown list, put through the real importer.

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

#ifndef SCAN_LIST_ALL_PATH
#error "SCAN_LIST_ALL_PATH must name the channel list"
#endif

/* Mirrors import_worker() in scan_import_file.c, which is the code that
   actually runs on the device: header line, '#' comments, blank lines, and
   a rejection of duplicate (freq, mode, zone) triples. That file cannot be
   linked on the host - it is all ESP heap and NVS - so the rules it enforces
   are restated here, and any drift between the two shows up as this test
   passing while the device refuses the file. */
static int load(scan_channel_t *out, int max, char *why, size_t why_len)
{
    FILE *f = fopen(SCAN_LIST_ALL_PATH, "rb");
    if (!f) {
        snprintf(why, why_len, "cannot open %s", SCAN_LIST_ALL_PATH);
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


LS_CASE(the_combined_list_imports)
{
    char why[192] = "";
    const int n = rows(why, sizeof(why));
    if (n <= 0) return;
    LS_CHECK_MSG(n >= 50, "only %d channels in the combined list", n);
}

LS_CASE(it_holds_both_halves_of_the_drive)
{
    /* The whole point of the combined file. Loading either regional list
       REPLACES the channel list, which is how Franklin PD went missing after
       the Cape list was loaded. Both ends must be in this one file. */
    char why[192] = "";
    const int n = rows(why, sizeof(why));
    if (n <= 0) return;

    static const char *const MUST[] = {
        "FRANKLIN PD", "LRMFA F-1 DISP", "NHSP STATE",
        "YARMOUTH FIRE", "PTOWN FIRE", "CCNS P25 1",
    };
    for (unsigned m = 0; m < sizeof(MUST) / sizeof(MUST[0]); ++m) {
        int found = 0;
        for (int i = 0; i < n; ++i) if (!strcmp(g_rows[i].name, MUST[m])) found = 1;
        LS_CHECK_MSG(found, "%s is not in the combined list", MUST[m]);
    }
}

LS_CASE(franklin_pd_is_the_decoded_one_and_is_p25)
{
    /* 154.7850 is the only frequency in either list the board has actually
       decoded, so a typo here is worth catching by value and not by name. */
    char why[192] = "";
    const int n = rows(why, sizeof(why));
    if (n <= 0) return;
    int i = -1;
    for (int k = 0; k < n; ++k) if (!strcmp(g_rows[k].name, "FRANKLIN PD")) i = k;
    LS_CHECK_MSG(i >= 0, "FRANKLIN PD missing");
    if (i < 0) return;
    LS_EQ_INT(154785000, (int)g_rows[i].freq_hz);
    LS_EQ_INT(SCAN_MODE_P25, (int)g_rows[i].mode);
}

LS_CASE(the_p25_screen_has_more_than_a_handful)
{
    /* ls_radio_panel filters the visible list by the screen's own mode, so a
       list whose P25 side is nearly empty shows a nearly empty P25 screen.
       That is what two entries looked like. */
    char why[192] = "";
    const int n = rows(why, sizeof(why));
    if (n <= 0) return;
    int p25 = 0, nfm = 0;
    for (int i = 0; i < n; ++i) {
        if (g_rows[i].mode == SCAN_MODE_P25) p25++; else nfm++;
    }
    LS_CHECK_MSG(p25 >= 10, "only %d P25 channels; the P25 screen would look empty", p25);
    LS_CHECK_MSG(nfm >= 10, "only %d NFM channels", nfm);
}

LS_CASE(every_channel_is_inside_the_receiver_span)
{
    char why[192] = "";
    const int n = rows(why, sizeof(why));
    if (n <= 0) return;
    for (int i = 0; i < n; ++i)
        LS_CHECK_MSG(g_rows[i].freq_hz >= 24000000 && g_rows[i].freq_hz <= 1766000000,
                     "%s at %llu Hz is outside the tuner",
                     g_rows[i].name, (unsigned long long)g_rows[i].freq_hz);
}

LS_CASE(driving_franklin_to_provincetown_admits_channels_the_whole_way)
{
    char why[192] = "";
    const int n = rows(why, sizeof(why));
    if (n <= 0) return;

    static const struct { const char *where; double lat, lon; } LEG[] = {
        { "Franklin NH",    43.4406, -71.6498 },
        { "Concord NH",     43.2081, -71.5376 },
        { "Manchester NH",  42.9956, -71.4548 },
        { "Nashua NH",      42.7654, -71.4676 },
        { "Bridgewater MA", 41.9900, -70.9750 },
        { "Bourne MA",      41.7440, -70.6160 },
        { "Yarmouth MA",    41.6690, -70.2028 },
        { "Brewster MA",    41.7601, -70.0819 },
        { "Eastham MA",     41.8298, -69.9740 },
        { "Truro MA",       41.9954, -70.0492 },
        { "Provincetown MA",42.0587, -70.1787 },
    };

    scan_geo_t geo;
    memset(&geo, 0, sizeof(geo));
    int64_t now = 1000000;
    for (unsigned leg = 0; leg < sizeof(LEG) / sizeof(LEG[0]); ++leg) {
        now += 600000000LL;
        scan_geo_update(&geo, g_rows, n, true, LEG[leg].lat, LEG[leg].lon, now, now);
        int admitted = 0;
        for (int i = 0; i < n; ++i) if (scan_geo_admits(&geo, i, now)) admitted++;
        LS_CHECK_MSG(admitted > 0,
                     "nothing is in range at %s - that leg scans silence", LEG[leg].where);
    }
}

LS_CASE(both_ends_have_a_local_channel)
{
    char why[192] = "";
    const int n = rows(why, sizeof(why));
    if (n <= 0) return;
    static const struct { const char *where; double lat, lon; } END[] = {
        { "Franklin NH",     43.4406, -71.6498 },
        { "Provincetown MA", 42.0587, -70.1787 },
    };
    for (unsigned e = 0; e < sizeof(END) / sizeof(END[0]); ++e) {
        scan_geo_t geo;
        memset(&geo, 0, sizeof(geo));
        const int64_t now = 2000000;
        scan_geo_update(&geo, g_rows, n, true, END[e].lat, END[e].lon, now, now);
        int local = 0;
        for (int i = 0; i < n; ++i)
            if (scan_geo_admits(&geo, i, now) && g_rows[i].radius_m <= 45000) local++;
        LS_CHECK_MSG(local > 0, "no local channel covers %s", END[e].where);
    }
}

LS_CASE(it_uses_every_zone_the_firmware_allows)
{
    char why[192] = "";
    const int n = rows(why, sizeof(why));
    if (n <= 0) return;
    bool seen[SCAN_MAX_ZONES] = { false };
    for (int i = 0; i < n; ++i) seen[g_rows[i].zone] = true;
    for (int z = 0; z < SCAN_MAX_ZONES; ++z)
        LS_CHECK_MSG(seen[z], "zone %d is empty", z);
}
