/* lssim: WARDRIVE ATLAS with history. Before `lssim wardrive` starts, the
   atlas in %TEMP%\lssim_wardrive\atlas is wiped and given four earlier runs
   of the simulator's own loop (lssim_wardrive.c's feed, driven at earlier
   times), each with a different radio job, so the live drive is run 5 of a
   known route: regulars come back, some devices are new, a few regulars are
   gone, a few moved, and coverage builds on what the other runs had.

   The runs go in through wd_atlas_apply as this board's own records (the
   format in wd_atlas.c), the same path a home base sync takes.
   LSSIM_ATLAS=empty leaves a fresh atlas; LSSIM_ATLAS=keep leaves it alone. */
#include "wardrive/wd_atlas.h"
#include "wardrive/wd_feed.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define LOOP_MS 365000u             /* one lap of the loop at the feed's speed */
#define STEP_MS 250u
#define MAXD 2048
#define GONE 6

typedef struct { uint8_t kind, addr[6]; int8_t rssi; double lat, lon; uint32_t first, last; uint16_t seen; char name[33]; bool used; } agg_t;
static agg_t AG[MAXD];
static int nag;

static agg_t *agg(uint8_t kind, const uint8_t a[6])
{
    uint32_t h = 2166136261u ^ kind;
    for (int i = 0; i < 6; i++) h = (h ^ a[i]) * 16777619u;
    for (uint32_t i = h & (MAXD - 1), k = 0; k < MAXD; k++, i = (i + 1) & (MAXD - 1)) {
        if (!AG[i].used) {
            if (nag >= MAXD * 3 / 4) return NULL;
            memset(&AG[i], 0, sizeof(AG[i]));
            AG[i].used = true; AG[i].kind = kind; memcpy(AG[i].addr, a, 6); AG[i].rssi = -128;
            nag++;
            return &AG[i];
        }
        if (AG[i].kind == kind && !memcmp(AG[i].addr, a, 6)) return &AG[i];
    }
    return NULL;
}

/* ---- records, as wd_atlas.c's header comment lays them out ---- */
static uint8_t *S;
static size_t sn, scap;
static uint16_t crc16(const uint8_t *p, size_t n)
{
    uint16_t c = 0xFFFF;
    while (n--) { c ^= (uint16_t)(*p++ << 8); for (int k = 0; k < 8; k++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1); }
    return c;
}
static uint8_t P[256];
static int pn;
static void p8(uint32_t v) { P[pn++] = (uint8_t)v; }
static void p16(uint32_t v) { p8(v); p8(v >> 8); }
static void p32(uint32_t v) { p16(v); p16(v >> 16); }
static void rec(uint8_t type)
{
    if (sn + (size_t)pn + 9 > scap) { scap = scap * 2 + 65536; S = realloc(S, scap); }
    uint8_t *o = S + sn;
    o[0] = 0xA7; o[1] = type; o[2] = (uint8_t)pn; o[3] = o[4] = o[5] = o[6] = 0;
    memcpy(o + 7, P, (size_t)pn);
    const uint16_t c = crc16(o + 1, (size_t)pn + 6);
    o[7 + pn] = (uint8_t)c; o[8 + pn] = (uint8_t)(c >> 8);
    sn += (size_t)pn + 9;
    pn = 0;
}

static void one_run(uint32_t board, uint16_t no, int k, const bool src[WD_KINDS], uint8_t srcmask)
{
    static wd_obs_t obs[256];
    nag = 0;
    memset(AG, 0, sizeof(AG));
    wd_feed_start(src, WD_PACE_NORMAL);
    const uint32_t t0 = (uint32_t)k * LOOP_MS + 1000, back = (uint32_t)(4 - k) * 86400u + 3600u * (uint32_t)k;
    int32_t cells[2][1024];
    int nc = 0;
    double plat = 0, plon = 0, dist = 0;
    uint32_t u0 = 0, u1 = 0;
    for (uint32_t t = t0; t < t0 + LOOP_MS; t += STEP_MS) {
        wd_fix_t f;
        memset(&f, 0, sizeof(f));
        wd_feed_fix(&f, t);
        const uint32_t utc = f.utc - back;
        if (!u0) u0 = utc;
        u1 = utc;
        if (plat != 0) dist += hypot((f.lat - plat) * 111195, (f.lon - plon) * 111195 * cos(f.lat * M_PI / 180));
        plat = f.lat; plon = f.lon;
        const int32_t cx = (int32_t)floor(f.lon * 640), cy = (int32_t)floor(f.lat * 640);
        if (!nc || cells[0][nc - 1] != cx || cells[1][nc - 1] != cy) {
            bool seen = false;
            for (int i = 0; i < nc; i++) if (cells[0][i] == cx && cells[1][i] == cy) seen = true;
            if (!seen && nc < 1024) { cells[0][nc] = cx; cells[1][nc] = cy; nc++; }
        }
        unsigned scans = 0;
        const int n = wd_feed_poll(obs, 256, t, &scans);
        for (int i = 0; i < n; i++) {
            const wd_obs_t *o = &obs[i];
            /* some of today's devices were not there on the earlier runs: NEW on the live one */
            if (o->addr[5] % 11 == 0 && o->kind != WD_CELL) continue;
            agg_t *a = agg(o->kind, o->addr);
            if (!a) continue;
            a->seen++;
            if (!a->first) a->first = utc;
            a->last = utc;
            if (o->rssi > a->rssi) {
                a->rssi = o->rssi;
                a->lat = o->own_pos ? o->lat : f.lat;
                a->lon = o->own_pos ? o->lon : f.lon;
                /* ...and a few sat somewhere else back then: MOVED on the live one */
                if (o->addr[5] % 37 == 1) a->lat += 0.0135;
            }
            if (!a->name[0]) snprintf(a->name, sizeof(a->name), "%s", o->name);
        }
    }
    if (src[WD_WIFI]) {
        /* regulars that are gone today: MISSING on the live one */
        static const char *const NAMES[GONE] = { "Old Diner WiFi", "Bait Shop", "HOME-7A21", "eero-44", "ATT9wXy", "Library Kiosk" };
        for (int g = 0; g < GONE; g++) {
            const uint8_t a6[6] = { 0x9E, 0x0A, 0x1A, 0x55, 0x00, (uint8_t)g };
            agg_t *a = agg(WD_WIFI, a6);
            if (!a) continue;
            /* on the part of the loop the live drive covers: just off its first streets */
            const double e = 80 + g * 95.0, nn = 30 + g * 3.0;
            a->lat = 43.4445 + nn / 111194.9266;
            a->lon = -71.6473 + e / (111194.9266 * cos(43.4445 * M_PI / 180));
            a->rssi = -58; a->seen = 6; a->first = u0 + 10 + (uint32_t)g * 15; a->last = a->first + 20;
            snprintf(a->name, sizeof(a->name), "%s", NAMES[g]);
        }
    }
    /* RUN begin, the cells, the devices, RUN end */
    uint16_t ri = 0;
    p32(board); p16(no); p32(u0); p32(0); p32(0); p32(0); p16(0); p16(0); p8(0); p8(0);
    rec(1);
    for (int i = 0; i < nc;) {
        p32(board); p16(no); p16(ri++);
        const int m = nc - i < 70 ? nc - i : 70;
        p8((uint32_t)m);
        p32((uint32_t)cells[0][i]); p32((uint32_t)cells[1][i]); p8(srcmask);
        for (int j = 1; j < m; j++) {
            p8((uint32_t)(int8_t)(cells[0][i + j] - cells[0][i + j - 1]));
            p8((uint32_t)(int8_t)(cells[1][i + j] - cells[1][i + j - 1]));
            p8(srcmask);
        }
        i += m;
        rec(2);
    }
    int ndev = 0;
    for (int i = 0; i < MAXD; i++) {
        const agg_t *a = &AG[i];
        if (!a->used) continue;
        ndev++;
        p32(board); p16(no); p16(ri++); p8(a->kind);
        for (int j = 0; j < 6; j++) p8(a->addr[j]);
        p8(1u << WD_RADIO_C6); p8(0); p16(0); p8((uint8_t)a->rssi);
        p32((uint32_t)(int32_t)lround(a->lat * 1e7)); p32((uint32_t)(int32_t)lround(a->lon * 1e7));
        p32(a->first); p32(a->last); p16(a->seen);
        const int nl = (int)strlen(a->name) > 32 ? 32 : (int)strlen(a->name);
        p8((uint32_t)nl);
        for (int j = 0; j < nl; j++) p8((uint8_t)a->name[j]);
        rec(3);
    }
    p32(board); p16(no); p32(u0); p32(u1); p32(0); p32((uint32_t)dist); p16((uint32_t)ndev); p16(0); p8(srcmask); p8(0);
    rec(1);
}

__attribute__((constructor)) static void lssim_wd_atlas_seed(void)
{
    const char *cl = GetCommandLineA();
    if (!cl || !strstr(cl, "wardrive")) return;
    const char *mode = getenv("LSSIM_ATLAS");
    if (mode && !strcmp(mode, "keep")) return;
    char p[260];
    static const char *const FILES[] = { "log.bin", "state.bin", "state.tmp", "dev.idx", "dev.tmp" };
    for (size_t i = 0; i < sizeof(FILES) / sizeof(FILES[0]); i++) {
        snprintf(p, sizeof(p), "%s/atlas/%s", wd_feed_dir(), FILES[i]);
        remove(p);
    }
    wd_atlas_reload();
    (void)wd_atlas_seq();           /* loads (and makes) the atlas */
    const uint32_t board = wd_atlas_board();
    if (!board || (mode && !strcmp(mode, "empty"))) return;
    /* four earlier laps, a different radio job each time */
    static const bool JOBS[4][WD_KINDS] = {
        { true, true, false, false },           /* Wi-Fi and Bluetooth */
        { true, false, true, false },           /* Wi-Fi and LTE cells */
        { true, true, true, false },            /* Wi-Fi, Bluetooth, cells */
        { false, true, true, true },            /* Bluetooth, cells, Remote ID: the C6 lent to another app */
    };
    sn = 0;
    if (!S) { scap = 262144; S = malloc(scap); }
    S[0] = 'W'; S[1] = 'A'; S[2] = 1; S[3] = 0;
    sn = 4;
    for (int k = 0; k < 4; k++) {
        uint8_t mask = 0;
        for (int kind = 0; kind < WD_KINDS; kind++) if (JOBS[k][kind]) mask |= (uint8_t)(1u << wd_atlas_src(kind));
        one_run(board, (uint16_t)(k + 1), k, JOBS[k], mask);
    }
    const int merged = wd_atlas_apply(S, sn, board);
    /* the worker's part: the index folded in once the sync has gone quiet */
    uint32_t clock = 1;
    for (int i = 0; i < 4000 && wd_atlas_pending(); i++) { clock += STEP_MS; wd_atlas_step(clock); }
    printf("lssim atlas seed: 4 earlier runs, %d records, %u bytes\n", merged, (unsigned)sn);
}
