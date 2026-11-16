/* BUGHUNT's detection core: see bh_core.h. */
#include "bh_core.h"
#include "lap_dot11.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---- the band plan ---------------------------------------------------------- */

typedef struct {
    const char *name, *what;
    uint8_t kind, regions, modes;   /* modes: bit 0 ROOM, bit 1 CAR */
    uint32_t lo_khz, hi_khz, step_khz;
} band_def_t;

/* Uplink only: where a tracker's modem transmits. 2 MHz steps, the radio
   listening 500 kHz wide at each. B12/B28 merge when both regions are on. */
static const band_def_t BANDS[] = {
    { "B12", "LTE uplink 699-716", BH_BAND_CELL, BH_REG_US, 3, 699000, 716000, 2000 },
    { "B28", "LTE uplink 703-748", BH_BAND_CELL, BH_REG_AR, 3, 703000, 748000, 2000 },
    { "B13", "LTE uplink 777-798", BH_BAND_CELL, BH_REG_US, 3, 777000, 798000, 2000 },
    { "B5", "850 uplink 814-849", BH_BAND_CELL, BH_REG_BOTH, 3, 814000, 849000, 2000 },
    { "B8", "900 uplink 880-915", BH_BAND_CELL, BH_REG_AR, 3, 880000, 915000, 2000 },
    { "315", "ISM 315 MHz", BH_BAND_ISM, BH_REG_BOTH, 3, 315000, 315000, 0 },
    { "UHF", "UHF bug band 400-470", BH_BAND_UHF, BH_REG_BOTH, 1, 400000, 470000, 5000 },
    { "433", "ISM 433.92 MHz", BH_BAND_ISM, BH_REG_BOTH, 3, 433920, 433920, 0 },
    /* 868 is left out: in the Americas it sits under the 850 downlink (869-894),
       which spills into it. 915 stops short of the 900 downlink from 925. */
    { "915", "ISM 902-924", BH_BAND_ISM, BH_REG_BOTH, 3, 902000, 924000, 2000 },
    { "2G4", "2.4 GHz energy", BH_BAND_24G, BH_REG_BOTH, 1, 2402000, 2482000, 4000 },
};
#define NBANDS ((int)(sizeof(BANDS) / sizeof(BANDS[0])))

static void add_band(bh_t *h, const band_def_t *b, uint32_t lo, uint32_t hi, const char *name, const char *what)
{
    if (h->nband >= BH_MAX_BANDS) return;
    bh_band_t *bd = &h->band[h->nband];
    snprintf(bd->name, sizeof(bd->name), "%s", name);
    snprintf(bd->what, sizeof(bd->what), "%s", what);
    bd->kind = b->kind;
    bd->lo_khz = lo;
    bd->hi_khz = hi;
    bd->first = (uint16_t)h->nstep;
    bd->peak = BH_NONE;
    /* step centres: lo + step/2 so the 500 kHz window sits inside each step */
    if (!b->step_khz) {
        if (h->nstep < BH_MAX_STEPS) h->step[h->nstep++].khz = lo;
    } else {
        for (uint32_t f = lo + b->step_khz / 2; f <= hi && h->nstep < BH_MAX_STEPS; f += b->step_khz) {
            bool dup = false;                   /* 902-928 overlaps 880-915: one step each */
            for (int s = 0; s < h->nstep && !dup; s++) dup = h->step[s].khz == f;
            if (!dup) h->step[h->nstep++].khz = f;
        }
    }
    bd->count = (uint16_t)(h->nstep - bd->first);
    for (int i = bd->first; i < h->nstep; i++) h->step[i].band = (uint8_t)h->nband;
    if (bd->count) h->nband++;
}

void bh_init(bh_t *h, int mode, int region, bool has_24g, uint32_t dur_ms, uint32_t now_ms)
{
    memset(h, 0, sizeof(*h));
    h->mode = (uint8_t)mode;
    h->region = (uint8_t)(region & BH_REG_BOTH ? region & BH_REG_BOTH : BH_REG_BOTH);
    h->has_24g = has_24g;
    h->t0_ms = now_ms;
    h->dur_ms = dur_ms ? dur_ms : 240000u;
    h->slot_ms = h->dur_ms / BH_SLOTS;
    if (h->slot_ms < 1000u) h->slot_ms = 1000u;
    h->sweep_ms = 16000u;
    h->ref = h->qtop = BH_NONE;
    for (int b = 0; b < BH_MAX_BANDS; b++) h->bqtop[b] = BH_NONE;
    const unsigned mbit = mode == BH_CAR ? 2u : 1u;
    for (int i = 0; i < NBANDS; i++) {
        const band_def_t *b = &BANDS[i];
        if (!(b->modes & mbit) || !(b->regions & h->region)) continue;
        if (b->kind == BH_BAND_24G && !has_24g) continue;
        /* both regions: B12 and B28 overlap, so one band 699-748 */
        if (!strcmp(b->name, "B12") && (h->region & BH_REG_AR)) continue;
        if (!strcmp(b->name, "B28") && (h->region & BH_REG_US)) {
            add_band(h, b, 699000, 748000, "B28", "LTE uplink 699-748");
            continue;
        }
        add_band(h, b, b->lo_khz, b->hi_khz, b->name, b->what);
    }
    for (int s = 0; s < h->nstep; s++) {
        h->step[s].floor = h->step[s].peak = h->step[s].last = BH_NONE;
        for (int k = 0; k < BH_SLOTS; k++) h->sslot[s][k] = BH_NONE;
    }
    for (int b = 0; b < BH_MAX_BANDS; b++)
        for (int k = 0; k < BH_SLOTS; k++) h->slot[b][k] = h->hslot[b][k] = BH_NONE;
    bw_init(&h->ble);
}

int bh_chunks(const bh_t *h, int max)
{
    int n = 0;
    for (int b = 0; b < h->nband; b++) n += h->band[b].failed ? 0 : h->band[b].count;
    return max > 0 ? (n + max - 1) / max : 0;
}
int bh_chunk(const bh_t *h, int k, int max, uint64_t *hz)
{
    int at = 0, n = 0;
    for (int s = 0; s < h->nstep; s++) {
        if (h->band[h->step[s].band].failed) continue;
        if (at++ / max == k) hz[n++] = (uint64_t)h->step[s].khz * 1000u;
    }
    return n;
}
int bh_step_of(const bh_t *h, uint64_t hz)
{
    const uint32_t khz = (uint32_t)(hz / 1000u);
    for (int s = 0; s < h->nstep; s++) {
        const uint32_t d = khz > h->step[s].khz ? khz - h->step[s].khz : h->step[s].khz - khz;
        if (d <= 50) return s;
    }
    return -1;
}
int bh_slot_now(const bh_t *h, uint32_t now_ms)
{
    const uint32_t k = (now_ms - h->t0_ms) / (h->slot_ms ? h->slot_ms : 1000u);
    return k >= BH_SLOTS ? BH_SLOTS - 1 : (int)k;
}

/* ---- RF --------------------------------------------------------------------- */

/* A sample over this is "hot": 10 dB over the step's own quiet level, more
   where the level wanders (cell tower downlink next door). */
int bh_hot_threshold(const bh_t *h, int s)
{
    const bh_step_t *st = &h->step[s];
    if (st->floor == BH_NONE || st->visits < 4) return 32767;     /* learn the step first */
    const int spread = st->dev > 0 ? st->dev : 0;
    int over = 100;
    if (25 + 3 * spread > over) over = 25 + 3 * spread;
    return st->floor + over;
}
bool bh_step_carrier(const bh_t *h, int s)
{
    const bh_step_t *st = &h->step[s];
    return h->ref != BH_NONE && st->visits >= 3 && st->floor != BH_NONE && st->floor >= h->ref + 120;
}
static uint32_t merge_ms(const bh_t *h)
{
    uint32_t m = h->sweep_ms + h->sweep_ms / 4;
    return m < 6000u ? 6000u : m > 30000u ? 30000u : m;
}

void bh_rf_sample(bh_t *h, uint64_t hz, int tenths, uint32_t now)
{
    const int s = bh_step_of(h, hz);
    if (s < 0) return;
    /* -125 dBm and below is the radio settling after a retune, not air */
    if (tenths <= -1250 || tenths > 200) return;
    bh_step_t *st = &h->step[s];
    bh_band_t *b = &h->band[st->band];
    h->samples++;
    h->last_sample_ms = now;
    const int thr = bh_hot_threshold(h, s);
    const bool hot = tenths >= thr;
    if (st->floor == BH_NONE) st->floor = (int16_t)tenths;
    else if (tenths < st->floor) st->floor = (int16_t)(st->floor + (tenths - st->floor) * 3 / 4);
    else if (!hot) st->floor = (int16_t)(st->floor + (tenths - st->floor + 63) / 64);
    else st->floor = (int16_t)(st->floor + (tenths - st->floor + 255) / 256);
    /* the spread learns from quiet samples only; while the step is still
       being learned, anything 15 dB over the provisional floor is a burst */
    const bool quiet_sample = st->visits < 4 ? tenths < st->floor + 150 : !hot;
    if (quiet_sample) {
        const int d = tenths - st->floor > 0 ? tenths - st->floor : 0;
        st->dev = (int16_t)(st->visits ? st->dev + (d - st->dev) / 8 : d);
    }
    st->visits++;
    st->last = (int16_t)tenths;
    st->last_ms = now;
    if (st->peak == BH_NONE || tenths > st->peak) st->peak = (int16_t)tenths;
    if (b->peak == BH_NONE || tenths > b->peak) b->peak = (int16_t)tenths;
    const int k = bh_slot_now(h, now);
    if (tenths > h->sslot[s][k]) h->sslot[s][k] = (int16_t)tenths;
    if (tenths > h->slot[st->band][k]) h->slot[st->band][k] = (int16_t)tenths;
    const int ex = thr == 32767 ? BH_NONE + 1 : tenths - thr;
    if (ex > h->hslot[st->band][k]) h->hslot[st->band][k] = (int16_t)(ex > 32000 ? 32000 : ex < -32000 ? -32000 : ex);
    if (!hot) return;
    st->hot++;
    b->hot_ms = now;
    bh_burst_t *last = b->nburst ? &b->burst[b->nburst - 1] : NULL;
    if (last && now - last->end_ms <= merge_ms(h)) {
        last->end_ms = now;
        if (tenths > last->peak) { last->peak = (int16_t)tenths; last->step = (uint16_t)s; }
        return;
    }
    if (b->nburst == BH_MAX_BURSTS) {       /* keep the latest: drop the oldest */
        memmove(&b->burst[0], &b->burst[1], sizeof(b->burst[0]) * (BH_MAX_BURSTS - 1));
        b->nburst--;
    }
    b->burst[b->nburst++] = (bh_burst_t){ now, now, (int16_t)tenths, (uint16_t)s };
}
void bh_rf_refused(bh_t *h, uint64_t hz)
{
    const int s = bh_step_of(h, hz);
    if (s >= 0) h->band[h->step[s].band].failed = true;
}

static int cmp16(const void *a, const void *b) { return *(const int16_t *)a - *(const int16_t *)b; }
static void sort16(int16_t *v, int n)
{
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && cmp16(&v[j - 1], &v[j]) > 0; j--) { const int16_t t = v[j]; v[j] = v[j - 1]; v[j - 1] = t; }
}
void bh_tick(bh_t *h, uint32_t now)
{
    int16_t f[BH_MAX_STEPS];
    int n = 0;
    for (int s = 0; s < h->nstep; s++)
        if (h->step[s].visits >= 2 && h->step[s].floor != BH_NONE && h->band[h->step[s].band].kind != BH_BAND_24G)
            f[n++] = h->step[s].floor;
    if (n >= 3) { sort16(f, n); h->ref = f[n / 2]; }
    n = 0;
    for (int s = 0; s < h->nstep; s++) {
        const int t = bh_hot_threshold(h, s);
        if (t != 32767 && h->band[h->step[s].band].kind != BH_BAND_24G) f[n++] = (int16_t)t;
    }
    if (n >= 3) { sort16(f, n); h->qtop = f[n / 2]; }
    for (int b = 0; b < h->nband; b++) {
        n = 0;
        for (int s = h->band[b].first; s < h->band[b].first + h->band[b].count; s++) {
            const int t = bh_hot_threshold(h, s);
            if (t != 32767) f[n++] = (int16_t)t;
        }
        if (n) { sort16(f, n); h->bqtop[b] = f[n / 2]; }
    }
    bw_tick(&h->ble, now);
}

/* ---- periodicity --------------------------------------------------------------- */

uint32_t bh_period(const uint32_t *t, int n, uint32_t jitter_ms, int *matched)
{
    if (matched) *matched = 0;
    if (n < 3) return 0;
    uint32_t best = 0;
    int best_m = 0, best_sum = 0;
    for (int i = 0; i + 1 < n; i++) {
        const uint32_t d = t[i + 1] - t[i];
        for (int k = 1; k <= 3; k++) {
            const uint32_t P = d / (uint32_t)k;
            if (P < 60000u || P > 900000u) continue;
            const uint32_t tol = jitter_ms > P / 10 ? jitter_ms : P / 10;
            if (tol * 5 > P) continue;                  /* too coarse: chance would fit it */
            int m = 0, sum = 0;
            for (int j = 0; j + 1 < n; j++) {
                const uint32_t dj = t[j + 1] - t[j];
                const uint32_t q = (dj + P / 2) / P;
                if (q < 1 || q > 4) continue;
                const uint32_t e = dj > q * P ? dj - q * P : q * P - dj;
                if (e <= tol) { m++; sum += (int)q; }
            }
            /* more intervals fit wins; then fewer missed check-ins (the larger period) */
            if (m > best_m || (m == best_m && m && sum < best_sum)) { best = P; best_m = m; best_sum = sum; }
        }
    }
    const int intervals = n - 1;
    /* most intervals fit, and few check-ins were missed */
    if (best_m < 2 || best_m * 5 < intervals * 4 || best_sum * 4 > best_m * 5 + 4) return 0;
    /* refine: total span of the fitting intervals over their multiples */
    uint32_t span = 0;
    int q_all = 0;
    const uint32_t tol = jitter_ms > best / 10 ? jitter_ms : best / 10;
    for (int j = 0; j + 1 < n; j++) {
        const uint32_t dj = t[j + 1] - t[j];
        const uint32_t q = (dj + best / 2) / best;
        if (q < 1 || q > 4) continue;
        const uint32_t e = dj > q * best ? dj - q * best : q * best - dj;
        if (e <= tol) { span += dj; q_all += (int)q; }
    }
    if (q_all) best = span / (uint32_t)q_all;
    if (matched) *matched = best_m;
    return best;
}

/* ---- Bluetooth: the tag engine (bh_tags.c) does the work ---------------------------- */

void bh_ble_advert(bh_t *h, const uint8_t addr[6], uint8_t addr_type, uint8_t pdu, int rssi, const uint8_t *ad,
                   size_t n, uint32_t now)
{
    h->ble_adverts++;
    bw_add(&h->ble, addr, addr_type & 1, pdu, rssi, ad, n, now);
}

/* ---- Wi-Fi ----------------------------------------------------------------------- */

typedef struct { uint8_t p[3]; uint8_t v; } oui_t;
static const char *const VENDORS[] = { "Hikvision", "Dahua", "Wyze", "Reolink", "Axis", "Espressif" };
#define V_ESP 6
static const oui_t OUIS[] = {
    {{0x44, 0x19, 0xB6}, 1}, {{0x4C, 0xBD, 0x8F}, 1}, {{0x54, 0xC4, 0x15}, 1}, {{0x28, 0x57, 0xBE}, 1},
    {{0xC0, 0x56, 0xE3}, 1}, {{0xBC, 0xAD, 0x28}, 1}, {{0xA4, 0x14, 0x37}, 1}, {{0x18, 0x68, 0xCB}, 1},
    {{0xC4, 0x2F, 0x90}, 1},
    {{0x90, 0x02, 0xA9}, 2}, {{0x3C, 0xEF, 0x8C}, 2}, {{0x4C, 0x11, 0xBF}, 2}, {{0x14, 0xA7, 0x8B}, 2},
    {{0xE0, 0x50, 0x8B}, 2}, {{0x38, 0xAF, 0x29}, 2},
    {{0x2C, 0xAA, 0x8E}, 3}, {{0xD0, 0x3F, 0x27}, 3}, {{0x7C, 0x78, 0xB2}, 3},
    {{0xEC, 0x71, 0xDB}, 4},
    {{0x00, 0x40, 0x8C}, 5}, {{0xAC, 0xCC, 0x8E}, 5}, {{0xB8, 0xA4, 0x4F}, 5},
    {{0x24, 0x0A, 0xC4}, 6}, {{0x30, 0xAE, 0xA4}, 6}, {{0xA4, 0xCF, 0x12}, 6}, {{0x84, 0xF3, 0xEB}, 6},
    {{0x60, 0x01, 0x94}, 6}, {{0x24, 0x6F, 0x28}, 6}, {{0x3C, 0x71, 0xBF}, 6}, {{0x84, 0xCC, 0xA8}, 6},
    {{0xAC, 0x67, 0xB2}, 6}, {{0xB4, 0xE6, 0x2D}, 6}, {{0xBC, 0xDD, 0xC2}, 6}, {{0xC4, 0x4F, 0x33}, 6},
    {{0xCC, 0x50, 0xE3}, 6}, {{0xD8, 0xA0, 0x1D}, 6}, {{0xE8, 0xDB, 0x84}, 6}, {{0xEC, 0xFA, 0xBC}, 6},
    {{0x7C, 0x9E, 0xBD}, 6}, {{0x08, 0x3A, 0xF2}, 6}, {{0x48, 0x3F, 0xDA}, 6}, {{0x58, 0xBF, 0x25}, 6},
    {{0x78, 0xE3, 0x6D}, 6}, {{0x8C, 0xAA, 0xB5}, 6}, {{0x94, 0xB9, 0x7E}, 6}, {{0x98, 0xF4, 0xAB}, 6},
    {{0xA0, 0x20, 0xA6}, 6}, {{0xC8, 0x2B, 0x96}, 6}, {{0xDC, 0x4F, 0x22}, 6}, {{0xF4, 0xCF, 0xA2}, 6},
    {{0x5C, 0xCF, 0x7F}, 6}, {{0x18, 0xFE, 0x34}, 6}, {{0x2C, 0x3A, 0xE8}, 6}, {{0x40, 0xF5, 0x20}, 6},
    {{0x10, 0x52, 0x1C}, 6}, {{0x24, 0x62, 0xAB}, 6}, {{0x7C, 0xDF, 0xA1}, 6}, {{0xC0, 0x49, 0xEF}, 6},
    {{0x34, 0x85, 0x18}, 6}, {{0x54, 0x32, 0x04}, 6}, {{0x64, 0xE8, 0x33}, 6}, {{0x70, 0x04, 0x1D}, 6},
    {{0x74, 0x4D, 0xBD}, 6}, {{0xA0, 0x76, 0x4E}, 6}, {{0xB0, 0xA7, 0x32}, 6}, {{0xD4, 0xD4, 0xDA}, 6},
    {{0xE4, 0x65, 0xB8}, 6}, {{0xEC, 0x62, 0x60}, 6}, {{0xF4, 0x12, 0xFA}, 6}, {{0x48, 0x27, 0xE2}, 6},
};
static int vendor_of(const uint8_t mac[6])
{
    if (mac[0] & 0x02) return 0;            /* locally administered: random, no maker */
    for (unsigned i = 0; i < sizeof(OUIS) / sizeof(OUIS[0]); i++)
        if (!memcmp(mac, OUIS[i].p, 3)) return OUIS[i].v;
    return 0;
}
const char *bh_cam_vendor(const uint8_t mac[6])
{
    const int v = vendor_of(mac);
    return v && v != V_ESP ? VENDORS[v - 1] : NULL;
}

#define BUCKET_MS 10000u
#define STEADY_BYTES 50000u             /* in a 10 s bucket: 5 kB/s */
static void close_buckets(bh_sta_t *s, uint32_t b)
{
    if (b == s->bucket_at) return;
    if (s->bucket_bytes >= STEADY_BYTES) s->buckets_on++;
    s->buckets += (uint16_t)(b - s->bucket_at > 60 ? 60 : b - s->bucket_at);
    s->bucket_at = b;
    s->bucket_bytes = 0;
}
void bh_wifi_frame(bh_t *h, const uint8_t *f, size_t n, size_t orig, int rssi, uint32_t now)
{
    lap_dot11_t d;
    if (!lap_dot11_parse(f, n, &d) || !d.have_ta) return;
    h->wifi_frames++;
    const bool beacon = d.type == LAP_DOT11_MGMT && d.subtype == LAP_DOT11_BEACON;
    const bool up = d.type == LAP_DOT11_DATA && d.to_ds && !d.from_ds;
    if (!up && !beacon && d.type != LAP_DOT11_DATA) return;
    if (lap_dot11_is_multicast(d.ta)) return;
    bh_sta_t *s = NULL;
    for (int i = 0; i < h->nsta; i++)
        if (!memcmp(h->sta[i].mac, d.ta, 6)) { s = &h->sta[i]; break; }
    if (!s) {
        if (h->nsta < BH_MAX_STA) s = &h->sta[h->nsta++];
        else {                                          /* replace the quietest */
            int q = 0;
            for (int i = 1; i < h->nsta; i++) if (h->sta[i].up_bytes < h->sta[q].up_bytes) q = i;
            s = &h->sta[q];
        }
        memset(s, 0, sizeof(*s));
        memcpy(s->mac, d.ta, 6);
        s->vendor = (uint8_t)vendor_of(d.ta);
        s->first_ms = now;
        s->bucket_at = now / BUCKET_MS;
        s->peak = -128;
    }
    close_buckets(s, now / BUCKET_MS);
    s->frames++;
    s->last_ms = now;
    s->rssi = (int8_t)rssi;
    if (rssi > s->peak) s->peak = (int8_t)rssi;
    if (up) {
        s->up_frames++;
        s->up_bytes += (uint32_t)orig;
        s->bucket_bytes += (uint32_t)orig;
    }
}
static bool sta_steady(const bh_sta_t *s, uint32_t now)
{
    bh_sta_t c = *s;
    close_buckets(&c, now / BUCKET_MS);
    const uint32_t secs = (c.last_ms - c.first_ms) / 1000u;
    return c.buckets >= 4 && c.buckets_on * 10 >= c.buckets * 7 && secs >= 30 && c.up_bytes / (secs ? secs : 1) >= 5000u;
}

/* ---- HUNT helpers ------------------------------------------------------------------ */

void bh_dir_reset(bh_dir_t *d)
{
    for (int i = 0; i < BH_DIRS; i++) d->best[i] = BH_NONE;
    d->peak = BH_NONE;
    d->peak_bin = -1;
}
void bh_dir_add(bh_dir_t *d, float hd, int tenths)
{
    if (!(hd >= 0.0f)) return;
    int b = (int)(hd / (360.0f / BH_DIRS) + 0.5f) % BH_DIRS;
    if (b < 0) b += BH_DIRS;
    if (tenths > d->best[b]) d->best[b] = (int16_t)tenths;
    if (d->peak == BH_NONE || tenths > d->peak) { d->peak = (int16_t)tenths; d->peak_bin = b; }
}
int bh_dir_covered(const bh_dir_t *d)
{
    int n = 0;
    for (int i = 0; i < BH_DIRS; i++) n += d->best[i] != BH_NONE;
    return n;
}
uint32_t bh_geiger_ms(int tenths, int floor_tenths)
{
    const float x = (float)(tenths - floor_tenths) / 10.0f;
    float ms = 1200.0f * powf(2.0f, -(x < 0 ? 0 : x) / 8.0f);
    if (ms < 35.0f) ms = 35.0f;
    if (ms > 1200.0f) ms = 1200.0f;
    return (uint32_t)ms;
}

/* ---- verdicts ------------------------------------------------------------------- */

static void mhz_text(uint32_t khz, char *out, size_t cap)
{
    if (khz % 1000u) snprintf(out, cap, "%u.%02u", (unsigned)(khz / 1000u), (unsigned)(khz % 1000u / 10u));
    else snprintf(out, cap, "%u", (unsigned)(khz / 1000u));
}
static void dur_text(uint32_t ms, char *out, size_t cap)
{
    const uint32_t s = (ms + 500u) / 1000u;
    if (s < 90) snprintf(out, cap, "%u s", (unsigned)s);
    else if (s % 60 == 0 || s >= 600) snprintf(out, cap, "%u min", (unsigned)((s + 30) / 60));
    else snprintf(out, cap, "%u min %u s", (unsigned)(s / 60), (unsigned)(s % 60));
}
static const char *strength(int tenths)
{
    return tenths >= -450 ? "very strong" : tenths >= -650 ? "strong" : tenths >= -850 ? "moderate" : "weak";
}
static bh_find_t *add(bh_find_t *out, int *n, int max, int kind, int conf, int target)
{
    if (*n >= max) {                                    /* full: replace the weakest if this beats it */
        int w = 0;
        for (int i = 1; i < *n; i++) if (out[i].conf < out[w].conf) w = i;
        if (out[w].conf >= conf) return NULL;
        memset(&out[w], 0, sizeof(out[w]));
        out[w].kind = (uint8_t)kind; out[w].conf = (uint8_t)(conf > 99 ? 99 : conf < 1 ? 1 : conf); out[w].target = (uint8_t)target;
        return &out[w];
    }
    bh_find_t *f = &out[(*n)++];
    memset(f, 0, sizeof(*f));
    f->kind = (uint8_t)kind;
    f->conf = (uint8_t)(conf > 99 ? 99 : conf < 1 ? 1 : conf);
    f->target = (uint8_t)target;
    return f;
}

static void rf_findings(bh_t *h, uint32_t now, bh_find_t *out, int *n, int max)
{
    (void)now;
    for (int b = 0; b < h->nband; b++) {
        bh_band_t *bd = &h->band[b];
        if (bd->failed) continue;
        /* bursts, leaving out steps that carry a steady signal */
        uint32_t t[BH_MAX_BURSTS];
        int nb = 0, pk = BH_NONE, pk_step = -1;
        for (int i = 0; i < bd->nburst; i++) {
            if (bh_step_carrier(h, bd->burst[i].step)) continue;
            t[nb++] = bd->burst[i].t_ms;
            if (bd->burst[i].peak > pk) { pk = bd->burst[i].peak; pk_step = bd->burst[i].step; }
        }
        char mhz[12], per[16];
        const bool cell = bd->kind == BH_BAND_CELL;
        if (nb && bd->kind != BH_BAND_24G) {
            int matched = 0;
            const uint32_t P = bh_period(t, nb, h->sweep_ms, &matched);
            mhz_text(h->step[pk_step].khz, mhz, sizeof(mhz));
            if (P) {
                dur_text(P >= 60000u ? (P + 5000u) / 10000u * 10000u : P, per, sizeof(per));
                int conf = matched >= 4 ? 80 : matched == 3 ? 65 : 45;
                if (!cell) conf -= 20;
                if (pk >= -650) conf += 10;
                if (pk < -800) conf -= 20;              /* weak: a tracker in the room or car would be loud */
                bh_find_t *f = add(out, n, max, cell ? BH_F_CELL_PERIODIC : BH_F_ISM_PERIODIC, conf, BH_T_FREQ);
                if (f) {
                    f->band = (uint8_t)b; f->khz = h->step[pk_step].khz; f->level = (int16_t)pk; f->period_ms = P; f->count = (uint16_t)nb;
                    snprintf(f->title, sizeof(f->title), "%s MHz burst every ~%s", mhz, per);
                    if (cell)
                        snprintf(f->detail, sizeof(f->detail), "%s, %s (%d dBm), %d bursts: possible cellular tracker%s. Phones check in too: airplane-mode yours and sweep again.",
                                 bd->what, strength(pk), pk / 10, nb, pk >= -650 ? " close by" : pk < -800 ? ", but far: likely not in here" : "");
                    else
                        snprintf(f->detail, sizeof(f->detail), "%s, %s (%d dBm), %d bursts. Sensors, remotes and tyre gauges do this too; a strong one inside is worth a HUNT.",
                                 bd->what, strength(pk), pk / 10, nb);
                }
            } else if (cell && nb >= 5) {
                bh_find_t *f = add(out, n, max, BH_F_PHONE, 45, BH_T_FREQ);
                if (f) {
                    f->band = (uint8_t)b; f->khz = h->step[pk_step].khz; f->level = (int16_t)pk; f->count = (uint16_t)nb;
                    snprintf(f->title, sizeof(f->title), "Busy %s uplink, no pattern", bd->name);
                    snprintf(f->detail, sizeof(f->detail), "%d irregular bursts near %s MHz, %s (%d dBm): most likely a phone in use (yours? airplane mode helps). A tracker checks in on a schedule.",
                             nb, mhz, strength(pk), pk / 10);
                }
            } else if (cell) {
                bh_find_t *f = add(out, n, max, BH_F_CELL_BURSTS, (nb >= 3 ? 30 : 20) - (pk < -800 ? 10 : 0), BH_T_FREQ);
                if (f) {
                    f->band = (uint8_t)b; f->khz = h->step[pk_step].khz; f->level = (int16_t)pk; f->count = (uint16_t)nb;
                    snprintf(f->title, sizeof(f->title), "%d cellular burst%s at %s MHz", nb, nb == 1 ? "" : "s", mhz);
                    snprintf(f->detail, sizeof(f->detail), "%s, %s (%d dBm). Too few for a pattern: a tracker needs 3+ check-ins. Sweep longer (CAR watches 15 min).",
                             bd->what, strength(pk), pk / 10);
                }
            }
        }
        /* very strong: anything this loud is within a metre or two. Not at
           2.4 GHz, where the board's own Wi-Fi and Bluetooth radio sits a
           few centimetres away (-27 dBm on the LR2021 board) */
        if (bd->peak != BH_NONE && bd->peak >= -450 && bd->kind != BH_BAND_24G) {
            int ps = bd->first;
            for (int s = bd->first; s < bd->first + bd->count; s++) if (h->step[s].peak > h->step[ps].peak) ps = s;
            mhz_text(h->step[ps].khz, mhz, sizeof(mhz));
            bh_find_t *f = add(out, n, max, BH_F_STRONG, cell ? 55 : 60, BH_T_FREQ);
            if (f) {
                f->band = (uint8_t)b; f->khz = h->step[ps].khz; f->level = bd->peak;
                snprintf(f->title, sizeof(f->title), "Very strong signal at %s MHz", mhz);
                snprintf(f->detail, sizeof(f->detail), "%d dBm on %s: a transmitter within a metre or two. %s",
                         bd->peak / 10, bd->what, cell ? "Your own phone does this when it sends; check it is in airplane mode." : "HUNT it: it gets louder as you close in.");
            }
        }
        /* steady carriers: not in the cell bands, where the towers' downlink sits next door */
        if (!cell) {
            int cs = -1;
            for (int s = bd->first; s < bd->first + bd->count; s++)
                if (bh_step_carrier(h, s) && (cs < 0 || h->step[s].floor > h->step[cs].floor)) cs = s;
            if (cs >= 0) {
                const int over = (h->step[cs].floor - h->ref) / 10;
                int conf = over >= 30 ? 70 : over >= 20 ? 55 : 40;
                mhz_text(h->step[cs].khz, mhz, sizeof(mhz));
                bh_find_t *f = add(out, n, max, BH_F_CARRIER, conf, BH_T_FREQ);
                if (f) {
                    f->band = (uint8_t)b; f->khz = h->step[cs].khz; f->level = h->step[cs].floor;
                    snprintf(f->title, sizeof(f->title), "Steady carrier at %s MHz", mhz);
                    if (bd->kind == BH_BAND_24G)
                        snprintf(f->detail, sizeof(f->detail), "On all the time, %d dB over the quiet level. Wi-Fi comes and goes; a steady 2.4 GHz carrier is how analog video senders work. HUNT it.", over);
                    else
                        snprintf(f->detail, sizeof(f->detail), "On all the time, %d dB over the quiet level (%s). Audio bugs transmit like this; so do some appliances. A bug gets much louder within a metre: HUNT it.",
                                 over, bd->what);
                }
            }
        }
    }
}

static const char *tag_name(int cls)
{
    switch (cls) {
    case BW_C_FINDMY: return "AirTag / Find My device";
    case BW_C_APPLE_PP: return "Apple pairing device";
    case BW_C_TILE: return "Tile tracker";
    case BW_C_SMARTTAG: return "Samsung SmartTag";
    case BW_C_FMDN: return "Google Find Hub tag";
    case BW_C_DULT: return "Separated tracker tag";
    case BW_C_CHIPOLO: return "Chipolo tag";
    case BW_C_FASTPAIR: return "Fast Pair device";
    default: return "Bluetooth tag";
    }
}
static void ble_findings(bh_t *h, uint32_t now, bh_find_t *out, int *n, int max)
{
    const uint32_t dur = now - h->t0_ms;
    char dt[24];
    /* Apple devices that say they are with their owner (AirPods, AirTags at
       home, phones) are everywhere people are: one card for all of them. */
    int near_n = 0, near_best = -1;
    for (int i = 0; i < h->ble.n; i++) {
        const bw_dev_t *d = &h->ble.dev[i];
        if (d->succ || !d->ad.level || now - d->last_ms >= 60000u) continue;
        const bool apple = d->ad.cls == BW_C_FINDMY || d->ad.cls == BW_C_APPLE_PP;
        if (apple && d->ad.separated != 1) {
            near_n++;
            if (near_best < 0 || d->rssi > h->ble.dev[near_best].rssi) near_best = i;
        }
    }
    if (near_n >= 2) {
        const bw_dev_t *d = &h->ble.dev[near_best];
        bh_find_t *f = add(out, n, max, BH_F_BLE_TAG, 15, BH_T_BLE);
        if (f) {
            memcpy(f->addr, d->addr, 6);
            f->addr_type = d->addr_type;
            f->uid = d->uid;
            f->level = (int16_t)(d->rssi * 10);
            f->count = (uint16_t)near_n;
            snprintf(f->title, sizeof(f->title), "%d Apple devices with owners", near_n);
            snprintf(f->detail, sizeof(f->detail), "AirPods, AirTags and phones that say their owner is near; strongest %d dBm. Normal wherever people are. A tag away from its owner is listed on its own.",
                     d->rssi);
        }
    }
    for (int i = 0; i < h->ble.n; i++) {
        const bw_dev_t *d = &h->ble.dev[i];
        if (d->succ || !d->ad.level) continue;
        if (near_n >= 2 && (d->ad.cls == BW_C_FINDMY || d->ad.cls == BW_C_APPLE_PP) && d->ad.separated != 1) continue;
        const uint32_t span = bw_span_ms(d);
        const bool here = now - d->last_ms < 60000u;
        const bool whole = dur >= 90000u && here && span * 10u >= dur * 7u;
        int conf = d->ad.level >= 2 ? 45 : 20;
        if (whole) conf += d->ad.level >= 2 ? 25 : 15;
        if (d->ad.separated == 1) conf += 15;
        if (d->ad.separated == 0) conf -= 30;
        if (d->peak >= -60) conf += 5;
        if (d->rssi < -80) conf -= h->mode == BH_CAR ? 20 : 30;     /* weak: most likely beyond the walls */
        if (!here) conf -= 15;
        bh_find_t *f = add(out, n, max, whole ? BH_F_BLE_FOLLOW : BH_F_BLE_TAG, conf, BH_T_BLE);
        if (!f) continue;
        memcpy(f->addr, d->addr, 6);
        f->addr_type = d->addr_type;
        f->uid = d->uid;
        f->level = (int16_t)(d->rssi * 10);
        f->count = (uint16_t)(d->rotations + 1);
        dur_text(span, dt, sizeof(dt));
        snprintf(f->title, sizeof(f->title), "%s%s", tag_name(d->ad.cls), whole ? (h->mode == BH_CAR ? " with you" : " in range") : "");
        const char *owner = d->ad.separated == 1 ? "away from its owner" : d->ad.separated == 0 ? "near its owner (likely yours or a companion's)" : "owner unknown";
        if (whole)
            snprintf(f->detail, sizeof(f->detail), "Heard the whole sweep (%s), %d dBm, %s: %s.%s", dt, d->rssi, owner,
                     h->mode == BH_CAR ? "it is travelling with you"
                     : d->rssi < -80 ? "weak, so most likely a neighbour's, beyond the walls" : "it is in or near this room",
                     d->rotations ? " Followed through address changes." : "");
        else
            snprintf(f->detail, sizeof(f->detail), "Heard for %s, %d dBm, %s. %s", dt, d->rssi, owner,
                     here ? "Keep sweeping: one that stays the whole time is the one to find." : "Gone now: probably passing by.");
    }
}

static void wifi_findings(bh_t *h, uint32_t now, bh_find_t *out, int *n, int max)
{
    char mac[20];
    for (int i = 0; i < h->nsta; i++) {
        const bh_sta_t *s = &h->sta[i];
        const bool steady = sta_steady(s, now);
        const bool cam = s->vendor && s->vendor != V_ESP;
        if (!steady && !cam) continue;
        int conf = cam && steady ? 85 : cam ? 50 : s->vendor == V_ESP ? 60 : 45;
        if (s->peak >= -50) conf += 5;
        bh_find_t *f = add(out, n, max, steady ? BH_F_WIFI_STREAM : BH_F_WIFI_CAM, conf, BH_T_WIFI);
        if (!f) continue;
        memcpy(f->addr, s->mac, 6);
        f->level = (int16_t)(s->rssi * 10);
        lap_dot11_mac(mac, sizeof(mac), s->mac);
        const uint32_t secs = (s->last_ms - s->first_ms) / 1000u;
        const unsigned kbs = (unsigned)(s->up_bytes / 1000u / (secs ? secs : 1));
        const char *v = s->vendor ? VENDORS[s->vendor - 1] : NULL;
        if (steady) {
            snprintf(f->title, sizeof(f->title), "%s uploading steadily", cam ? v : s->vendor == V_ESP ? "IoT Wi-Fi chip" : "Wi-Fi device");
            snprintf(f->detail, sizeof(f->detail), "%s sends about %u kB/s without a break, %d dBm: like a camera streaming video.%s", mac, kbs, s->rssi,
                     s->vendor == V_ESP ? " Espressif chips are in many cheap Wi-Fi cameras (and smart plugs)." : cam ? " A camera maker's device." : "");
        } else {
            snprintf(f->title, sizeof(f->title), "%s device on Wi-Fi", v);
            snprintf(f->detail, sizeof(f->detail), "%s is made by a camera maker, %d dBm, %u frames. Look for a camera where you would not expect one.",
                     mac, s->rssi, (unsigned)s->frames);
        }
    }
}

static int by_conf(const bh_find_t *a, const bh_find_t *b)
{
    if (a->conf != b->conf) return (int)b->conf - (int)a->conf;
    return (int)a->kind - (int)b->kind;
}
int bh_findings(bh_t *h, uint32_t now, bh_find_t *out, int max)
{
    int n = 0;
    rf_findings(h, now, out, &n, max);
    ble_findings(h, now, out, &n, max);
    wifi_findings(h, now, out, &n, max);
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && by_conf(&out[j - 1], &out[j]) > 0; j--) {
            bh_find_t t = out[j];
            out[j] = out[j - 1];
            out[j - 1] = t;
        }
    return n;
}
