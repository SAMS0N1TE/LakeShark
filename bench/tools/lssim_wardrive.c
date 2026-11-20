/* lssim: WARDRIVE's feed (components/apps/tui/wardrive/wd_feed.h) as a
   drive. A loop of streets around the map fixture's town at 47 km/h, access
   points and Bluetooth devices placed beside it, a few LTE cells and one
   Remote ID drone over a field; each is heard when the car is near enough,
   at a level that falls with distance. lssim_wardrive_seed() drives five
   minutes of it before the first frame, so a picture shows a drive under
   way. LSSIM_WD=idle leaves the session unstarted; LSSIM_GPS=nofix keeps the
   fix away. Deterministic: the same picture every run. */
#include "wardrive/wd_feed.h"
#include "esp_timer.h"
#ifdef _WIN32
#include <direct.h>
#define WD_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define WD_MKDIR(p) mkdir(p, 0777)
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LAT0 43.4445
#define LON0 -71.6473
#define SPEED 13.0                  /* m/s */
#define T_END_MS 1500000u           /* the frozen instant of the picture */
#define SEED_MS 300000u
#define N_AP 520
#define N_BLE 160
#define N_CELL 5

typedef struct { double e, n; } pt_t;
static const pt_t ROUTE[] = {
    { 0, 0 }, { 620, 40 }, { 700, 420 }, { 1180, 470 }, { 1260, -260 }, { 820, -620 },
    { 260, -540 }, { -240, -300 }, { -320, 160 }, { 0, 0 },
};
#define NR ((int)(sizeof(ROUTE) / sizeof(ROUTE[0])))

typedef struct { double e, n; uint8_t mac[6], auth, ch; char name[33]; } thing_t;
static thing_t AP[N_AP], BLE[N_BLE], CELL[N_CELL];
static bool s_built, s_src[WD_KINDS];
static uint32_t s_t0, s_last_ble, s_last_cell, s_last_rid;
static double s_len;

static uint32_t rnd(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s >> 8; }
static double frand(uint32_t *s) { return (rnd(s) & 0xFFFF) / 65535.0; }

static double route_len(void)
{
    double l = 0;
    for (int i = 1; i < NR; i++) l += hypot(ROUTE[i].e - ROUTE[i - 1].e, ROUTE[i].n - ROUTE[i - 1].n);
    return l;
}

/* Where the car is `d` metres round the loop, and which way it faces. */
static void at(double d, double *e, double *n, double *course)
{
    d = fmod(d, s_len);
    for (int i = 1; i < NR; i++) {
        const double de = ROUTE[i].e - ROUTE[i - 1].e, dn = ROUTE[i].n - ROUTE[i - 1].n, l = hypot(de, dn);
        if (d <= l || i == NR - 1) {
            const double t = l > 0 ? d / l : 0;
            *e = ROUTE[i - 1].e + de * t; *n = ROUTE[i - 1].n + dn * t;
            *course = fmod(atan2(de, dn) * 180 / M_PI + 360, 360);
            return;
        }
        d -= l;
    }
}

static void build(void)
{
    if (s_built) return;
    s_built = true;
    s_len = route_len();
    static const char *const SSID[] = { "NETGEAR%02u", "xfinitywifi", "HOME-%04X", "Linksys%05u", "TP-Link_%04X",
                                        "ATT%uwXy", "DIRECT-roku-%u", "Library Guest", "", "MySpectrumWiFi%02X",
                                        "FBI Van %u", "Verizon_%04X", "", "Pretty Fly %u", "eero-%02u", "CoffeeHouse" };
    static const char *const BTN[] = { "", "", "Tile", "JBL Flip %u", "[TV] Samsung", "Galaxy Buds", "", "Fitbit %u",
                                       "AirPods", "", "Govee H%u", "WHOOP %u" };
    uint32_t s = 20261119;
    for (int i = 0; i < N_AP; i++) {
        double e, n, c;
        at(frand(&s) * s_len, &e, &n, &c);
        thing_t *t = &AP[i];
        t->e = e + (frand(&s) - 0.5) * 120; t->n = n + (frand(&s) - 0.5) * 120;
        for (int k = 0; k < 6; k++) t->mac[k] = (uint8_t)rnd(&s);
        t->mac[0] &= 0xFC;
        const double a = frand(&s);
        t->auth = a < 0.08 ? WD_AUTH_OPEN : a < 0.10 ? WD_AUTH_WEP : a < 0.14 ? WD_AUTH_WPA : a < 0.18 ? WD_AUTH_WPA_WPA2 :
                  a < 0.80 ? WD_AUTH_WPA2 : a < 0.84 ? WD_AUTH_WPA2_EAP : a < 0.93 ? WD_AUTH_WPA2_WPA3 : WD_AUTH_WPA3;
        static const uint8_t CH[] = { 1, 6, 11, 1, 6, 11, 3, 9 };
        t->ch = CH[rnd(&s) & 7];
        snprintf(t->name, sizeof(t->name), SSID[rnd(&s) % 16], (unsigned)(rnd(&s) & 0xFFFF));
        if (!strcmp(t->name, "xfinitywifi") || !strcmp(t->name, "Library Guest") || !strcmp(t->name, "CoffeeHouse")) t->auth = WD_AUTH_OPEN;
    }
    for (int i = 0; i < N_BLE; i++) {
        double e, n, c;
        at(frand(&s) * s_len, &e, &n, &c);
        thing_t *t = &BLE[i];
        t->e = e + (frand(&s) - 0.5) * 60; t->n = n + (frand(&s) - 0.5) * 60;
        for (int k = 0; k < 6; k++) t->mac[k] = (uint8_t)rnd(&s);
        snprintf(t->name, sizeof(t->name), BTN[rnd(&s) % 12], (unsigned)(rnd(&s) % 900 + 100));
    }
    for (int i = 0; i < N_CELL; i++) {
        thing_t *t = &CELL[i];
        t->e = -400 + i * 450.0; t->n = (i & 1) ? 500 : -700;
        memset(t->mac, 0, 6);
        const unsigned earfcn = i < 3 ? 5230 : 2175, pci = 100 + 37u * (unsigned)i;
        t->mac[2] = (uint8_t)(earfcn >> 8); t->mac[3] = (uint8_t)earfcn; t->mac[4] = (uint8_t)(pci >> 8); t->mac[5] = (uint8_t)pci;
        snprintf(t->name, sizeof(t->name), "LTE %u PCI %u", earfcn, pci);
    }
}

static void car(uint32_t now, double *e, double *n, double *course)
{
    const double d = now > s_t0 ? (now - s_t0) / 1000.0 * SPEED : 0;
    at(d, e, n, course);
}

static int level(double d, double n_exp, uint32_t salt)
{
    const double jitter = ((salt * 2654435761u) >> 28) - 8.0;
    return (int)lround(-32 - 10 * n_exp * log10(d < 1 ? 1 : d) + jitter * 0.6);
}

void wd_feed_start(const bool src[WD_KINDS], int pace) { (void)pace; build(); memcpy(s_src, src, sizeof(s_src)); }
void wd_feed_stop(void) { }
void wd_feed_pace(int pace) { (void)pace; }
bool wd_feed_has(int kind) { return kind >= 0 && kind < WD_KINDS; }

bool wd_feed_fix(wd_fix_t *f, uint32_t now)
{
    build();
    double e, n, course;
    car(now, &e, &n, &course);
    const char *g = getenv("LSSIM_GPS");
    f->fix = !(g && strcmp(g, "fix"));
    f->lat = LAT0 + n / 111194.9266;
    f->lon = LON0 + e / (111194.9266 * cos(LAT0 * M_PI / 180));
    f->alt_m = 95; f->hdop = 0.9f; f->sats = 9;
    f->speed_mps = (float)SPEED; f->course_deg = (float)course; f->compass_deg = -1;
    f->utc = 1795096800u - T_END_MS / 1000 + now / 1000;   /* a November 2026 afternoon */
    return f->fix;
}

static void emit(wd_obs_t *out, int cap, int *k, int kind, const thing_t *t, int rssi, uint32_t now)
{
    if (*k >= cap) return;
    wd_obs_t *o = &out[(*k)++];
    memset(o, 0, sizeof(*o));
    o->kind = (uint8_t)kind;
    memcpy(o->addr, t->mac, 6);
    o->auth = kind == WD_WIFI ? t->auth : 0;
    o->channel = kind == WD_WIFI ? t->ch : 0;
    o->rssi = (int8_t)(rssi < -127 ? -127 : rssi);
    snprintf(o->name, sizeof(o->name), "%s", t->name);
    (void)now;
}

int wd_feed_poll(wd_obs_t *out, int cap, uint32_t now, unsigned *scans)
{
    build();
    double e, n, c;
    car(now, &e, &n, &c);
    int k = 0;
    /* A Wi-Fi scan every 3 s, the last one just before the picture. */
    if (s_src[WD_WIFI] && (int)(T_END_MS - now - 750) % 3000 == 0) {
        (*scans)++;
        for (int i = 0; i < N_AP; i++) {
            const int r = level(hypot(AP[i].e - e, AP[i].n - n), 2.6, (uint32_t)i * 7u + now / 3000);
            if (r > -91) emit(out, cap, &k, WD_WIFI, &AP[i], r, now);
        }
    }
    if (s_src[WD_BLE] && now - s_last_ble >= 1000) {
        s_last_ble = now;
        for (int i = 0; i < N_BLE; i++) {
            const int r = level(hypot(BLE[i].e - e, BLE[i].n - n), 3.0, (uint32_t)i * 13u + now / 1000);
            if (r > -94) emit(out, cap, &k, WD_BLE, &BLE[i], r, now);
        }
    }
    if (s_src[WD_CELL] && now - s_last_cell >= 3000) {
        s_last_cell = now;
        for (int i = 0; i < N_CELL; i++) {
            const int r = level(hypot(CELL[i].e - e, CELL[i].n - n), 2.2, (uint32_t)i);
            if (r > -100) emit(out, cap, &k, WD_CELL, &CELL[i], r, now);
        }
    }
    if (s_src[WD_DRONE] && now - s_last_rid >= 1000 && k < cap) {
        s_last_rid = now;
        const double de = 1000 - e, dn = 150 - n, d = hypot(de, dn);
        if (d < 450) {
            static const thing_t uas = { 1000, 150, { 0x60, 0x60, 0x1f, 0x22, 0x33, 0x44 }, 0, 0, "1581F5FJD239C00DW22E" };
            emit(out, cap, &k, WD_DRONE, &uas, level(d, 2.4, now / 1000), now);
            out[k - 1].own_pos = true;
            out[k - 1].lat = LAT0 + 150 / 111194.9266;
            out[k - 1].lon = LON0 + 1000 / (111194.9266 * cos(LAT0 * M_PI / 180));
        }
    }
    return k;
}

bool wd_feed_spawn(void) { return false; }
bool wd_feed_worker_alive(void) { return false; }
void wd_lock(void) { }
void wd_unlock(void) { }
void *wd_feed_alloc(size_t bytes) { return calloc(1, bytes); }
bool wd_feed_card(void) { return true; }
const char *wd_feed_dir(void)
{
    static char dir[200];
    snprintf(dir, sizeof(dir), "%s/lssim_wardrive", getenv("TEMP") ? getenv("TEMP") : ".");
    WD_MKDIR(dir);
    return dir;
}
const char *wd_feed_board(void) { return "lssim"; }
bool wd_feed_upload(const char *path) { (void)path; return false; }
bool wd_feed_hub(char *out, size_t cap, bool *busy) { snprintf(out, cap, "no hub in the simulator"); *busy = false; return true; }

/* Five minutes of driving before the first frame. */
void lssim_wardrive_seed(void)
{
    const char *m = getenv("LSSIM_WD");
    ls_shim_time_set((int64_t)T_END_MS * 1000);
    if (m && !strcmp(m, "idle")) return;
    build();
    s_t0 = T_END_MS - SEED_MS;
    s_last_ble = s_last_cell = s_last_rid = s_t0 - 5000;
    const bool src[WD_KINDS] = { true, true, true, true };
    wd_lock();
    wd_begin(s_t0, src, false);
    wd_unlock();
    wd_feed_start(src, WD_PACE_NORMAL);
    for (uint32_t t = s_t0; t <= T_END_MS; t += 250) wd_step(t);
    /* LSSIM_WD=stopped: the drive over, its files written. */
    if (m && !strcmp(m, "stopped")) {
        wd_lock(); wd_end(T_END_MS); wd_request(WD_REQ_EXPORT); wd_unlock();
        wd_step(T_END_MS);
    }
}
