/* See ls_search_core.h. Pure arithmetic; no radio, no ESP-IDF. */

#include "ls_search_core.h"
#include "ls_sweep_core.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Below this a floor has not been learned yet (a bin with no reading when the
   floors were primed). The first real reading becomes its floor. */
#define NO_FLOOR       (-200.0f)
#define NO_FLOOR_LIMIT (-150.0f)

#define RUN_STRONG     0x80u    /* in run[]: over STRONG_DB on this pass */
#define RUN_COUNT      0x7fu

bool ls_search_plan(uint64_t lo_hz, uint64_t hi_hz, ls_sweep_plan_t *out)
{
    uint32_t dc = (uint32_t)((uint64_t)LS_SEARCH_DC_BINS * LS_SEARCH_RATE_HZ /
                             LS_SEARCH_FFT_N);
    return ls_sweep_plan_overlap(lo_hz, hi_hz, LS_SEARCH_BIN_HZ,
                                 LS_SEARCH_RATE_HZ, LS_SEARCH_USABLE_PCT, dc,
                                 LS_SEARCH_HOP_HZ, out);
}

void ls_search_init(ls_search_t *s, uint64_t start_hz, uint32_t bin_hz,
                    uint32_t n_bins, void *mem)
{
    if (!s) return;
    memset(s, 0, sizeof(*s));
    s->start_hz = start_hz;
    s->bin_hz   = bin_hz;
    s->n_bins   = n_bins;
    if (mem) {
        s->floor = (float *)mem;
        s->run   = (uint8_t *)(s->floor + n_bins);
        s->hist  = s->run + n_bins;
        s->looks = s->hist + n_bins;
    }
    ls_search_reset(s);
}

void ls_search_reset(ls_search_t *s)
{
    if (!s) return;
    s->primed  = false;
    s->pass    = 0;
    s->n_hits  = 0;
    s->dropped = 0;
    memset(s->hit, 0, sizeof(s->hit));
    if (!s->floor) return;
    for (uint32_t i = 0; i < s->n_bins; i++) s->floor[i] = NO_FLOOR;
    memset(s->run,   0, s->n_bins);
    memset(s->hist,  0, s->n_bins);
    memset(s->looks, 0, s->n_bins);
}

const char *ls_search_path_name(ls_search_path_t path)
{
    return path == LS_SEARCH_PATH_STRONG ? "strong" : "steady";
}

uint64_t ls_search_snap(uint64_t hz)
{
    return ((hz + LS_SEARCH_RASTER_HZ / 2u) / LS_SEARCH_RASTER_HZ) *
           LS_SEARCH_RASTER_HZ;
}

float ls_search_duty(const ls_search_t *s, const ls_search_hit_t *h)
{
    if (!s || !h || s->pass < h->first_pass) return 0.0f;
    float d = (float)h->count / (float)(s->pass - h->first_pass + 1u);
    return d > 1.0f ? 1.0f : d;
}

uint32_t ls_search_n_constant(const ls_search_t *s)
{
    uint32_t n = 0;
    if (s) for (uint32_t i = 0; i < s->n_hits; i++) if (s->hit[i].constant) n++;
    return n;
}

static uint32_t popcount8(uint8_t v)
{
    uint32_t n = 0;
    for (; v; v &= (uint8_t)(v - 1u)) n++;
    return n;
}

/* Over HIT_DB on at least CONST_PCT of the looks it has had, up to the last
   HISTORY of them. Called while looks[] still counts the looks BEFORE this
   one, whose bit is already in hist[]. */
static bool bin_constant(const ls_search_t *s, uint32_t i)
{
    if (s->looks[i] < LS_SEARCH_WARMUP) return false;
    uint32_t n = (uint32_t)s->looks[i] + 1u;
    if (n > LS_SEARCH_HISTORY) n = LS_SEARCH_HISTORY;
    uint8_t mask = n >= 8 ? 0xffu : (uint8_t)((1u << n) - 1u);
    return popcount8(s->hist[i] & mask) * 100u >= n * LS_SEARCH_CONST_PCT;
}

/* Each block of bins starts at its own median. Insertion sort: the block is
   small and this runs once per search. */
static void prime_floors(ls_search_t *s, const int8_t *dbfs)
{
    float blk[LS_SEARCH_INIT_BLOCK];

    for (uint32_t a = 0; a < s->n_bins; a += LS_SEARCH_INIT_BLOCK) {
        uint32_t b = a + LS_SEARCH_INIT_BLOCK;
        if (b > s->n_bins) b = s->n_bins;

        uint32_t m = 0;
        for (uint32_t i = a; i < b; i++) {
            if (dbfs[i] == LS_SWEEP_NO_DATA) continue;
            float x = (float)dbfs[i];
            uint32_t j = m++;
            for (; j > 0 && blk[j - 1] > x; j--) blk[j] = blk[j - 1];
            blk[j] = x;
        }
        float med = m ? blk[m / 2] : NO_FLOOR;
        for (uint32_t i = a; i < b; i++) s->floor[i] = med;
    }
    s->primed = true;
}

/* Bins [a, b) are over HIT_DB and touch: one signal. Its centre is the
   power-weighted centroid, so a signal straddling two bins lands between them
   rather than on whichever was louder. `strong` says a bin of it is over
   STRONG_DB, which is the rule that makes a new hit of it; `constant` that a
   bin of it is a spur. */
static void take_cluster(ls_search_t *s, uint32_t a, uint32_t b, bool strong,
                         bool constant, const int8_t *dbfs, uint32_t now_s,
                         ls_search_hit_fn fn, void *user, uint32_t *n_new)
{
    float wsum = 0.0f, moment = 0.0f, peak = 0.0f;
    for (uint32_t k = a; k < b; k++) {
        /* A quiet bin inside the cluster, or one that has no reading, weighs
           what it weighs: next to nothing. */
        if (dbfs[k] == LS_SWEEP_NO_DATA || s->floor[k] < NO_FLOOR_LIMIT) continue;
        float over = (float)dbfs[k] - s->floor[k];
        if (over > peak) peak = over;
        float w = powf(10.0f, over * 0.1f);
        wsum   += w;
        moment += w * (float)(k - a);
    }

    /* Offset in bins from the first bin keeps the arithmetic small, so float
       is enough: a frequency in float would be good to 128 Hz at 170 MHz. */
    uint64_t first = s->start_hz + (uint64_t)s->bin_hz * a + s->bin_hz / 2u;
    uint64_t hz = ls_search_snap(first + (uint64_t)(moment / wsum *
                                                    (float)s->bin_hz + 0.5f));

    int      best   = -1;
    uint64_t best_d = LS_SEARCH_MATCH_HZ;           /* strictly closer than */
    for (uint32_t i = 0; i < s->n_hits; i++) {
        uint64_t f = s->hit[i].freq_hz;
        uint64_t d = f > hz ? f - hz : hz - f;
        if (d < best_d) { best_d = d; best = (int)i; }
    }

    if (best >= 0) {
        ls_search_hit_t *h = &s->hit[best];
        /* Two pieces of one signal in a single pass: still one look. */
        if (h->last_pass == s->pass) {
            if (peak > h->max_db) h->max_db = peak;
            if (constant) h->constant = true;
            return;
        }
        /* Constant stays constant while it is there, however the last look
           fell: a spur riding the threshold as its floor creeps up is still
           a spur. After a gap it is judged afresh, and a spur that was
           learned and then keys up like a signal is news again. */
        bool gap   = s->pass - h->last_pass > LS_SEARCH_AGAIN_GAP;
        h->constant = gap ? constant : (h->constant || constant);
        bool again = gap && !h->constant;
        h->count++;
        h->last_pass = s->pass;
        h->last_s    = now_s;
        /* The strongest look is the best measurement of where it is. */
        if (peak > h->max_db) { h->max_db = peak; h->freq_hz = (uint32_t)hz; }
        if (again && fn) fn(h, LS_SEARCH_HIT_AGAIN, user);
        return;
    }

    uint32_t slot;
    if (s->n_hits < LS_SEARCH_MAX_HITS) {
        slot = s->n_hits++;
    } else {
        /* Full: the signal seen the fewest times goes, constants last, the
           stalest of equals. */
        slot = 0;
        for (uint32_t i = 1; i < s->n_hits; i++) {
            const ls_search_hit_t *c = &s->hit[i], *w = &s->hit[slot];
            if (c->constant != w->constant ? !c->constant :
                c->count != w->count ? c->count < w->count :
                c->last_pass < w->last_pass)
                slot = i;
        }
        /* A constant newcomer does not push out anything that is already
           there: that is what turned a table of spurs over every pass. */
        if (constant && s->hit[slot].constant) { s->dropped++; return; }
    }
    ls_search_hit_t *h = &s->hit[slot];
    h->freq_hz    = (uint32_t)hz;
    h->path       = strong ? LS_SEARCH_PATH_STRONG : LS_SEARCH_PATH_STEADY;
    h->constant   = constant;
    h->max_db     = peak;
    h->count      = 1;
    h->first_s    = h->last_s    = now_s;
    h->first_pass = h->last_pass = s->pass;
    (*n_new)++;
    if (fn) fn(h, LS_SEARCH_HIT_NEW, user);
}

uint32_t ls_search_feed(ls_search_t *s, const int8_t *dbfs, uint32_t now_s,
                        ls_search_hit_fn fn, void *user)
{
    if (!s || !dbfs || !s->floor || s->n_bins == 0) return 0;

    s->pass++;
    if (!s->primed) prime_floors(s, dbfs);

    for (uint32_t i = 0; i < s->n_bins; i++) {
        s->run[i] &= (uint8_t)~RUN_STRONG;
        if (dbfs[i] == LS_SWEEP_NO_DATA) { s->run[i] = 0; continue; }
        float x = (float)dbfs[i];

        if (s->floor[i] < NO_FLOOR_LIMIT) {     /* nothing to compare with yet */
            s->floor[i] = x;
            s->run[i]   = 0;
            s->hist[i]  = (uint8_t)(s->hist[i] << 1);
            continue;
        }

        float over = x - s->floor[i];
        bool  warm = s->looks[i] >= LS_SEARCH_WARMUP;
        bool  hot  = warm && over >= LS_SEARCH_HIT_DB;
        s->hist[i] = (uint8_t)((s->hist[i] << 1) | (hot ? 1u : 0u));

        if (!hot) {
            /* A running mean while the floor is young - of everything, hot or
               not, so what is there from the start is the baseline - and
               1/16 once it is not. */
            float a = 1.0f / ((float)s->looks[i] + 2.0f);
            if (a < LS_SEARCH_FLOOR_ALPHA) a = LS_SEARCH_FLOOR_ALPHA;
            s->floor[i] += a * over;
            s->run[i] = 0;
        } else {
            /* Over: the floor is left alone until the clusters below have
               been judged, so strong means strong against what it was. */
            uint8_t n = s->run[i] & RUN_COUNT;
            if (n < RUN_COUNT) n++;
            s->run[i] = (uint8_t)(n | (over >= LS_SEARCH_STRONG_DB ? RUN_STRONG : 0u));
        }
    }

    /* run & RUN_COUNT > 0 is a bin over HIT_DB on this pass. A run of them is
       a hit when one of its bins has been over for LS_SEARCH_LOOKS passes or
       is over STRONG_DB now; the rest of the run is the skirts of the same
       signal. */
    uint32_t n_new = 0, i = 0;
    while (i < s->n_bins) {
        if ((s->run[i] & RUN_COUNT) == 0) { i++; continue; }
        uint32_t a = i;
        bool strong = false, steady = false, constant = false;
        for (;;) {
            for (; i < s->n_bins && (s->run[i] & RUN_COUNT) > 0; i++) {
                if ((s->run[i] & RUN_COUNT) >= LS_SEARCH_LOOKS) steady = true;
                if (s->run[i] & RUN_STRONG) strong = true;
                if (bin_constant(s, i)) constant = true;
            }
            /* One quiet bin between two over ones: their centres are closer
               than LS_SEARCH_MATCH_HZ, so it is one signal (the dip of a wide
               one, POCSAG). Two quiet bins are a gap. */
            if (2u * s->bin_hz < LS_SEARCH_MATCH_HZ &&
                i + 1 < s->n_bins && (s->run[i + 1] & RUN_COUNT) > 0) {
                i++;
                continue;
            }
            break;
        }
        if (strong || steady)
            take_cluster(s, a, i, strong, constant, dbfs, now_s, fn, user,
                         &n_new);
    }

    /* Now the floors that were held, and the age of every floor. A bin that is
       over still moves it, slowly: that is how a spur is learned. */
    for (uint32_t k = 0; k < s->n_bins; k++) {
        if (dbfs[k] == LS_SWEEP_NO_DATA) continue;
        if (s->hist[k] & 1u)
            s->floor[k] += LS_SEARCH_FLOOR_ALPHA_OVER *
                           ((float)dbfs[k] - s->floor[k]);
        if (s->looks[k] < 255) s->looks[k]++;
    }
    return n_new;
}

uint32_t ls_search_top(const ls_search_t *s, uint32_t *idx, uint32_t max)
{
    if (!s || !idx || max == 0) return 0;

    bool taken[LS_SEARCH_MAX_HITS] = { false };
    uint32_t n = 0;
    while (n < max && n < s->n_hits) {
        int best = -1;
        for (uint32_t i = 0; i < s->n_hits; i++) {
            if (taken[i]) continue;
            if (best >= 0) {
                const ls_search_hit_t *c = &s->hit[i], *w = &s->hit[best];
                bool better = c->constant != w->constant ? !c->constant :
                              c->count != w->count ? c->count > w->count :
                              c->max_db > w->max_db;
                if (!better) continue;
            }
            best = (int)i;
        }
        taken[best] = true;
        idx[n++] = (uint32_t)best;
    }
    return n;
}

static uint8_t enc_db(float db)
{
    float v = (db + 100.0f) * 2.0f;
    if (v < 0.0f)   v = 0.0f;
    if (v > 255.0f) v = 255.0f;
    return (uint8_t)(v + 0.5f);
}

size_t ls_search_dump_line(const ls_search_t *s, const int8_t *last,
                           uint32_t first, uint32_t count, char *out,
                           size_t cap)
{
    if (!s || !s->floor || !last || !out || count == 0 ||
        first >= s->n_bins || count > s->n_bins - first)
        return 0;

    /* The prefix, four numbers, three tags and the digits, the terminator. */
    size_t need = 80 + (size_t)count * 5u;
    if (cap < need) return 0;

    static const char hex[] = "0123456789abcdef";
    uint64_t hz0 = s->start_hz + (uint64_t)s->bin_hz * first + s->bin_hz / 2u;
    int n = snprintf(out, cap, "SEARCH-DUMP D %lu %lu %lu %lu F",
                     (unsigned long)s->pass,
                     (unsigned long)hz0, (unsigned long)s->bin_hz,
                     (unsigned long)count);
    if (n < 0) return 0;
    size_t p = (size_t)n;

    for (uint32_t k = 0; k < count; k++) {
        uint8_t v = s->floor[first + k] < NO_FLOOR_LIMIT
                        ? 0 : enc_db(s->floor[first + k]);
        out[p++] = hex[v >> 4];
        out[p++] = hex[v & 15];
    }
    out[p++] = ' '; out[p++] = 'L';
    for (uint32_t k = 0; k < count; k++) {
        int8_t q = last[first + k];
        uint8_t v = q == LS_SWEEP_NO_DATA ? 0 : enc_db((float)q);
        out[p++] = hex[v >> 4];
        out[p++] = hex[v & 15];
    }
    out[p++] = ' '; out[p++] = 'H';
    for (uint32_t k = 0; k < count; k++)
        out[p++] = (char)('0' + popcount8(s->hist[first + k]));
    out[p] = 0;
    return p;
}

uint32_t ls_search_dump_per_line(const ls_sweep_plan_t *plan)
{
    if (!plan || plan->bin_hz == 0) return 1;
    uint32_t n = plan->hop_hz ? plan->hop_hz / plan->bin_hz
                              : plan->strip_hz / plan->bin_hz;
    return n ? n : 1;
}

size_t ls_search_dump_cap(uint32_t per_line)
{
    return 80 + (size_t)per_line * 5u;
}

size_t ls_search_dump_next(const ls_search_t *s, const int8_t *last,
                           uint32_t per_line, uint32_t *cursor, char *out,
                           size_t cap)
{
    if (!s || !cursor || per_line == 0 || *cursor >= s->n_bins) return 0;
    uint32_t count = s->n_bins - *cursor;
    if (count > per_line) count = per_line;
    size_t len = ls_search_dump_line(s, last, *cursor, count, out, cap);
    if (len) *cursor += count;
    return len;
}
