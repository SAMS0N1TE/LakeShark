/* lssim: WARDRIVE's feed (components/apps/tui/wardrive/wd_feed.h) as a
   drive. A loop of streets around the map fixture's town at 47 km/h, access
   points and Bluetooth devices placed beside it, a few LTE cells and one
   Remote ID drone over a field; each is heard when the car is near enough,
   at a level that falls with distance. lssim_wardrive_seed() drives five
   minutes of it before the first frame, so a picture shows a drive under
   way. LSSIM_WD=idle leaves the session unstarted; LSSIM_GPS=nofix keeps the
   fix away. Deterministic: the same picture every run. */
#include "wardrive/wd_feed.h"
#include "wardrive/wd_radio.h"
#include "party/party_wd_peer.h"
#include "esp_timer.h"
#include <direct.h>
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

void wd_feed_start(const bool src[WD_KINDS], int pace) { (void)pace; build(); memcpy(s_src, src, sizeof(s_src)); wd_radio_start(src); }
void wd_feed_stop(void) { wd_radio_stop(); }
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
    /* LSSIM_GPS=garage: the last minute's fixes are weak (HDOP 9), so the
       position comes from the atlas's access points (wd_atlas_locate) */
    if (g && !strcmp(g, "garage") && now + 60000 > T_END_MS) { f->fix = true; f->hdop = 9.0f; f->sats = 3; }
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

/* ------------------------------------------------------------ the radios

   The radio jobs' port (wd_radio.h): an LR2021 in the LoRa socket and an
   RTL-SDR (LSSIM_SDR=none: none plugged in, as on battery). What they hear:
   oncoming traffic, each car with four tyre sensors that send every half
   minute or so while the wheels turn, heard within 45 m; weather stations
   by the houses; aircraft on straight tracks over the town; mesh nodes, some
   MeshCore with their place in the advert, some Meshtastic (header only). */

#define N_CARS 46
#define N_WX 22
#define N_AIR 6
#define N_NODE 8
typedef struct { double off; uint32_t id[4]; uint8_t proto; uint16_t phase[4]; } traffic_t;
typedef struct { double e, n; uint32_t id; uint8_t proto; uint16_t every, phase; } wx_t;
typedef struct { double e0, n0, ve, vn; uint32_t icao; char cs[9]; } plane_t;
typedef struct { double e, n; uint8_t flavour; uint32_t id; uint16_t every, phase; char name[20]; bool pos; } node_t;
static traffic_t TRAF[N_CARS];
static wx_t WX[N_WX];
static plane_t PLANE[N_AIR];
static node_t NODE[N_NODE];
static int s_rjob[WD_RJ_N] = { -1, -1 };
static uint32_t s_rlast[WD_RJ_N];
static bool s_rbuilt;

static void rbuild(void)
{
    if (s_rbuilt) return;
    s_rbuilt = true;
    build();
    uint32_t s = 315000;
    static const uint8_t TP[] = { LR433_P_SCHRADER, LR433_P_SCHRADER_EG53MA4, LR433_P_TOYOTA, LR433_P_FORD,
                                  LR433_P_PMV107J, LR433_P_HYUNDAI_VDO, LR433_P_SCHRADER_SMD3MA4 };
    for (int i = 0; i < N_CARS; i++) {
        TRAF[i].off = s_len * i / N_CARS + frand(&s) * 60;
        TRAF[i].proto = TP[rnd(&s) % 7];
        for (int k = 0; k < 4; k++) { TRAF[i].id[k] = rnd(&s) & 0x0FFFFFFF; TRAF[i].phase[k] = (uint16_t)(rnd(&s) % 30000); }
    }
    static const uint8_t WP[] = { LR433_P_ACURITE_TOWER, LR433_P_ACURITE_5N1, LR433_P_LACROSSE_TX141, LR433_P_AMBIENT_F007TH,
                                  LR433_P_AMBIENT_WH31E };
    for (int i = 0; i < N_WX; i++) {
        double e, n, c;
        at(frand(&s) * s_len, &e, &n, &c);
        WX[i].e = e + (frand(&s) - 0.5) * 140; WX[i].n = n + (frand(&s) - 0.5) * 140;
        WX[i].proto = WP[rnd(&s) % 5];
        WX[i].id = rnd(&s) % 16000;
        WX[i].every = (uint16_t)(31000 + rnd(&s) % 30000);
        WX[i].phase = (uint16_t)(rnd(&s) % WX[i].every);
    }
    static const char *const CS[N_AIR] = { "N512KT", "DAL1734", "SKW5021", "UPS2911", "JBU488", "LN-HOSP" };
    for (int i = 0; i < N_AIR; i++) {
        const double a = frand(&s) * 2 * M_PI, v = 60 + frand(&s) * 170;
        PLANE[i] = (plane_t){ (frand(&s) - 0.5) * 60000, (frand(&s) - 0.5) * 60000, sin(a) * v, cos(a) * v,
                              0xA00000u + (rnd(&s) & 0x0FFFFF), "" };
        snprintf(PLANE[i].cs, sizeof(PLANE[i].cs), "%s", CS[i]);
    }
    static const char *const NN[N_NODE] = { "Ridge Rptr", "KC1QRT mobile", "Lakeview", "", "Hilltop RPT", "", "Base-7", "" };
    for (int i = 0; i < N_NODE; i++) {
        NODE[i].e = (frand(&s) - 0.3) * 3000; NODE[i].n = (frand(&s) - 0.5) * 2600;
        NODE[i].flavour = i == 3 || i == 5 || i == 7 ? WD_MESH_MESHTASTIC : WD_MESH_MESHCORE;
        NODE[i].id = rnd(&s) | 0x10000000u;
        NODE[i].every = (uint16_t)(NODE[i].flavour == WD_MESH_MESHCORE ? 50000 + rnd(&s) % 40000 : 25000 + rnd(&s) % 20000);
        NODE[i].phase = (uint16_t)(rnd(&s) % NODE[i].every);
        NODE[i].pos = NODE[i].flavour == WD_MESH_MESHCORE && i != 1;
        snprintf(NODE[i].name, sizeof(NODE[i].name), "%s", NN[i]);
    }
}

static bool sdr_present(void) { const char *v = getenv("LSSIM_SDR"); return !(v && !strcmp(v, "none")); }

void wd_rport_probe(int r, wd_rport_t *o)
{
    memset(o, 0, sizeof(*o));
    if (r == WD_RJ_LORA) {
        o->present = true;
        snprintf(o->name, sizeof(o->name), "LR2021");
        o->can = 1 << WD_JOB_TPMS | 1 << WD_JOB_ISM | 1 << WD_JOB_MESH;
        if (!sdr_present()) o->can |= 1 << WD_JOB_AIR;     /* ADS-B falls back to the LR2021 */
        o->freq_khz = s_rjob[r] >= 0 ? wd_radio_job_khz(s_rjob[r], WD_REGION_NA) : 0;
        return;
    }
    o->present = sdr_present();
    snprintf(o->name, sizeof(o->name), "RTL-SDR");
    o->can = 1 << WD_JOB_TPMS | 1 << WD_JOB_ISM | 1 << WD_JOB_AIR;
}

bool wd_rport_run(int r, int job, int region, char *why, size_t n)
{
    (void)region; (void)why; (void)n;
    if (s_rjob[r] != job) s_rlast[r] = 0;
    s_rjob[r] = job;
    return true;
}

void wd_rport_start(void) { rbuild(); s_rjob[0] = s_rjob[1] = -1; }
void wd_rport_stop(void) { s_rjob[0] = s_rjob[1] = -1; }
int wd_rport_passive(wd_obs_t *out, int cap, uint32_t now) { (void)out; (void)cap; (void)now; return 0; }

/* Did a transmitter that sends every `every` ms from `phase` send in (a, b]? */
static bool sent(uint32_t a, uint32_t b, uint32_t every, uint32_t phase)
{
    if (!a || b <= a) return false;
    return (b + every - phase) / every != (a + every - phase) / every;
}

static void local_ll(double e, double n, double *lat, double *lon)
{
    *lat = LAT0 + n / 111194.9266;
    *lon = LON0 + e / (111194.9266 * cos(LAT0 * M_PI / 180));
}

int wd_rport_poll(int r, wd_obs_t *out, int cap, uint32_t now)
{
    rbuild();
    const int job = s_rjob[r];
    const uint32_t a = s_rlast[r];
    s_rlast[r] = now;
    if (job < 0 || !a) return 0;
    const int radio = r == WD_RJ_LORA ? WD_RADIO_LORA : WD_RADIO_SDR;
    double e, n, c;
    car(now, &e, &n, &c);
    const double d_car = now > s_t0 ? (now - s_t0) / 1000.0 * SPEED : 0;
    int k = 0;
    lr433_msg_t m;
    if (job == WD_JOB_TPMS) {
        for (int i = 0; i < N_CARS && k < cap; i++) {
            /* oncoming: the gap closes at twice the speed */
            const double d = fmod(TRAF[i].off - d_car * 2 + s_len * 64, s_len), gap = d < s_len / 2 ? d : s_len - d;
            if (gap > 45) continue;
            for (int w = 0; w < 4 && k < cap; w++) {
                if (!sent(a, now, 30000, TRAF[i].phase[w])) continue;
                memset(&m, 0, sizeof(m));
                m.proto = TRAF[i].proto;
                m.channel = -1;
                snprintf(m.id, sizeof(m.id), "%07x", (unsigned)TRAF[i].id[w]);
                if (wd_radio_obs_ism(&out[k], &m, 315000000u, (float)level(gap + 3, 2.4, TRAF[i].id[w]), radio)) k++;
            }
        }
    } else if (job == WD_JOB_ISM) {
        for (int i = 0; i < N_WX && k < cap; i++) {
            const double d = hypot(WX[i].e - e, WX[i].n - n);
            if (d > 150 || !sent(a, now, WX[i].every, WX[i].phase)) continue;
            memset(&m, 0, sizeof(m));
            m.proto = WX[i].proto;
            m.channel = (int8_t)(i % 3);
            snprintf(m.id, sizeof(m.id), "%u", (unsigned)WX[i].id);
            if (wd_radio_obs_ism(&out[k], &m, WX[i].proto == LR433_P_AMBIENT_WH31E ? 915000000u : 433920000u,
                                 (float)level(d, 2.8, WX[i].id), radio)) k++;
        }
    } else if (job == WD_JOB_AIR) {
        if (now / 1000 == a / 1000) return 0;                  /* the table, once a second */
        for (int i = 0; i < N_AIR && k < cap; i++) {
            const double t = now / 1000.0, pe = PLANE[i].e0 + PLANE[i].ve * t / 8, pn = PLANE[i].n0 + PLANE[i].vn * t / 8;
            const double pe2 = fmod(pe + 1e6 + 40000, 80000) - 40000, pn2 = fmod(pn + 1e6 + 40000, 80000) - 40000;
            wd_air_t w = { .icao = PLANE[i].icao, .cat = 0x41, .pos = i != 4, .alt_ft = 3000 + i * 4000 };
            snprintf(w.callsign, sizeof(w.callsign), "%s", PLANE[i].cs);
            double lat, lon;
            local_ll(pe2, pn2, &lat, &lon);
            w.lat = (float)lat; w.lon = (float)lon;
            wd_radio_obs_air(&out[k++], &w, radio);
        }
    } else if (job == WD_JOB_MESH) {
        for (int i = 0; i < N_NODE && k < cap; i++) {
            if (!sent(a, now, NODE[i].every, NODE[i].phase)) continue;
            const node_t *q = &NODE[i];
            uint8_t p[160];
            int len;
            memset(p, 0, sizeof(p));
            if (q->flavour == WD_MESH_MESHTASTIC) {
                p[0] = p[1] = p[2] = p[3] = 0xFF;
                p[4] = (uint8_t)q->id; p[5] = (uint8_t)(q->id >> 8); p[6] = (uint8_t)(q->id >> 16); p[7] = (uint8_t)(q->id >> 24);
                p[12] = 3 << 5 | 2;
                len = 40;
            } else {
                p[0] = 1 | 4 << 2;                              /* flood, advert */
                p[1] = 0;                                       /* no path */
                for (int b = 0; b < 32; b++) p[2 + b] = (uint8_t)(q->id >> (8 * (b & 3))) ^ (uint8_t)b;
                int j = 2 + 100;
                p[j++] = (uint8_t)((i == 0 || i == 4 ? 2 : 1) | (q->pos ? 0x10 : 0) | (q->name[0] ? 0x80 : 0));
                if (q->pos) {
                    double lat, lon;
                    local_ll(q->e, q->n, &lat, &lon);
                    const int32_t la = (int32_t)lround(lat * 1e6), lo = (int32_t)lround(lon * 1e6);
                    memcpy(p + j, &la, 4); memcpy(p + j + 4, &lo, 4);
                    j += 8;
                }
                const size_t nl = strlen(q->name);
                memcpy(p + j, q->name, nl);
                len = j + (int)nl;
            }
            const double d = hypot(q->e - e, q->n - n);
            if (wd_radio_obs_lora(&out[k], p, len, q->flavour, (float)level(d, 2.2, q->id),
                                  q->flavour == WD_MESH_MESHTASTIC ? 906875000u : 910525000u, radio)) k++;
        }
    }
    return k;
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
    if ((s_src[WD_RF] || s_src[WD_AIR] || s_src[WD_LORA]) && k < cap) k += wd_radio_poll(out + k, cap - k, now);
    {
        /* HUNT's targets along the road (lssim_wd_hunt_seed.c) */
        extern int lssim_wd_hunt_obs(wd_obs_t *out, int cap, uint32_t now, double e, double n);
        if (k < cap) k += lssim_wd_hunt_obs(out + k, cap - k, now, e, n);
    }
    /* PARTY's members' finds, when a party runs in the same simulator (every 3 s). */
    {
        static party_find_t pf[16];
        static uint32_t last_ms, since;
        if (now - last_ms >= 3000 && k < cap) {
            last_ms = now;
            if (!since) since = now - 60000;
            const int pn = party_peer_since(pf, 16, since);
            for (int i = 0; i < pn; i++) if ((int32_t)(pf[i].ms - since) > 0) since = pf[i].ms;
            k += party_peers_to_obs(pf, pn, out + k, cap - k);
        }
    }
    return k;
}

bool wd_feed_spawn(void) { return false; }
bool wd_feed_worker_alive(void) { return false; }
void wd_lock(void) { }
void wd_unlock(void) { }
void *wd_feed_alloc(size_t bytes) { return calloc(1, bytes); }
void wd_feed_free(void *p) { free(p); }
bool wd_feed_card(void) { return true; }
const char *wd_feed_dir(void)
{
    static char dir[200];
    snprintf(dir, sizeof(dir), "%s/lssim_wardrive", getenv("TEMP") ? getenv("TEMP") : ".");
    _mkdir(dir);
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
    const bool src[WD_KINDS] = { true, true, true, true, true, true, true };
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
