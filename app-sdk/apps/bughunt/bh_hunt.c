/* BUGHUNT's finder maths (see bh_hunt.h). Receive only; no board calls. */
#include "bh_hunt.h"
#include "bh_tags.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the level ---------------------------------------------------------------- */

void bh_lvl_init(bh_lvl_t *l, uint32_t window_ms)
{
    memset(l, 0, sizeof(*l));
    l->window_ms = window_ms ? window_ms : 1;
    l->peak = BH_HNONE;
}
int bh_lvl_now(const bh_lvl_t *l, uint32_t now)
{
    int best = BH_HNONE;
    for (int i = 0; i < l->n; i++) {
        const int k = (l->head - 1 - i + BH_LVL_N) % BH_LVL_N;
        if (now - l->t[k] > l->window_ms) break;        /* newest first: the rest are older */
        if (l->v[k] > best) best = l->v[k];
    }
    return best;
}
void bh_lvl_add(bh_lvl_t *l, int tenths, uint32_t now)
{
    if (tenths < -1500 || tenths > 300) return;
    l->v[l->head] = (int16_t)tenths;
    l->t[l->head] = now;
    l->head = (l->head + 1) % BH_LVL_N;
    if (l->n < BH_LVL_N) l->n++;
    l->last_ms = now;
    if (l->peak == BH_HNONE || tenths > l->peak) l->peak = (int16_t)tenths;
    const float w = (float)bh_lvl_now(l, now);
    if (!l->started) { l->fast = l->slow = w; l->started = true; return; }
    l->fast += (w - l->fast) * 0.35f;
    l->slow += (w - l->slow) * 0.05f;
}
int bh_lvl_trend(const bh_lvl_t *l, int db)
{
    if (!l->started) return 0;
    const float d = l->fast - l->slow;
    return d > (float)db * 10.0f ? 1 : d < -(float)db * 10.0f ? -1 : 0;
}

/* ---- the meter's range ---------------------------------------------------------- */

void bh_range_init(bh_range_t *r, int base, int span, int max_step)
{
    memset(r, 0, sizeof(*r));
    r->base = (int16_t)base;
    r->span = (int16_t)(span > 100 ? span : 100);
    r->max_step = (uint8_t)(max_step < 0 ? 0 : max_step);
}
int bh_range_lo(const bh_range_t *r) { return r->base + r->step * 200; }
int bh_range_hi(const bh_range_t *r) { return bh_range_lo(r) + r->span; }
float bh_range_frac(const bh_range_t *r, float tenths)
{
    const float f = (tenths - (float)bh_range_lo(r)) / (float)r->span;
    return f < 0 ? 0 : f > 1 ? 1 : f;
}
int bh_range_feed(bh_range_t *r, int tenths, uint32_t now)
{
    if (tenths == BH_HNONE) { r->over_ms = r->under_ms = 0; return 0; }
    const float f = bh_range_frac(r, (float)tenths);
    if (f >= 0.95f && r->step < r->max_step) {
        r->under_ms = 0;
        if (!r->over_ms) r->over_ms = now ? now : 1;
        if (now - r->over_ms >= 600u) { r->step++; r->over_ms = 0; return 1; }
        return 0;
    }
    r->over_ms = 0;
    /* down only when the level sits in the bottom quarter, and the range
       below would not be pinned straight away */
    if (r->step > 0 && f < 0.25f) {
        if (!r->under_ms) r->under_ms = now ? now : 1;
        if (now - r->under_ms >= 3000u) { r->step--; r->under_ms = 0; return -1; }
        return 0;
    }
    r->under_ms = 0;
    return 0;
}

/* ---- how far a tag is ------------------------------------------------------------- */

int bh_dist_ref(int cls, bool has_tx, int tx_power)
{
    if (has_tx && tx_power > -40 && tx_power < 21) return tx_power - 41;
    switch (cls) {
    case BW_C_FINDMY: return -60;       /* AirTag on a phone-class receiver at 1 m */
    case BW_C_SMARTTAG: return -61;
    case BW_C_TILE: return -63;
    case BW_C_FMDN: case BW_C_DULT: return -62;
    case BW_C_CHIPOLO: return -62;
    default: return -60;
    }
}
float bh_dist_m(int rssi, int ref, float n)
{
    if (n < 1.5f) n = 1.5f;
    float m = powf(10.0f, (float)(ref - rssi) / (10.0f * n));
    if (m < 0.1f) m = 0.1f;
    if (m > 200.0f) m = 200.0f;
    return m;
}
static void metres(float m, char *out, int cap)
{
    if (m < 1.0f) snprintf(out, (size_t)cap, "%.1f", (double)m);
    else snprintf(out, (size_t)cap, "%d", (int)(m + 0.5f));
}
void bh_dist_text(int rssi, int ref, char *out, int cap)
{
    /* 2.2: a room with people and furniture; a 6 dB fade either way is the span */
    const float m = bh_dist_m(rssi, ref, 2.2f), lo = bh_dist_m(rssi + 6, ref, 2.2f), hi = bh_dist_m(rssi - 6, ref, 2.2f);
    char a[8], b[8], c[8];
    metres(m, a, sizeof(a));
    metres(lo, b, sizeof(b));
    metres(hi, c, sizeof(c));
    snprintf(out, (size_t)cap, "~%s m (%s-%s)", a, b, c);
}

/* ---- direction ------------------------------------------------------------------ */

void bh_dir2_reset(bh_dir2_t *d)
{
    for (int i = 0; i < BH_DIR2; i++) { d->val[i] = BH_HNONE; d->hits[i] = 0; }
    d->lo = d->hi = BH_HNONE;
}
void bh_dir2_add(bh_dir2_t *d, float hd, int tenths)
{
    if (!(hd >= 0.0f) || tenths < -1500) return;
    int b = (int)(hd / (360.0f / BH_DIR2) + 0.5f) % BH_DIR2;
    if (b < 0) b += BH_DIR2;
    /* the first reading sets it; later ones pull it, the stronger ones harder
       (a fade is a dip, so a dip moves the bin less than a peak does) */
    if (d->val[b] == BH_HNONE) d->val[b] = (int16_t)tenths;
    else {
        const float a = tenths > d->val[b] ? 0.5f : 0.2f;
        d->val[b] = (int16_t)((float)d->val[b] + ((float)tenths - (float)d->val[b]) * a);
    }
    if (d->hits[b] < 0xFFFF) d->hits[b]++;
    d->lo = d->hi = BH_HNONE;
    for (int i = 0; i < BH_DIR2; i++) {
        if (d->val[i] == BH_HNONE) continue;
        if (d->lo == BH_HNONE || d->val[i] < d->lo) d->lo = d->val[i];
        if (d->hi == BH_HNONE || d->val[i] > d->hi) d->hi = d->val[i];
    }
}
int bh_dir2_bin(const bh_dir2_t *d, int b)
{
    b = ((b % BH_DIR2) + BH_DIR2) % BH_DIR2;
    if (d->val[b] == BH_HNONE) return BH_HNONE;
    float s = 2.0f * (float)d->val[b], w = 2.0f;
    const int l = (b + BH_DIR2 - 1) % BH_DIR2, r = (b + 1) % BH_DIR2;
    if (d->val[l] != BH_HNONE) { s += d->val[l]; w += 1.0f; }
    if (d->val[r] != BH_HNONE) { s += d->val[r]; w += 1.0f; }
    return (int)(s / w + (s < 0 ? -0.5f : 0.5f));
}
static int cmp16(const void *a, const void *b) { return *(const int16_t *)a - *(const int16_t *)b; }

bh_dir_fix_t bh_dir2_fix(const bh_dir2_t *d, int mode)
{
    bh_dir_fix_t f = { -1, 0, 0, 0, 0 };
    int16_t v[BH_DIR2];
    int nv = 0, best_b = -1, best_v = 0;
    for (int b = 0; b < BH_DIR2; b++) {
        const int x = bh_dir2_bin(d, b);
        if (x == BH_HNONE) continue;
        v[nv++] = (int16_t)x;
        const bool better = best_b < 0 || (mode == BH_DM_BODY ? x < best_v : x > best_v);
        if (better) { best_b = b; best_v = x; }
    }
    f.covered = nv;
    if (nv < 6 || best_b < 0) return f;     /* a sixth of the circle at least */
    qsort(v, (size_t)nv, sizeof(v[0]), cmp16);
    const int med = v[nv / 2];
    f.contrast = mode == BH_DM_BODY ? med - best_v : best_v - med;
    /* the lobe: bins either side within 3 dB of the best, through unvisited ones */
    int wl = 0, wr = 0;
    for (int k = 1; k < BH_DIR2 / 2; k++) {
        const int x = bh_dir2_bin(d, best_b - k);
        if (x == BH_HNONE || (mode == BH_DM_BODY ? x - best_v : best_v - x) > 30) break;
        wl = k;
    }
    for (int k = 1; k < BH_DIR2 / 2; k++) {
        const int x = bh_dir2_bin(d, best_b + k);
        if (x == BH_HNONE || (mode == BH_DM_BODY ? x - best_v : best_v - x) > 30) break;
        wr = k;
    }
    f.width = (wl + wr + 1) * (360 / BH_DIR2);
    /* the middle of the lobe, not its first bin */
    int centre = (best_b * (360 / BH_DIR2)) + (wr - wl) * (360 / BH_DIR2) / 2;
    if (mode == BH_DM_BODY) centre += 180;      /* your back faced it: it is behind you */
    f.bearing = ((centre % 360) + 360) % 360;
    f.quality = f.contrast >= 80 && f.width <= 90 ? 3 : f.contrast >= 40 ? 2 : 1;
    return f;
}
int bh_turn(int facing, int bearing)
{
    int t = (bearing - facing) % 360;
    if (t > 180) t -= 360;
    if (t <= -180) t += 360;
    return t;
}

/* ---- bursts --------------------------------------------------------------------- */

void bh_bursts_init(bh_bursts_t *b, int floor_tenths)
{
    memset(b, 0, sizeof(*b));
    b->thr = (int16_t)(floor_tenths + 100);     /* 10 dB over the quiet level */
}
static void burst_end(bh_bursts_t *b)
{
    b->in = false;
    if (b->n == BH_BURSTS) { memmove(b->b, b->b + 1, sizeof(b->b[0]) * (BH_BURSTS - 1)); b->n--; }
    b->b[b->n++] = b->cur;
    b->total++;
    /* the spacing: a period most intervals fit, missed check-ins allowed */
    if (b->n >= 3) {
        uint32_t best = 0;
        int best_fit = 0;
        for (int i = 1; i < b->n; i++) {
            const uint32_t cand = b->b[i].t_ms - b->b[i - 1].t_ms;
            if (cand < 2000u) continue;
            int fit = 0;
            for (int j = 1; j < b->n; j++) {
                const uint32_t gap = b->b[j].t_ms - b->b[j - 1].t_ms;
                const uint32_t k = (gap + cand / 2) / cand;
                if (!k || k > 3) continue;
                const int32_t err = (int32_t)gap - (int32_t)(k * cand);
                if ((err < 0 ? -err : err) <= (int32_t)(cand / 8 + 1500)) fit++;
            }
            if (fit > best_fit || (fit == best_fit && cand < best)) { best_fit = fit; best = cand; }
        }
        b->period_ms = best_fit * 10 >= (b->n - 1) * 7 ? best : 0;
    }
}
bool bh_bursts_add(bh_bursts_t *b, int tenths, int heading, uint32_t now)
{
    if (tenths >= b->thr) {
        if (!b->in) {
            b->in = true;
            b->cur.t_ms = now; b->cur.dur_ms = 0;
            b->cur.peak = (int16_t)tenths; b->cur.heading = (int16_t)heading;
        } else {
            b->cur.dur_ms = now - b->cur.t_ms;
            if (tenths > b->cur.peak) { b->cur.peak = (int16_t)tenths; b->cur.heading = (int16_t)heading; }
        }
        return false;
    }
    b->quiet_ms = now;
    if (b->in) { burst_end(b); return true; }
    return false;
}
bool bh_bursts_tick(bh_bursts_t *b, uint32_t now)
{
    /* no readings under the threshold for a while but nothing over it either:
       the target went silent, which ends the burst too */
    if (b->in && now - (b->cur.t_ms + b->cur.dur_ms) > 400u) { burst_end(b); return true; }
    return false;
}
int32_t bh_bursts_due(const bh_bursts_t *b, uint32_t now)
{
    if (!b->period_ms || !b->n) return -1;
    const uint32_t next = b->b[b->n - 1].t_ms + b->period_ms;
    return now >= next ? 0 : (int32_t)(next - now);
}
int bh_bursts_delta(const bh_bursts_t *b)
{
    return b->n >= 2 ? b->b[b->n - 1].peak - b->b[b->n - 2].peak : 0;
}
bool bh_bursts_bursty(const bh_bursts_t *b, uint32_t now)
{
    /* two bursts at least, and the last one was short */
    return b->n >= 2 && !b->in && b->b[b->n - 1].dur_ms < 5000u && now - b->quiet_ms < 5000u;
}

/* ---- two receivers -------------------------------------------------------------- */

void bh_pair_init(bh_pair_t *p, uint32_t window_ms)
{
    memset(p, 0, sizeof(*p));
    bh_lvl_init(&p->a, window_ms);
    bh_lvl_init(&p->b, window_ms);
    p->floor_a = p->floor_b = BH_HNONE;
    p->best = -1000.0f;
}
void bh_pair_floor(bh_pair_t *p, int fa, int fb)
{
    p->floor_a = (int16_t)fa;
    p->floor_b = (int16_t)fb;
}
void bh_pair_add(bh_pair_t *p, int which, int tenths, uint32_t now)
{
    bh_lvl_add(which ? &p->b : &p->a, tenths, now);
}
int bh_pair_point(bh_pair_t *p, uint32_t now)
{
    const int a = bh_lvl_now(&p->a, now), b = bh_lvl_now(&p->b, now);
    if (a == BH_HNONE || b == BH_HNONE || p->floor_a == BH_HNONE || p->floor_b == BH_HNONE) return BH_HNONE;
    /* only while the reference hears it: a difference of two noise floors is no answer */
    if (b - p->floor_b < 60) return BH_HNONE;
    const float d = (float)((a - p->floor_a) - (b - p->floor_b));
    if (!p->have) { p->diff = d; p->have = true; }
    else p->diff += (d - p->diff) * 0.3f;
    if (p->diff > p->best) p->best = p->diff;
    return (int)p->diff;
}
