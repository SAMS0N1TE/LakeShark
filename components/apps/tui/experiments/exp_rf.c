/* See exp_rf.h. */
#include "exp_rf.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ls_lora.h"

/* ----------------------------------------------------------- bursts -- */

/* How fast the noise estimate follows quiet readings, how many spreads a
   burst must clear, and how many readings in a row open one. */
#define NOISE_FOLLOW 0.02f
#define SPREADS      4.0f
#define OPEN_READS   2

void exp_burst_det_init(exp_burst_det_t *d, float thresh_db)
{
    memset(d, 0, sizeof(*d));
    d->thresh_db = thresh_db;
    d->hyst_db = 3.0f;
    if (d->hyst_db > thresh_db / 2) d->hyst_db = thresh_db / 2;
    d->max_gap_us = 5000;
    d->max_len_us = 2000000;
}

static void finish(exp_burst_det_t *d, bool cut, exp_burst_t *out)
{
    out->start_us = d->start_us;
    int64_t len = d->last_above_us - d->start_us + (int64_t)(d->dt_us + 0.5f);
    if (len < 1) len = 1;
    out->len_us = len > UINT32_MAX ? UINT32_MAX : (uint32_t)len;
    out->peak_dbm = d->peak_dbm;
    out->floor_dbm = d->floor_dbm;
    out->cut = cut;
    d->open = false;
}

bool exp_burst_det_flush(exp_burst_det_t *d, exp_burst_t *out)
{
    if (!d->open) return false;
    finish(d, true, out);
    return true;
}

bool exp_burst_det_feed(exp_burst_det_t *d, int64_t t_us, float dbm, exp_burst_t *out)
{
    if (!d->seeded) {
        d->seeded = true;
        d->floor_dbm = dbm;
        d->spread_db = 1.0f;
        d->last_us = t_us;
        return false;
    }
    bool ended = false;
    const int64_t dt = t_us - d->last_us;
    if (d->open && dt > d->max_gap_us) {
        finish(d, true, out);
        ended = true;
    }
    if (dt > 0 && dt <= d->max_gap_us)
        d->dt_us = d->dt_us > 0 ? d->dt_us + ((float)dt - d->dt_us) / 16.0f : (float)dt;
    d->last_us = t_us;

    const float margin = d->thresh_db > SPREADS * d->spread_db ? d->thresh_db
                                                               : SPREADS * d->spread_db;
    const float on = d->floor_dbm + margin;
    const float off = on - d->hyst_db;
    if (d->open) {
        if (dbm >= off) {
            d->last_above_us = t_us;
            if (dbm > d->peak_dbm) d->peak_dbm = dbm;
            /* A level that never goes away is a carrier, or a change in the
               noise: either way it is where the floor now is. */
            if (t_us - d->start_us > d->max_len_us) {
                d->open = false;
                d->floor_dbm = dbm;
            }
            return ended;
        }
        finish(d, false, out);
        ended = true;
    } else if (dbm >= on) {
        if (!d->pending++) { d->pending_us = t_us; d->pending_peak = dbm; }
        else if (dbm > d->pending_peak) d->pending_peak = dbm;
        if (d->pending >= OPEN_READS) {
            d->open = true;
            d->start_us = d->pending_us;
            d->last_above_us = t_us;
            d->peak_dbm = d->pending_peak;
            d->pending = 0;
        }
        return ended;
    }
    d->pending = 0;
    const float dev = fabsf(dbm - d->floor_dbm);
    d->floor_dbm += (dbm - d->floor_dbm) * NOISE_FOLLOW;
    d->spread_db += (dev - d->spread_db) * NOISE_FOLLOW;
    return ended;
}

void exp_burst_log_reset(exp_burst_log_t *l)
{
    memset(l, 0, sizeof(*l));
    l->peak_dbm = -200.0f;
}

void exp_burst_log_add(exp_burst_log_t *l, const exp_burst_t *b)
{
    l->ring[l->head] = *b;
    l->head = (l->head + 1) % EXP_BURST_RING;
    if (l->n < EXP_BURST_RING) l->n++;
    l->count++;
    if (b->peak_dbm > l->peak_dbm) l->peak_dbm = b->peak_dbm;
}

const exp_burst_t *exp_burst_log_at(const exp_burst_log_t *l, int k)
{
    if (k < 0 || k >= l->n) return NULL;
    return &l->ring[(l->head - 1 - k + 2 * EXP_BURST_RING) % EXP_BURST_RING];
}

static uint32_t tol_us(uint32_t us)
{
    const uint32_t t = us / 20;
    return t > 200 ? t : 200;
}

static bool near(uint32_t a, uint32_t b)
{
    const uint32_t d = a > b ? a - b : b - a;
    return d <= tol_us(b);
}

static int cmp_u32(const void *a, const void *b)
{
    const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

void exp_burst_rhythm(const exp_burst_log_t *l, exp_burst_rhythm_t *out)
{
    memset(out, 0, sizeof(*out));
    uint32_t iv[EXP_BURST_RING], len[EXP_BURST_RING];
    int ni = 0;
    for (int k = 0; k < l->n; k++) {
        len[k] = exp_burst_log_at(l, k)->len_us;
        if (k + 1 < l->n) {
            const int64_t d = exp_burst_log_at(l, k)->start_us - exp_burst_log_at(l, k + 1)->start_us;
            if (d > 0 && d <= UINT32_MAX) iv[ni++] = (uint32_t)d;
        }
    }
    if (l->n) {
        qsort(len, (size_t)l->n, sizeof(len[0]), cmp_u32);
        out->median_len_us = len[l->n / 2];
    }
    out->intervals = ni;
    if (ni < 2) return;
    /* The interval with the most others near it; the shorter on a tie. */
    int best = -1, best_hits = 0;
    for (int i = 0; i < ni; i++) {
        int hits = 0;
        for (int j = 0; j < ni; j++) if (near(iv[j], iv[i])) hits++;
        if (hits > best_hits || (hits == best_hits && iv[i] < iv[best])) { best = i; best_hits = hits; }
    }
    /* The middle of that cluster, not its mean: a burst whose start fell in
       a hole in the readings is seen late, and pulls a mean with it. */
    uint32_t near_iv[EXP_BURST_RING];
    int n = 0;
    for (int j = 0; j < ni; j++) if (near(iv[j], iv[best])) near_iv[n++] = iv[j];
    qsort(near_iv, (size_t)n, sizeof(near_iv[0]), cmp_u32);
    out->mode_us = near_iv[n / 2];
    out->mode_hits = n;
    for (int j = 0; j < ni; j++)
        for (uint32_t m = 1; m <= 8; m++)
            if (near(iv[j], out->mode_us * m)) { out->period_hits++; break; }
}

const char *const EXP_LEN_LABEL[EXP_LEN_BUCKETS] = { "<.1m", ".3m", "1m", "3m", "10m", "30m", ".1s", ">" };
const char *const EXP_GAP_LABEL[EXP_GAP_BUCKETS] = { "<1m", "3m", "10m", "30m", ".1s", ".3s", "1s", "3s", ">" };

int exp_len_bucket(uint32_t us)
{
    static const uint32_t EDGE[EXP_LEN_BUCKETS - 1] = { 100, 300, 1000, 3000, 10000, 30000, 100000 };
    int b = 0;
    while (b < EXP_LEN_BUCKETS - 1 && us >= EDGE[b]) b++;
    return b;
}

int exp_gap_bucket(uint32_t us)
{
    static const uint32_t EDGE[EXP_GAP_BUCKETS - 1] = { 1000, 3000, 10000, 30000, 100000, 300000, 1000000, 3000000 };
    int b = 0;
    while (b < EXP_GAP_BUCKETS - 1 && us >= EDGE[b]) b++;
    return b;
}

void exp_burst_hist(const exp_burst_log_t *l, uint32_t len[EXP_LEN_BUCKETS],
                    uint32_t gap[EXP_GAP_BUCKETS])
{
    memset(len, 0, sizeof(uint32_t) * EXP_LEN_BUCKETS);
    memset(gap, 0, sizeof(uint32_t) * EXP_GAP_BUCKETS);
    for (int k = 0; k < l->n; k++) {
        const exp_burst_t *b = exp_burst_log_at(l, k);
        len[exp_len_bucket(b->len_us)]++;
        if (k + 1 < l->n) {
            const int64_t d = b->start_us - exp_burst_log_at(l, k + 1)->start_us;
            if (d > 0) gap[exp_gap_bucket(d > UINT32_MAX ? UINT32_MAX : (uint32_t)d)]++;
        }
    }
}

void exp_burst_strip(const exp_burst_log_t *l, int64_t now_us, int64_t span_us,
                     int64_t since_us, char *out, int cells)
{
    if (cells <= 0) { if (out) out[0] = 0; return; }
    if (span_us < cells) span_us = cells;
    const int64_t t0 = now_us - span_us;
    for (int c = 0; c < cells; c++) {
        const int64_t z = t0 + span_us * (c + 1) / cells;
        out[c] = z <= since_us ? ' ' : '.';
    }
    for (int k = 0; k < l->n; k++) {
        const exp_burst_t *b = exp_burst_log_at(l, k);
        const int64_t s = b->start_us, e = b->start_us + b->len_us;
        if (e <= t0 || s >= now_us) continue;
        int c0 = (int)((s - t0) * cells / span_us), c1 = (int)((e - 1 - t0) * cells / span_us);
        if (c0 < 0) c0 = 0;
        if (c1 >= cells) c1 = cells - 1;
        for (int c = c0; c <= c1; c++) out[c] = '#';
    }
    out[cells] = 0;
}

void exp_fmt_us(uint32_t us, char *out, size_t n)
{
    if (us < 1000)          snprintf(out, n, "%luus", (unsigned long)us);
    else if (us < 10000)    snprintf(out, n, "%.2fms", us / 1000.0);
    else if (us < 1000000)  snprintf(out, n, "%.1fms", us / 1000.0);
    else if (us < 10000000) snprintf(out, n, "%.2fs", us / 1e6);
    else                    snprintf(out, n, "%.0fs", us / 1e6);
}

void exp_fmt_clock(uint32_t s, char *out, size_t n)
{
    if (s < 6000) snprintf(out, n, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
    else          snprintf(out, n, "%luh%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60));
}

uint32_t exp_settle_us(uint32_t bw_hz)
{
    if (!bw_hz || bw_hz > 500000u) bw_hz = 500000u;
    return (300u * 500000u + bw_hz - 1) / bw_hz;
}

/* ------------------------------------------------------- band survey -- */

int exp_survey_plan(uint32_t lo_hz, uint32_t hi_hz, uint32_t caps,
                    uint32_t max_chunk_hz, exp_span_t *out, int max)
{
    exp_span_t reach[2];
    int nr = 0;
    if (!(caps & LS_LORA_CAP_RX_WIDE)) {
        reach[nr++] = (exp_span_t){ LS_LORA_RX_MIN_HZ, LS_LORA_RX_NARROW_MAX_HZ };
    } else {
        reach[nr++] = (exp_span_t){ LS_LORA_RX_MIN_HZ, LS_LORA_RX_LF_MAX_HZ };
        if (caps & LS_LORA_CAP_BAND_1G5_2G5)
            reach[nr++] = (exp_span_t){ LS_LORA_RX_HF_MIN_HZ, LS_LORA_RX_HF_MAX_HZ };
    }
    if (max_chunk_hz < 1000000u) max_chunk_hz = 1000000u;
    int n = 0;
    for (int r = 0; r < nr && n < max; r++) {
        const uint32_t a = lo_hz > reach[r].lo_hz ? lo_hz : reach[r].lo_hz;
        const uint32_t z = hi_hz < reach[r].hi_hz ? hi_hz : reach[r].hi_hz;
        if (z <= a || z - a < 1000000u) continue;
        const uint32_t w = z - a;
        const uint32_t pieces = (w + max_chunk_hz - 1) / max_chunk_hz;
        /* Inner edges on whole megahertz from the start, so a chunk reads as
           "300-342" and never wider than ceil(w / pieces). */
        uint32_t edge = a;
        for (uint32_t p = 0; p < pieces && n < max; p++) {
            const uint32_t next = p + 1 == pieces ? z
                : a + (uint32_t)((uint64_t)w * (p + 1) / pieces / 1000000u * 1000000u);
            out[n].lo_hz = edge;
            out[n].hi_hz = next;
            edge = next;
            n++;
        }
    }
    return n;
}

void exp_survey_chunk_init(exp_survey_chunk_t *c, exp_span_t span, int n)
{
    memset(c, 0, sizeof(*c));
    c->span = span;
    c->n = n < 2 ? 2 : n > EXP_SURVEY_BINS ? EXP_SURVEY_BINS : n;
    for (int i = 0; i < EXP_SURVEY_BINS; i++) c->bin[i].peak_dbm = -200.0f;
}

uint32_t exp_survey_bin_hz(const exp_survey_chunk_t *c, int i)
{
    if (i < 0) i = 0;
    if (i >= c->n) i = c->n - 1;
    const uint64_t span = c->span.hi_hz - c->span.lo_hz;
    return c->span.lo_hz + (uint32_t)(span * (uint64_t)i / (uint64_t)(c->n - 1));
}

static int cmp_float(const void *a, const void *b)
{
    const float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

int exp_survey_row(exp_survey_chunk_t *c, const float *row, float thresh_db,
                   uint32_t t_s, exp_survey_event_t *ev, int max_ev)
{
    float sorted[EXP_SURVEY_BINS];
    memcpy(sorted, row, sizeof(float) * (size_t)c->n);
    qsort(sorted, (size_t)c->n, sizeof(float), cmp_float);
    const float median = sorted[c->n / 2];
    /* The median, not the minimum: one quiet bin would drag the floor down,
       and a band busy across more than half its width is rare enough to
       read as a raised floor when it happens. */
    if (!c->seeded) { c->floor_dbm = median; c->seeded = true; }
    else c->floor_dbm += (median - c->floor_dbm) * 0.2f;
    c->rows++;

    const float on = c->floor_dbm + thresh_db;
    int nev = 0;
    /* A run of bins rising together this row is one event, at its peak. */
    int run_best = -1;
    for (int i = 0; i <= c->n; i++) {
        bool rose = false;
        if (i < c->n) {
            exp_survey_bin_t *b = &c->bin[i];
            if (row[i] > b->peak_dbm) b->peak_dbm = row[i];
            const bool above = row[i] >= on;
            if (above) {
                if (!b->above) b->first_s = t_s;
                b->above++;
                b->last_s = t_s;
                rose = !b->was_above;
            }
            b->was_above = above;
        }
        if (rose) {
            if (run_best < 0 || row[i] > row[run_best]) run_best = i;
        } else if (run_best >= 0) {
            if (nev < max_ev && ev) {
                ev[nev].hz = exp_survey_bin_hz(c, run_best);
                ev[nev].dbm = row[run_best];
                ev[nev].t_s = t_s;
                nev++;
            }
            run_best = -1;
        }
    }
    return nev;
}

int exp_survey_top(const exp_survey_chunk_t *c, exp_survey_emitter_t *out, int max)
{
    bool taken[EXP_SURVEY_BINS] = { false };
    int n = 0;
    while (n < max) {
        int best = -1;
        for (int i = 0; i < c->n; i++) {
            if (!c->bin[i].above || taken[i]) continue;
            if ((i > 0 && taken[i - 1] && c->bin[i - 1].above) ||
                (i + 1 < c->n && taken[i + 1] && c->bin[i + 1].above)) continue;
            if (best < 0 || c->bin[i].peak_dbm > c->bin[best].peak_dbm) best = i;
        }
        if (best < 0) break;
        taken[best] = true;
        const exp_survey_bin_t *b = &c->bin[best];
        out[n].hz = exp_survey_bin_hz(c, best);
        out[n].peak_dbm = b->peak_dbm;
        out[n].duty = c->rows ? (float)b->above / (float)c->rows : 0.0f;
        out[n].first_s = b->first_s;
        out[n].last_s = b->last_s;
        n++;
    }
    return n;
}
