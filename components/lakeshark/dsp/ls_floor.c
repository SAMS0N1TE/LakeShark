#include "ls_floor.h"

#include <math.h>
#include <string.h>

/* While a bin is over its gate its floor moves at this share of the step: a
   carrier that stays on is learned over minutes, not seconds. */
#define HELD_SHARE  0.02f
/* How fast the scatter estimate follows, a frame. */
#define DEV_FOLLOW  0.02f
/* Mean absolute deviation to one standard deviation, for Gaussian scatter. */
#define MAD_TO_SD   1.2533f
/* How many learning periods the floor takes to settle after it. */
#define SETTLE_LEARNS 4
/* How much noisier than the row a bin must be for its own scatter to set
   its gate. */
#define OWN_SHARE   1.5f

const ls_floor_cfg_t LS_FLOOR_IQ    = { .step_db = 0.10f, .learn = 16, .dwell = 2,
                                        .hyst_db = 3.0f, .dev_k = 3.0f };
const ls_floor_cfg_t LS_FLOOR_SWEEP = { .step_db = 0.12f, .learn = 8, .dwell = 1,
                                        .hyst_db = 3.0f, .dev_k = 6.5f };

static float median_of(ls_floor_t *f, const float *src, int n);

void ls_floor_reset(ls_floor_t *f)
{
    if (!f) return;
    f->n = 0;
    f->frames = 0;
    f->pool = 0.0f;
    memset(f->base, 0, sizeof(f->base));
    memset(f->dev, 0, sizeof(f->dev));
    memset(f->run, 0, sizeof(f->run));
    memset(f->hit, 0, sizeof(f->hit));
}

void ls_floor_init(ls_floor_t *f, const ls_floor_cfg_t *cfg)
{
    if (!f) return;
    f->cfg = cfg ? *cfg : LS_FLOOR_SWEEP;
    if (f->cfg.learn < 1) f->cfg.learn = 1;
    if (f->cfg.learn > LS_FLOOR_LEARN_MAX) f->cfg.learn = LS_FLOOR_LEARN_MAX;
    if (f->cfg.dwell < 1) f->cfg.dwell = 1;
    if (f->cfg.dwell > 255) f->cfg.dwell = 255;
    if (!(f->cfg.step_db > 0.0f)) f->cfg.step_db = 0.1f;
    if (f->cfg.hyst_db < 0.0f) f->cfg.hyst_db = 0.0f;
    if (f->cfg.dev_k < 0.0f) f->cfg.dev_k = 0.0f;
    ls_floor_reset(f);
}

bool ls_floor_ready(const ls_floor_t *f)
{
    return f && f->n > 0 && f->frames >= (uint32_t)f->cfg.learn;
}

static void sort_small(float *v, int n)
{
    for (int i = 1; i < n; i++) {
        const float k = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > k) { v[j + 1] = v[j]; j--; }
        v[j + 1] = k;
    }
}

static float gate_of(const ls_floor_t *f, int i, float gate_db, bool settling)
{
    float g = gate_db < 1.0f ? 1.0f : gate_db;
    /* The row's scatter, which a few hundred bins pin down; a bin's own
       only where it is clearly noisier (a spur that wanders, an edge). One
       bin's estimate alone comes out a third high or low by chance, and
       that bin then misses a real burst or fires on an ordinary wiggle. */
    float dev = f->pool;
    if (f->dev[i] > OWN_SHARE * dev) dev = f->dev[i];
    float noise = f->cfg.dev_k * dev;
    /* Until the floor has settled, the scatter is a guess from a few frames
       and the floor is a few dB off either way: hold the gate wider. */
    if (settling) noise *= 1.5f;
    return noise > g ? noise : g;
}

static bool settling(const ls_floor_t *f)
{
    return f->frames < (uint32_t)(SETTLE_LEARNS * f->cfg.learn);
}

float ls_floor_gate(const ls_floor_t *f, int i, float gate_db)
{
    return gate_of(f, i, gate_db, settling(f));
}

/* The median of what the bin read while learning: a burst in one or two of
   the frames moves it by a reading's rank, not by the burst's size. */
static void seed_bin(ls_floor_t *f, int i, int have)
{
    float v[LS_FLOOR_LEARN_MAX];
    for (int k = 0; k < have; k++) v[k] = f->seed[k][i];
    sort_small(v, have);
    const float b = have & 1 ? v[have / 2] : 0.5f * (v[have / 2 - 1] + v[have / 2]);
    f->base[i] = b;
    float dev = 0.0f;
    for (int k = 0; k < have; k++) dev += fabsf(v[k] - b);
    f->dev[i] = dev / (float)have;
}

int ls_floor_update(ls_floor_t *f, const float *db, int n, float gate_db)
{
    if (!f || !db || n <= 0) return 0;
    if (n > LS_FLOOR_BINS_MAX) n = LS_FLOOR_BINS_MAX;
    if (n != f->n) { ls_floor_reset(f); f->n = n; }

    const int learn = f->cfg.learn;
    if (f->frames < (uint32_t)learn) {
        const int k = (int)f->frames;
        for (int i = 0; i < n; i++) f->seed[k][i] = db[i];
        f->frames++;
        /* The floor so far, so a view drawn while learning is already flat. */
        for (int i = 0; i < n; i++) seed_bin(f, i, k + 1);
        return 0;
    }

    f->pool = median_of(f, f->dev, n);
    /* Eight frames place a median to within a few dB, not a tenth of one:
       for as long again as it took to learn, the floor moves four times as
       fast, then settles to its own pace. */
    const float step = f->cfg.step_db *
                       (f->frames < (uint32_t)(SETTLE_LEARNS * learn) ? 4.0f : 1.0f);
    int rising = 0;
    for (int i = 0; i < n; i++) {
        const float x = db[i];
        const float over = x - f->base[i];
        const float g = ls_floor_gate(f, i, gate_db);
        /* What the floor stops for is the plain gate: a carrier that comes
           up while the hit gate is still held wide must not be learned. */
        const float g_floor = gate_of(f, i, gate_db, false);

        if (over >= g) {
            if (f->run[i] < 255) f->run[i]++;
            if (!f->hit[i] && f->run[i] >= f->cfg.dwell) { f->hit[i] = true; rising++; }
        } else {
            f->run[i] = 0;
            if (f->hit[i] && over < g - f->cfg.hyst_db) f->hit[i] = false;
        }

        const bool busy = f->hit[i] || over >= g_floor;
        if (busy) {
            /* Barely moves under a signal; never chases it down. */
            if (x > f->base[i]) f->base[i] += step * HELD_SHARE;
        } else {
            /* A median tracker: equal steps up and down settle where half
               the readings are above. A reading closer than a step lands. */
            /* One wild reading moves the estimate by a bounded amount. */
            float a = fabsf(over);
            const float cap = 4.0f * (f->pool > 0.5f ? f->pool : 0.5f);
            if (a > cap) a = cap;
            f->dev[i] += (a - f->dev[i]) * DEV_FOLLOW;
            if (over > step) f->base[i] += step;
            else if (over < -step) f->base[i] -= step;
            else f->base[i] = x;
        }
    }
    f->frames++;
    return rising;
}

/* Sorted in the tracker's own scratch: the sweep runs on a task whose stack
   is not the place for another kilobyte. */
static float median_of(ls_floor_t *f, const float *src, int n)
{
    if (n <= 0) return 0.0f;
    float *v = f->scratch;
    memcpy(v, src, sizeof(float) * (size_t)n);
    sort_small(v, n);
    return v[n / 2];
}

float ls_floor_median_base(ls_floor_t *f)
{
    return f ? median_of(f, f->base, f->n) : 0.0f;
}

float ls_floor_median_spread(ls_floor_t *f)
{
    return f ? median_of(f, f->dev, f->n) * MAD_TO_SD : 0.0f;
}

void ls_floor_view(ls_floor_t *f, const float *db, float *out, int n,
                   float pad_db, float window_db, float edge_db)
{
    if (!f || !db || !out || n <= 0) return;
    if (n > f->n) n = f->n;
    if (window_db < 1.0f) window_db = 1.0f;
    /* The passband's floor: the upper quartile of the floors, which a
       filter's flat top occupies and its skirts do not. */
    float band = 0.0f;
    if (edge_db > 0.0f) {
        float *v = f->scratch;
        memcpy(v, f->base, sizeof(float) * (size_t)n);
        sort_small(v, n);
        band = v[(3 * n) / 4];
    }
    for (int i = 0; i < n; i++) {
        float v = (db[i] - f->base[i] + pad_db) / window_db;
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        if (edge_db > 0.0f && f->base[i] < band - edge_db) v *= 0.5f;
        out[i] = v;
    }
}

int ls_floor_iq_row(ls_floor_t *f, float *bins, int n, float gate_db)
{
    if (!f || !bins || n <= 0) return 0;
    if (n > LS_FLOOR_BINS_MAX) n = LS_FLOOR_BINS_MAX;
    float *db = f->row;
    for (int i = 0; i < n; i++) db[i] = bins[i] * 100.0f - 100.0f;
    const int rising = ls_floor_update(f, db, n, gate_db);
    ls_floor_view(f, db, bins, n, LS_FLOOR_IQ_PAD_DB, LS_FLOOR_IQ_WINDOW_DB, LS_FLOOR_IQ_EDGE_DB);
    return rising;
}
