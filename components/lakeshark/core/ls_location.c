/* See ls_location.h. One small task on a PSRAM stack reads the receiver every
   2 s; everything apps call is a RAM read. NVS goes through the settings
   worker (settings_set_last_fix), never from here directly. */
#include "ls_location.h"
#include "ls_board.h"
#include "ls_gps.h"
#include "settings.h"
#include "ls_time.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#if defined(LS_BOARD_GPS_RX_GPIO) && defined(LS_BOARD_GPS_UART_NUM)
#define HAS_GPS 1
#else
#define HAS_GPS 0
#endif

#define TICK_MS      2000
#define SAVE_KM      0.5f                   /* a last fix this far from the saved one... */
#define SAVE_US      (600 * 1000000LL)      /* ...is written at most this often */
#define REGION_KM    5.0f

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static ls_location_t s;                     /* state and region are filled on read / by the task */

/* ------------------------------------------------------------- regions

   Tenths of a degree: west, south, east, north. parent 1 US, 2 CA, 3 AU (the
   code is then the state); 0 means the code is the country itself. A region
   may have a second, smaller box where a neighbour's box would win wrongly. */
typedef struct { char code[3]; uint8_t parent; int16_t w, s, e, n; } box_t;
static const box_t BOX[] = {
    {"AL",1,-885,302,-849,350},{"AK",1,-1799,512,-1299,715},{"AZ",1,-1148,313,-1090,370},{"AR",1,-946,330,-896,365},
    {"CA",1,-1245,325,-1141,420},{"CO",1,-1091,370,-1020,410},{"CT",1,-737,410,-718,421},{"DE",1,-758,384,-750,398},
    {"DC",1,-771,388,-769,390},{"FL",1,-876,244,-800,310},{"GA",1,-856,304,-808,350},{"HI",1,-1603,189,-1548,222},
    {"ID",1,-1172,420,-1110,490},{"IL",1,-915,370,-875,425},{"IN",1,-881,378,-848,418},{"IA",1,-966,404,-901,435},
    {"KS",1,-1021,370,-946,400},{"KY",1,-896,365,-819,391},{"LA",1,-940,289,-888,330},{"ME",1,-711,430,-669,475},
    {"MD",1,-795,379,-750,397},{"MA",1,-735,412,-699,429},{"MI",1,-904,417,-824,483},{"MN",1,-972,435,-895,494},
    {"MS",1,-917,302,-881,350},{"MO",1,-958,360,-891,406},{"MT",1,-1161,444,-1040,490},{"NE",1,-1041,400,-953,430},
    {"NV",1,-1200,350,-1140,420},{"NH",1,-726,427,-706,453},{"NJ",1,-756,389,-739,414},{"NM",1,-1091,313,-1030,370},
    {"NY",1,-798,405,-718,450},{"NC",1,-843,338,-754,366},{"ND",1,-1041,459,-966,490},{"OH",1,-848,384,-805,420},
    {"OK",1,-1030,336,-944,370},{"OR",1,-1246,420,-1165,463},{"PA",1,-805,397,-747,423},{"RI",1,-719,411,-711,420},
    {"SC",1,-834,320,-785,352},{"SD",1,-1041,425,-964,459},{"TN",1,-903,350,-816,367},{"TX",1,-1066,258,-935,365},
    {"UT",1,-1141,370,-1090,420},{"VT",1,-734,427,-715,450},{"VA",1,-837,365,-752,395},{"WA",1,-1248,455,-1169,490},
    {"WV",1,-826,372,-777,406},{"WI",1,-929,425,-868,471},{"WY",1,-1111,410,-1041,450},{"PR",1,-673,179,-652,185},
    {"BC",2,-1391,483,-1140,600},{"BC",2,-1238,483,-1232,490},{"AB",2,-1200,490,-1100,600},{"SK",2,-1100,490,-1014,600},
    {"MB",2,-1020,490,-890,600},{"ON",2,-952,417,-743,569},{"ON",2,-824,425,-791,450},{"QC",2,-798,450,-571,626},
    {"NB",2,-691,446,-638,481},{"NS",2,-664,434,-597,470},{"PE",2,-644,459,-620,471},{"NL",2,-678,466,-526,604},
    {"YT",2,-1410,600,-1240,696},{"NT",2,-1365,600,-1020,788},{"NU",2,-1207,517,-612,831},
    {"NSW",3,1410,-375,1536,-282},{"VIC",3,1410,-392,1500,-340},{"QLD",3,1380,-292,1536,-100},{"SA",3,1290,-381,1410,-260},
    {"WA",3,1129,-352,1290,-137},{"TAS",3,1438,-437,1485,-395},{"NT",3,1290,-260,1380,-109},{"ACT",3,1487,-359,1494,-351},
    {"MX",0,-1184,145,-867,327},{"AR",0,-736,-551,-536,-218},{"BR",0,-740,-338,-348,53},{"CL",0,-757,-560,-664,-175},
    {"CO",0,-790,-42,-669,125},{"PE",0,-814,-184,-687,0},{"GB",0,-82,499,18,609},{"IE",0,-105,514,-60,554},
    {"FR",0,-52,423,82,511},{"ES",0,-93,360,33,438},{"PT",0,-95,370,-62,422},{"DE",0,59,473,150,551},
    {"IT",0,66,366,185,471},{"NL",0,34,508,72,536},{"BE",0,25,495,64,515},{"CH",0,60,458,105,478},
    {"AT",0,95,464,172,490},{"PL",0,141,490,242,548},{"CZ",0,121,486,189,511},{"SE",0,111,553,242,691},
    {"NO",0,46,580,311,712},{"FI",0,206,598,316,701},{"DK",0,81,546,127,578},{"UA",0,221,444,402,524},
    {"RO",0,203,436,297,483},{"GR",0,194,348,282,417},{"TR",0,260,360,448,421},{"RU",0,273,412,1800,819},
    {"JP",0,1294,310,1458,455},{"KR",0,1261,331,1296,386},{"CN",0,735,182,1348,536},{"IN",0,682,67,974,355},
    {"ID",0,950,-110,1410,59},{"PH",0,1169,46,1266,211},{"TH",0,973,56,1056,205},{"NZ",0,1664,-473,1786,-344},
    {"ZA",0,165,-348,329,-221},{"IL",0,343,295,359,333},{"SA",0,345,164,557,322},{"AE",0,515,226,564,261},
    {"EG",0,247,220,369,317},
};
#define NBOX ((int)(sizeof(BOX) / sizeof(BOX[0])))

static bool in_box(const box_t *b, int x, int y, int m)
{
    return x >= b->w - m && x <= b->e + m && y >= b->s - m && y <= b->n + m;
}

bool ls_location_region(double lat, double lon, char out[8])
{
    const int x = (int)floor(lon * 10), y = (int)floor(lat * 10);
    long least = LONG_MAX;
    const box_t *hit = NULL;
    for (int i = 0; i < NBOX; i++) {
        const long area = (long)(BOX[i].e - BOX[i].w + 1) * (BOX[i].n - BOX[i].s + 1);
        if (area < least && in_box(&BOX[i], x, y, 0)) { least = area; hit = &BOX[i]; }
    }
    out[0] = 0;
    if (!hit) return false;
    static const char PARENT[][4] = { "", "US-", "CA-", "AU-" };
    snprintf(out, 8, "%s%.3s", PARENT[hit->parent], hit->code);
    return true;
}

int ls_location_us_states(double lat, double lon, float margin_deg, char out[][3], int cap)
{
    const int x = (int)floor(lon * 10), y = (int)floor(lat * 10), m = (int)(margin_deg * 10);
    int n = 0;
    for (int i = 0; i < NBOX && BOX[i].parent == 1 && n < cap; i++)
        if (in_box(&BOX[i], x, y, m)) { out[n][0] = BOX[i].code[0]; out[n][1] = BOX[i].code[1]; out[n++][2] = 0; }
    return n;
}

float ls_location_km(double lat1, double lon1, double lat2, double lon2)
{
    const float dy = (float)(lat2 - lat1);
    float dx = (float)(lon2 - lon1);
    if (dx > 180) dx -= 360;
    if (dx < -180) dx += 360;
    dx *= cosf((float)(lat1 + lat2) * 0.00872665f);
    return 111.19f * sqrtf(dx * dx + dy * dy);
}

/* ------------------------------------------------------------- service */

typedef struct { ls_location_moved_fn fn; void *ctx; float km; bool has; double lat, lon; } sub_t;
static sub_t s_subs[LS_LOC_SUBS];

bool ls_location_on_moved(float km, ls_location_moved_fn fn, void *ctx)
{
    for (int i = 0; i < LS_LOC_SUBS; i++)
        if (!s_subs[i].fn) { s_subs[i].ctx = ctx; s_subs[i].km = km; s_subs[i].has = false; s_subs[i].fn = fn; return true; }
    return false;
}

#if HAS_GPS
static void tick(void *arg)
{
    (void)arg;
    static EXT_RAM_BSS_ATTR ls_gps_state_t g;
    float f_lat = 0, f_lon = 0;
    bool saved = settings_get_last_fix(&f_lat, &f_lon), named = false;
    double saved_lat = f_lat, saved_lon = f_lon, named_lat = 0, named_lon = 0;
    int64_t save_us = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
        ls_gps_get(&g);
        /* a fix that is not a place on the globe (a garbled sentence) is no fix */
        const bool fix = ls_gps_running() && g.fix && fabs(g.lat_deg) <= 90 && fabs(g.lon_deg) <= 180;
        portENTER_CRITICAL(&s_mux);
        s.fix = fix;
        if (fix) {
            s.have = true;
            s.lat = g.lat_deg; s.lon = g.lon_deg; s.alt_m = g.alt_m; s.hdop = g.hdop;
            s.quality = g.quality; s.sats = g.sats_used; s.fix_us = g.last_fix_us;
        }
        portEXIT_CRITICAL(&s_mux);
        if (!fix) continue;
        /* The fix's own time sets the clock when nothing better has: a
           board in the field with no Wi-Fi had no clock until it found one. */
        if (g.utc_s)
            ls_time_offer_gps((time_t)g.utc_s, (uint32_t)(esp_timer_get_time() / 1000) - g.utc_ms);
        const double lat = g.lat_deg, lon = g.lon_deg;
        if (!named || ls_location_km(named_lat, named_lon, lat, lon) > REGION_KM) {
            char region[8];
            ls_location_region(lat, lon, region);
            portENTER_CRITICAL(&s_mux);
            memcpy(s.region, region, sizeof(region));
            portEXIT_CRITICAL(&s_mux);
            named = true; named_lat = lat; named_lon = lon;
        }
        const int64_t now = esp_timer_get_time();
        if ((!saved || ls_location_km(saved_lat, saved_lon, lat, lon) > SAVE_KM) && (!save_us || now - save_us > SAVE_US) &&
            settings_set_last_fix((float)lat, (float)lon)) {
            saved = true; saved_lat = lat; saved_lon = lon; save_us = now;
        }
        for (int i = 0; i < LS_LOC_SUBS; i++) {
            sub_t *u = &s_subs[i];
            if (!u->fn || (u->has && ls_location_km(u->lat, u->lon, lat, lon) < u->km)) continue;
            u->has = true; u->lat = lat; u->lon = lon;
            u->fn(lat, lon, u->ctx);
        }
    }
}
#endif

void ls_location_init(void)
{
    static bool done;
    if (done) return;
    done = true;
    float lat, lon;
    if (settings_get_last_fix(&lat, &lon)) {
        s.have = true; s.lat = lat; s.lon = lon;
        ls_location_region(lat, lon, s.region);
    }
#if HAS_GPS
    if (settings_get_gps_on()) (void)ls_gps_start();
    xTaskCreatePinnedToCoreWithCaps(tick, "ls_loc", 4096, NULL, 2, NULL, tskNO_AFFINITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
}

void ls_location_get(ls_location_t *out)
{
    if (!out) return;
    const bool running = ls_gps_running();
    portENTER_CRITICAL(&s_mux);
    *out = s;
    portEXIT_CRITICAL(&s_mux);
    out->fix = out->fix && running;
    out->state = !HAS_GPS ? LS_LOC_NO_GPS : !running ? LS_LOC_OFF : out->fix ? LS_LOC_FIX : LS_LOC_NO_FIX;
    out->dim = !out->fix ? 0 : out->sats >= 4 ? 3 : 2;
}

ls_loc_src_t ls_location_here(double *lat, double *lon)
{
    ls_location_t l;
    ls_location_get(&l);
    float h_lat, h_lon;
    if (l.have) { *lat = l.lat; *lon = l.lon; return l.fix ? LS_LOC_SRC_LIVE : LS_LOC_SRC_LAST; }
    if (settings_get_home(&h_lat, &h_lon)) { *lat = h_lat; *lon = h_lon; return LS_LOC_SRC_HOME; }
    return LS_LOC_SRC_NONE;
}

bool ls_location_enabled(void) { return HAS_GPS && settings_get_gps_on(); }

void ls_location_set_enabled(bool on)
{
    settings_set_gps_on(on);
    if (on) (void)ls_gps_start();
    else ls_gps_stop();
}

void ls_location_want(void)
{
    if (HAS_GPS && !ls_gps_running() && settings_get_gps_on()) (void)ls_gps_start();
}

ls_loc_state_t ls_location_key(char *out, size_t cap)
{
    ls_location_t l;
    ls_location_get(&l);
    static const char *const T[] = { "NO GPS", "GPS OFF", "NO FIX" };
    if (l.state == LS_LOC_FIX) snprintf(out, cap, "GPS %dD %u sats", l.dim, l.sats);
    else snprintf(out, cap, "%s", T[l.state]);
    return l.state;
}

void ls_location_key_tap(void)
{
    if (HAS_GPS && !ls_gps_running()) ls_location_set_enabled(true);
}
