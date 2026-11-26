/* WARDRIVE's tracker (wd_track.h). */
#include "wd_track.h"
#include "wd_feed.h"
#include "wd_sig.h"
#include "../ls_tail.h"
#include <math.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif

#define PLACE_M     300.0f
#define FOLLOW_N    4           /* places, and steps */
#define FRESH_MS    10000u      /* wd_core's: a fix older than this is no position */
#define CAND_MAX    64
#define CAND_GONE   120000u     /* a candidate not heard for 2 min stops being compared */
#define GROUP_MS    2000u
#define ALERTS      16

#define F_POS     1
#define F_ALERTED 2

typedef struct {
    uint64_t bits;              /* steps heard in; bit 0 is step `at` */
    uint32_t at, step0;         /* the last step heard in, the first */
    float fx, fz, lx, lz;       /* the board: where first heard, where the last place was counted */
    float span, odo0, odo1;
    uint8_t places, flags;
    uint16_t veh;
} trk_t;

static trk_t *dv;               /* WD_CAP of them, PSRAM */
static EXT_RAM_BSS_ATTR wd_veh_t veh[WD_VEH_MAX];
static int nveh;
static uint16_t veh_next;
static uint32_t step, step_odo_n;   /* steps taken; the odometer's step count when step was last moved */
static uint32_t group_ms;
static wd_alert_t al[ALERTS];
static int al_head, al_n;
static uint32_t hits, hit_ms;

/* ------------------------------------------------------------ helpers */

static int pop64(uint64_t v) { int n = 0; while (v) { v &= v - 1; n++; } return n; }

static uint64_t bits_now(const trk_t *d)
{
    const uint32_t k = step - d->at;
    return k >= 64 ? 0 : d->bits << k;
}

/* 64 steps as buckets of `w` steps (4: 16 buckets, 8: 8). */
static uint16_t coarse(uint64_t b, int w)
{
    uint16_t c = 0;
    const uint64_t m = w >= 64 ? ~0ull : (1ull << w) - 1;
    for (int i = 0; i * w < 64; i++) if ((b >> (i * w)) & m) c |= (uint16_t)(1u << i);
    return c;
}

static int pop16(uint16_t v) { int n = 0; while (v) { v &= (uint16_t)(v - 1); n++; } return n; }
static uint32_t gap(uint32_t a, uint32_t b) { return a > b ? a - b : b - a; }

/* Heard together: in the same stretches of road (buckets of 600 m, or
   1.2 km when a sub-GHz sensor that talks once a minute is one of them),
   first heard within 1.2 km of each other, and if either has gone quiet,
   gone quiet together. Your own phone and the tag in your bag start with the
   drive; the car that pulls in behind you starts where it pulled in. */
static bool together(int ia, uint64_t a, int ib, uint64_t b)
{
    const trk_t *da = &dv[ia], *db = &dv[ib];
    if (gap(da->step0, db->step0) > 8) return false;
    const bool quiet_a = step - da->at > 4, quiet_b = step - db->at > 4;
    if ((quiet_a || quiet_b) && gap(da->at, db->at) > 8) return false;
    const bool slow = wd_dev_at(ia)->kind == WD_RF || wd_dev_at(ib)->kind == WD_RF;
    const uint16_t ca = coarse(a, slow ? 8 : 4), cb = coarse(b, slow ? 8 : 4);
    const int c = pop16(ca & cb), u = pop16(ca | cb);
    return c >= 3 && c * 4 >= u * 3;
}

static bool board_at(uint32_t now, float *x, float *z, float *odo)
{
    const wd_stats_t *s = wd_stats();
    *odo = s->dist_m;
    if (!s->origin || !s->fix.fix || !s->fix_ms || now - s->fix_ms > FRESH_MS) return false;
    *x = s->x;
    *z = s->z;
    return true;
}

static void step_move(void)
{
    const uint32_t n = (uint32_t)(wd_stats()->dist_m / WD_TRK_STEP_M);
    if (n <= step_odo_n) return;
    const uint32_t k = n - step_odo_n;
    step_odo_n = n;
    /* each vehicle's strip: the strongest member heard in the step that closed */
    for (int v = 0; v < nveh; v++) {
        wd_veh_t *w = &veh[v];
        int best = -128;
        for (int m = 0; m < w->n; m++) {
            if (w->idx[m] < 0) continue;
            const trk_t *d = &dv[w->idx[m]];
            const wd_dev_t *e = wd_dev_at(w->idx[m]);
            if (e && (bits_now(d) & 1) && e->last_rssi > best) best = e->last_rssi;
        }
        for (uint32_t i = 0; i < k && i < WD_VEH_TRAIL; i++) {
            memmove(w->trail, w->trail + 1, WD_VEH_TRAIL - 1);
            w->trail[WD_VEH_TRAIL - 1] = (int8_t)(i == 0 ? best : -128);
        }
    }
    step += k;
}

static void alert(int why, int idx, const wd_dev_t *e, uint16_t vnum, uint16_t tags, uint32_t now)
{
    wd_alert_t *a = &al[(al_head + al_n) % ALERTS];
    if (al_n == ALERTS) al_head = (al_head + 1) % ALERTS;   /* full: the oldest goes */
    else al_n++;
    memset(a, 0, sizeof(*a));
    a->why = (uint8_t)why;
    a->idx = (int16_t)idx;
    a->veh = vnum;
    a->tags = tags;
    a->ms = now;
    if (e) {
        a->kind = e->kind;
        memcpy(a->addr, e->addr, 6);
        a->sig = e->sig;
        a->rssi = e->last_rssi;
        memcpy(a->name, e->name, sizeof(a->name) - 1);
    }
    hits++;
    hit_ms = now;
}

static float follow_span(int kind)
{
    switch (kind) {
    case WD_WIFI: return 1500;
    case WD_BLE: case WD_RF: return 900;
    case WD_LORA: return 5000;
    default: return -1;         /* cells, aircraft, drones: never "with you" */
    }
}

/* ------------------------------------------------------------ hooks */

void wd_track_reset(void)
{
    if (!dv) dv = wd_feed_alloc(sizeof(trk_t) * WD_CAP);
    if (dv) memset(dv, 0, sizeof(trk_t) * WD_CAP);
    memset(veh, 0, sizeof(veh));
    nveh = 0;
    veh_next = 0;
    step = step_odo_n = 0;
    group_ms = 0;
    al_head = al_n = 0;
    hits = hit_ms = 0;
}

void wd_track_note(int idx, wd_dev_t *e, bool added, uint32_t now)
{
    if (!dv || idx < 0 || idx >= WD_CAP) return;
    trk_t *d = &dv[idx];
    if (added) {
        /* a reused slot: if it was in a vehicle, that member is gone */
        if (d->veh) {
            for (int v = 0; v < nveh; v++)
                for (int m = 0; m < veh[v].n; m++) if (veh[v].idx[m] == idx) veh[v].idx[m] = -1;
        }
        memset(d, 0, sizeof(*d));
    }
    step_move();
    d->bits = bits_now(d) | 1;
    d->at = step;
    float x, z, odo;
    if (board_at(now, &x, &z, &odo)) {
        if (!(d->flags & F_POS)) {
            d->fx = d->lx = x;
            d->fz = d->lz = z;
            d->places = 1;
            d->odo0 = odo;
            d->step0 = step;
            d->flags |= F_POS;
        } else {
            const float s = hypotf(x - d->fx, z - d->fz);
            if (s > d->span) d->span = s;
            if (hypotf(x - d->lx, z - d->lz) >= PLACE_M) {
                d->lx = x; d->lz = z;
                if (d->places < 255) d->places++;
            }
        }
        d->odo1 = odo;
    }
    /* a signature worth telling about, once per device */
    if (!(d->flags & F_ALERTED) && (e->tags & ~(WD_T_FOLLOW | WD_T_REGULAR | WD_T_NEWHERE | WD_T_PEER))) {
        const wd_sig_info_t *s = e->sig ? wd_sig_at(e->sig - 1) : NULL;
        if (!e->sig || (s && s->alert > 0)) {
            d->flags |= F_ALERTED;
            alert(WD_AL_SIG, idx, e, d->veh, e->tags, now);
        }
    }
    /* "with you" is the shared rule (ls_tail.h): its minutes, and its metres
       but never less than the kind's radio range; WARDRIVE adds the places
       and steps on top */
    if (!(e->tags & WD_T_FOLLOW)) {
        const float range = follow_span(e->kind);
        if (range > 0 && (d->flags & F_POS) && ls_tail_time_ok(now - e->first_ms) && d->span >= ls_tail_need_m(range) &&
            d->places >= FOLLOW_N && pop64(d->bits) >= FOLLOW_N) {
            e->tags |= WD_T_FOLLOW;
            alert(WD_AL_FOLLOW, idx, e, d->veh, e->tags, now);
        }
    }
}

/* ------------------------------------------------------------ vehicles */

static int veh_find(uint16_t num)
{
    for (int v = 0; v < nveh; v++) if (veh[v].num == num) return v;
    return -1;
}

static void veh_drop_member(int v, int m)
{
    wd_veh_t *w = &veh[v];
    for (int i = m; i + 1 < w->n; i++) {
        w->kind[i] = w->kind[i + 1];
        memcpy(w->addr[i], w->addr[i + 1], 6);
        w->idx[i] = w->idx[i + 1];
    }
    w->n--;
}

static void veh_remove(int v)
{
    for (int m = 0; m < veh[v].n; m++) if (veh[v].idx[m] >= 0) dv[veh[v].idx[m]].veh = 0;
    memmove(&veh[v], &veh[v + 1], sizeof(veh[0]) * (size_t)(nveh - v - 1));
    nveh--;
}

static int veh_new(uint32_t now)
{
    if (nveh == WD_VEH_MAX) {               /* full: the one gone longest goes */
        int old = 0;
        for (int v = 1; v < nveh; v++) if ((int32_t)(veh[v].last_ms - veh[old].last_ms) < 0) old = v;
        veh_remove(old);
    }
    wd_veh_t *w = &veh[nveh];
    memset(w, 0, sizeof(*w));
    w->num = ++veh_next;
    w->first_ms = w->last_ms = now;
    memset(w->trail, 0x80, sizeof(w->trail));
    return nveh++;
}

static void veh_join(int v, int idx)
{
    trk_t *d = &dv[idx];
    const wd_dev_t *e = wd_dev_at(idx);
    if (!e) return;
    if (d->veh == veh[v].num) return;
    if (d->veh) {                           /* moving over from another vehicle */
        const int o = veh_find(d->veh);
        if (o >= 0) {
            for (int m = 0; m < veh[o].n; m++) if (veh[o].idx[m] == idx) { veh_drop_member(o, m); break; }
            if (!veh[o].n) { veh_remove(o); if (o < v) v--; }
        }
    }
    wd_veh_t *w = &veh[v];
    if (w->n == WD_VEH_MEMBERS) return;
    w->kind[w->n] = e->kind;
    memcpy(w->addr[w->n], e->addr, 6);
    w->idx[w->n] = (int16_t)idx;
    w->n++;
    d->veh = w->num;
}

static int uf_find(int *p, int i) { while (p[i] != i) i = p[i] = p[p[i]]; return i; }

static void group(uint32_t now)
{
    int cand[CAND_MAX], nc = 0;
    uint64_t cb[CAND_MAX];
    const int nd = wd_dev_count();
    for (int i = 0; i < nd && nc < CAND_MAX; i++) {
        const wd_dev_t *e = wd_dev_at(i);
        const trk_t *d = &dv[i];
        if (!(d->flags & F_POS) || now - e->last_ms > CAND_GONE) continue;
        const uint64_t b = bits_now(d);
        const bool follow = e->tags & WD_T_FOLLOW;
        const bool car = (e->tags & WD_T_VEHICLE) || e->kind == WD_RF;
        if (!follow && !(car && pop64(b) >= 3 && d->span >= 300)) continue;
        cb[nc] = b;
        cand[nc++] = i;
    }
    int par[CAND_MAX];
    for (int i = 0; i < nc; i++) par[i] = i;
    for (int i = 0; i < nc; i++)
        for (int j = i + 1; j < nc; j++)
            if (together(cand[i], cb[i], cand[j], cb[j])) par[uf_find(par, i)] = uf_find(par, j);
    for (int i = 0; i < nc; i++) {
        if (uf_find(par, i) != i) continue;
        int mem[CAND_MAX], nm = 0;
        for (int j = 0; j < nc; j++) if (uf_find(par, j) == i) mem[nm++] = cand[j];
        if (nm == 1) {
            /* alone, it is a vehicle only if it is a car's own radio that follows */
            const wd_dev_t *e = wd_dev_at(mem[0]);
            if (!((e->tags & WD_T_FOLLOW) && (e->tags & WD_T_VEHICLE)) && !dv[mem[0]].veh) continue;
        }
        /* the number most of its members already carry */
        uint16_t num = 0;
        int best = 0;
        for (int a = 0; a < nm; a++) {
            const uint16_t n = dv[mem[a]].veh;
            if (!n) continue;
            int k = 0;
            for (int b = 0; b < nm; b++) if (dv[mem[b]].veh == n) k++;
            if (k > best || (k == best && n < num)) { best = k; num = n; }
        }
        int v = num ? veh_find(num) : -1;
        const bool fresh = v < 0;
        if (fresh) {
            if (nm < 2) {
                const wd_dev_t *e = wd_dev_at(mem[0]);
                if (!((e->tags & WD_T_FOLLOW) && (e->tags & WD_T_VEHICLE))) continue;
            }
            v = veh_new(now);
        }
        const uint16_t vn = veh[v].num;
        for (int a = 0; a < nm; a++) { veh_join(v, mem[a]); v = veh_find(vn); if (v < 0) break; }
        if (v < 0) continue;
        if (fresh) {
            const wd_dev_t *e = wd_dev_at(mem[0]);
            alert(WD_AL_VEHICLE, mem[0], e, vn, (uint16_t)(e->tags | WD_T_VEHICLE), now);
        }
    }
    /* each vehicle's figures from its members as they are now */
    for (int v = 0; v < nveh; v++) {
        wd_veh_t *w = &veh[v];
        w->tags = 0;
        float o0 = 1e30f, o1 = -1;
        for (int m = 0; m < w->n; m++) {
            const int k = w->idx[m];
            const wd_dev_t *e = k >= 0 ? wd_dev_at(k) : NULL;
            if (!e || e->kind != w->kind[m] || memcmp(e->addr, w->addr[m], 6)) { w->idx[m] = -1; continue; }
            w->tags |= e->tags;
            if ((int32_t)(e->first_ms - w->first_ms) < 0) w->first_ms = e->first_ms;
            if ((int32_t)(e->last_ms - w->last_ms) > 0) w->last_ms = e->last_ms;
            if (dv[k].odo0 < o0) o0 = dv[k].odo0;
            if (dv[k].odo1 > o1) o1 = dv[k].odo1;
        }
        if (o1 >= o0) w->with_m = o1 - o0;
    }
}

void wd_track_tick(uint32_t now)
{
    if (!dv) return;
    const uint32_t before = step;
    step_move();
    if (step == before && now - group_ms < GROUP_MS) return;
    group_ms = now;
    group(now);
}

/* ------------------------------------------------------------ queries */

bool wd_track_info(int idx, wd_trk_t *out)
{
    const wd_dev_t *e = wd_dev_at(idx);
    if (!dv || !e) return false;
    const trk_t *d = &dv[idx];
    memset(out, 0, sizeof(*out));
    out->first_ms = e->first_ms;
    out->last_ms = e->last_ms;
    out->span_m = d->span;
    out->with_m = d->odo1 > d->odo0 ? d->odo1 - d->odo0 : 0;
    out->places = d->places;
    out->steps = (uint8_t)pop64(bits_now(d));
    out->veh = d->veh;
    out->placed = d->flags & F_POS;
    out->first_x = d->fx;
    out->first_z = d->fz;
    return true;
}

int wd_track_vehicles(void) { return nveh; }
const wd_veh_t *wd_track_vehicle(int i) { return i >= 0 && i < nveh ? &veh[i] : NULL; }
const wd_veh_t *wd_track_vehicle_num(uint16_t num) { const int v = veh_find(num); return v >= 0 ? &veh[v] : NULL; }

int wd_track_following(void)
{
    int n = 0;
    const int nd = wd_dev_count();
    for (int i = 0; i < nd; i++) if (wd_dev_at(i)->tags & WD_T_FOLLOW) n++;
    return n;
}

uint32_t wd_track_steps(void) { return step; }

bool wd_track_alert(wd_alert_t *out)
{
    if (!al_n) return false;
    *out = al[al_head];
    al_head = (al_head + 1) % ALERTS;
    al_n--;
    return true;
}

uint32_t wd_track_hits(void) { return hits; }
uint32_t wd_track_hit_ms(void) { return hit_ms; }
