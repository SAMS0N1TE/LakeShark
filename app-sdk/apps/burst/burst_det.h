/* Bursts in a stream of level readings, and the level readings themselves
 * from a radio's I/Q: what BURST SCOPE, IRIDIUM BURSTS and CARRIER LEVEL
 * share.
 *
 * An app folder builds alone (lap.py compiles only its own .c files), so the
 * other two include this file from here as "../burst/burst_det.h". Every
 * function is static, so the PC simulator, which links every app into one
 * program, sees no clash, and an app that leaves one unused pays nothing.
 *
 * Floats never meet 64-bit integers directly here: the board exports no
 * conversion between the two, so they go through 32 bits or double.
 *
 * Pure: no lap_* calls, no clock. Every time is handed in, so the bench can
 * drive all of it with made-up series (bench/tests/test_burst_app.c). */
#ifndef BURST_DET_H
#define BURST_DET_H

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------- readings -- */

/* Level readings from I/Q: a boxcar of `filt` pairs is the channel filter
   (its first null at rate / filt), and the power of `win` filtered pairs in
   a row is one reading, in dB relative to full scale (a full-scale tone
   reads 0). A reading is stamped with the time of its first pair, counted
   from the radio's own sample numbers, so the timing is exact however late
   the app reads the queue. */
typedef struct {
    uint32_t rate;
    int filt, win;
    int64_t si, sq;             /* the boxcar being summed */
    int fn;
    double p;                   /* the reading being summed */
    int pn;
    uint64_t next;              /* the sample number the next pair should carry */
    uint64_t start;             /* the first sample of the reading being summed */
    bool have;
} burst_level_t;

/* filter_hz: the channel; reading_us: about how long one reading takes. */
static inline void burst_level_init(burst_level_t *l, uint32_t rate, uint32_t filter_hz, uint32_t reading_us)
{
    memset(l, 0, sizeof(*l));
    l->rate = rate ? rate : 1;
    l->filt = filter_hz && filter_hz < l->rate ? (int)((l->rate + filter_hz / 2) / filter_hz) : 1;
    if (l->filt < 1) l->filt = 1;
    const double pairs = (double)l->rate * reading_us / 1e6 / l->filt;
    l->win = pairs < 1 ? 1 : (int)(pairs + 0.5);
}

/* Microseconds between readings. */
static inline uint32_t burst_level_us(const burst_level_t *l)
{
    return (uint32_t)((uint64_t)l->filt * (uint64_t)l->win * 1000000u / l->rate);
}

/* Drop what is half summed: the next pair starts afresh (after a gap or a
   retune). */
static inline void burst_level_restart(burst_level_t *l)
{
    l->si = l->sq = 0;
    l->fn = 0;
    l->p = 0;
    l->pn = 0;
    l->have = false;
}

static inline int64_t burst_level_t_us(const burst_level_t *l, uint64_t sample)
{
    return (int64_t)(sample * 1000000u / l->rate);
}

/* n pairs whose first is sample number `first`. Each reading finished goes
   to out[] with its time, up to max; returns how many. A first that is not
   where the last block ended starts the sums again. */
static inline int burst_level_feed(burst_level_t *l, const int16_t *iq, int n, uint64_t first,
                                   int64_t *t_out, float *db_out, int max)
{
    int got = 0;
    if (l->have && first != l->next) burst_level_restart(l);
    l->have = true;
    l->next = first + (uint64_t)n;
    const double full = 32768.0 * 32768.0;
    for (int k = 0; k < n; k++) {
        if (!l->fn && !l->pn) l->start = first + (uint64_t)k;
        l->si += iq[2 * k];
        l->sq += iq[2 * k + 1];
        if (++l->fn < l->filt) continue;
        const double i = (double)l->si / l->fn, q = (double)l->sq / l->fn;
        l->si = l->sq = 0;
        l->fn = 0;
        l->p += i * i + q * q;
        if (++l->pn < l->win) continue;
        const double p = l->p / l->pn / full;
        l->p = 0;
        l->pn = 0;
        if (got < max) {
            t_out[got] = burst_level_t_us(l, l->start);
            db_out[got] = p > 1e-14 ? 4.3429448f * logf((float)p) : -140.0f;   /* logf: the board exports no log10 */
            got++;
        }
    }
    return got;
}

/* ----------------------------------------------------------- bursts -- */

/* One burst: when it rose above the floor, how long it stayed, how strong. */
typedef struct {
    int64_t  start_us;
    uint32_t len_us;
    float    peak_db;
    float    floor_db;    /* the floor it was measured against */
    bool     cut;         /* a hole in the readings ended it, not the signal */
} burst_t;

/* The detector. Quiet readings set the noise: its average (the floor) and
   its spread, the average distance of a reading from that average. A burst
   opens on two readings in a row over the floor by `thresh_db` or by four
   spreads, whichever is more - single readings in a wide filter swing by
   ten dB on noise alone - and one under that by less than `hyst_db` keeps
   it open. While a burst is open the noise estimate stands still. */
typedef struct {
    float   thresh_db;
    float   hyst_db;
    int64_t max_gap_us;  /* a gap in the readings longer than this ends an open burst */
    int64_t max_len_us;  /* a level held longer than this is the new floor, not a burst */
    float   floor_db;
    float   spread_db;   /* mean absolute deviation of quiet readings */
    int     pending;     /* readings in a row over the threshold, not yet a burst */
    int64_t pending_us;
    float   pending_peak;
    bool    seeded, open;
    int64_t start_us, last_above_us, last_us;
    float   peak_db;
    float   dt_us;       /* the usual spacing of the readings */
} burst_det_t;

/* How fast the noise estimate follows quiet readings, how many spreads a
   burst must clear, and how many readings in a row open one. */
#define BURST_NOISE_FOLLOW 0.02f
#define BURST_SPREADS      4.0f
#define BURST_OPEN_READS   2

/* 3 dB of hysteresis, a 5 ms gap and a 2 s level by default. */
static inline void burst_det_init(burst_det_t *d, float thresh_db)
{
    memset(d, 0, sizeof(*d));
    d->thresh_db = thresh_db;
    d->hyst_db = 3.0f;
    if (d->hyst_db > thresh_db / 2) d->hyst_db = thresh_db / 2;
    d->max_gap_us = 5000;
    d->max_len_us = 2000000;
}

static inline void burst_det_finish(burst_det_t *d, bool cut, burst_t *out)
{
    out->start_us = d->start_us;
    int64_t len = d->last_above_us - d->start_us + (int32_t)(d->dt_us + 0.5f);
    if (len < 1) len = 1;
    out->len_us = len > UINT32_MAX ? UINT32_MAX : (uint32_t)len;
    out->peak_db = d->peak_db;
    out->floor_db = d->floor_db;
    out->cut = cut;
    d->open = false;
}

/* End an open burst now, at its last reading over the floor, as cut: for a
   listener that is about to look somewhere else. */
static inline bool burst_det_flush(burst_det_t *d, burst_t *out)
{
    if (!d->open) return false;
    burst_det_finish(d, true, out);
    return true;
}

/* One reading. True, with the burst in *out, when one ended: at most one a
   call. A burst that ends for want of readings is marked cut. */
static inline bool burst_det_feed(burst_det_t *d, int64_t t_us, float db, burst_t *out)
{
    if (!d->seeded) {
        d->seeded = true;
        d->floor_db = db;
        d->spread_db = 1.0f;
        d->last_us = t_us;
        return false;
    }
    bool ended = false;
    const int64_t dt = t_us - d->last_us;
    if (d->open && dt > d->max_gap_us) {
        burst_det_finish(d, true, out);
        ended = true;
    }
    if (dt > 0 && dt <= d->max_gap_us)
        d->dt_us = d->dt_us > 0 ? d->dt_us + ((float)(int32_t)dt - d->dt_us) / 16.0f : (float)(int32_t)dt;
    d->last_us = t_us;

    const float margin = d->thresh_db > BURST_SPREADS * d->spread_db ? d->thresh_db
                                                                     : BURST_SPREADS * d->spread_db;
    const float on = d->floor_db + margin;
    const float off = on - d->hyst_db;
    if (d->open) {
        if (db >= off) {
            d->last_above_us = t_us;
            if (db > d->peak_db) d->peak_db = db;
            /* A level that never goes away is a carrier, or a change in the
               noise: either way it is where the floor now is. */
            if (t_us - d->start_us > d->max_len_us) {
                d->open = false;
                d->floor_db = db;
            }
            return ended;
        }
        burst_det_finish(d, false, out);
        ended = true;
    } else if (db >= on) {
        if (!d->pending++) { d->pending_us = t_us; d->pending_peak = db; }
        else if (db > d->pending_peak) d->pending_peak = db;
        if (d->pending >= BURST_OPEN_READS) {
            d->open = true;
            d->start_us = d->pending_us;
            d->last_above_us = t_us;
            d->peak_db = d->pending_peak;
            d->pending = 0;
        }
        return ended;
    }
    d->pending = 0;
    const float dev = fabsf(db - d->floor_db);
    d->floor_db += (db - d->floor_db) * BURST_NOISE_FOLLOW;
    d->spread_db += (dev - d->spread_db) * BURST_NOISE_FOLLOW;
    return ended;
}

/* The last BURST_RING bursts and totals over all of them. */
#define BURST_RING 64
typedef struct {
    burst_t  ring[BURST_RING];
    int      n, head;          /* head: where the next one goes */
    uint32_t count;
    float    peak_db;
} burst_log_t;

static inline void burst_log_reset(burst_log_t *l)
{
    memset(l, 0, sizeof(*l));
    l->peak_db = -200.0f;
}

static inline void burst_log_add(burst_log_t *l, const burst_t *b)
{
    l->ring[l->head] = *b;
    l->head = (l->head + 1) % BURST_RING;
    if (l->n < BURST_RING) l->n++;
    l->count++;
    if (b->peak_db > l->peak_db) l->peak_db = b->peak_db;
}

/* k = 0 is the newest; NULL past the oldest kept. */
static inline const burst_t *burst_log_at(const burst_log_t *l, int k)
{
    if (k < 0 || k >= l->n) return NULL;
    return &l->ring[(l->head - 1 - k + 2 * BURST_RING) % BURST_RING];
}

/* The rhythm of the bursts kept: the commonest start-to-start interval, to
   within 5 % (and 0.2 ms), and how many intervals are a whole multiple of
   it, one to eight, which is what a periodic sender with missed bursts
   looks like. */
typedef struct {
    int      intervals;     /* start-to-start intervals between kept bursts */
    uint32_t mode_us;       /* 0 with fewer than two intervals */
    int      mode_hits;     /* intervals within tolerance of the mode */
    int      period_hits;   /* within tolerance of 1..8 times it */
    uint32_t median_len_us;
} burst_rhythm_t;

static inline bool burst_near(uint32_t a, uint32_t b)
{
    const uint32_t d = a > b ? a - b : b - a, t = b / 20;
    return d <= (t > 200 ? t : 200);
}

static inline int burst_cmp_u32(const void *a, const void *b)
{
    const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static inline void burst_rhythm(const burst_log_t *l, burst_rhythm_t *out)
{
    memset(out, 0, sizeof(*out));
    uint32_t iv[BURST_RING], len[BURST_RING];
    int ni = 0;
    for (int k = 0; k < l->n; k++) {
        len[k] = burst_log_at(l, k)->len_us;
        if (k + 1 < l->n) {
            const int64_t d = burst_log_at(l, k)->start_us - burst_log_at(l, k + 1)->start_us;
            if (d > 0 && d <= UINT32_MAX) iv[ni++] = (uint32_t)d;
        }
    }
    if (l->n) {
        qsort(len, (size_t)l->n, sizeof(len[0]), burst_cmp_u32);
        out->median_len_us = len[l->n / 2];
    }
    out->intervals = ni;
    if (ni < 2) return;
    /* The interval with the most others near it; the shorter on a tie. */
    int best = -1, best_hits = 0;
    for (int i = 0; i < ni; i++) {
        int hits = 0;
        for (int j = 0; j < ni; j++) if (burst_near(iv[j], iv[i])) hits++;
        if (hits > best_hits || (hits == best_hits && iv[i] < iv[best])) { best = i; best_hits = hits; }
    }
    /* The middle of that cluster, not its mean: a burst whose start fell in
       a hole in the readings is seen late, and pulls a mean with it. */
    uint32_t near_iv[BURST_RING];
    int n = 0;
    for (int j = 0; j < ni; j++) if (burst_near(iv[j], iv[best])) near_iv[n++] = iv[j];
    qsort(near_iv, (size_t)n, sizeof(near_iv[0]), burst_cmp_u32);
    out->mode_us = near_iv[n / 2];
    out->mode_hits = n;
    for (int j = 0; j < ni; j++)
        for (uint32_t m = 1; m <= 8; m++)
            if (burst_near(iv[j], out->mode_us * m)) { out->period_hits++; break; }
}

/* Log-spaced histogram buckets for display. Lengths: under 0.1, 0.3, 1, 3,
   10, 30, 100 ms, and over. Intervals: under 1, 3, 10, 30, 100, 300 ms, 1,
   3 s, and over. */
#define BURST_LEN_BUCKETS 8
#define BURST_GAP_BUCKETS 9
static inline const char *burst_len_label(int b)
{
    static const char *const L[BURST_LEN_BUCKETS] = { "<.1m", ".3m", "1m", "3m", "10m", "30m", ".1s", ">" };
    return b >= 0 && b < BURST_LEN_BUCKETS ? L[b] : "";
}
static inline const char *burst_gap_label(int b)
{
    static const char *const L[BURST_GAP_BUCKETS] = { "<1m", "3m", "10m", "30m", ".1s", ".3s", "1s", "3s", ">" };
    return b >= 0 && b < BURST_GAP_BUCKETS ? L[b] : "";
}

static inline int burst_len_bucket(uint32_t us)
{
    static const uint32_t EDGE[BURST_LEN_BUCKETS - 1] = { 100, 300, 1000, 3000, 10000, 30000, 100000 };
    int b = 0;
    while (b < BURST_LEN_BUCKETS - 1 && us >= EDGE[b]) b++;
    return b;
}

static inline int burst_gap_bucket(uint32_t us)
{
    static const uint32_t EDGE[BURST_GAP_BUCKETS - 1] = { 1000, 3000, 10000, 30000, 100000, 300000, 1000000, 3000000 };
    int b = 0;
    while (b < BURST_GAP_BUCKETS - 1 && us >= EDGE[b]) b++;
    return b;
}

/* Both histograms over the bursts kept. */
static inline void burst_hist(const burst_log_t *l, uint32_t len[BURST_LEN_BUCKETS], uint32_t gap[BURST_GAP_BUCKETS])
{
    memset(len, 0, sizeof(uint32_t) * BURST_LEN_BUCKETS);
    memset(gap, 0, sizeof(uint32_t) * BURST_GAP_BUCKETS);
    for (int k = 0; k < l->n; k++) {
        const burst_t *b = burst_log_at(l, k);
        len[burst_len_bucket(b->len_us)]++;
        if (k + 1 < l->n) {
            const int64_t d = b->start_us - burst_log_at(l, k + 1)->start_us;
            if (d > 0) gap[burst_gap_bucket(d > UINT32_MAX ? UINT32_MAX : (uint32_t)d)]++;
        }
    }
}

/* A strip of `cells` characters for the last `span_us` before `now_us`:
   '#' where a kept burst covers any of the cell, '.' where nothing did, and
   ' ' before `since_us`, when nothing was listening. Terminated; `out`
   holds cells + 1. */
static inline void burst_strip(const burst_log_t *l, int64_t now_us, int64_t span_us, int64_t since_us,
                               char *out, int cells)
{
    if (cells <= 0) { if (out) out[0] = 0; return; }
    if (span_us < cells) span_us = cells;
    const int64_t t0 = now_us - span_us;
    for (int c = 0; c < cells; c++) {
        const int64_t z = t0 + span_us * (c + 1) / cells;
        out[c] = z <= since_us ? ' ' : '.';
    }
    for (int k = 0; k < l->n; k++) {
        const burst_t *b = burst_log_at(l, k);
        const int64_t s = b->start_us, e = b->start_us + b->len_us;
        if (e <= t0 || s >= now_us) continue;
        int c0 = (int)((s - t0) * cells / span_us), c1 = (int)((e - 1 - t0) * cells / span_us);
        if (c0 < 0) c0 = 0;
        if (c1 >= cells) c1 = cells - 1;
        for (int c = c0; c <= c1; c++) out[c] = '#';
    }
    out[cells] = 0;
}

/* "12.3ms", "840us", "1.25s": a duration in six characters or fewer. */
static inline void burst_fmt_us(uint32_t us, char *out, size_t n)
{
    if (us < 1000)          snprintf(out, n, "%luus", (unsigned long)us);
    else if (us < 10000)    snprintf(out, n, "%.2fms", us / 1000.0);
    else if (us < 1000000)  snprintf(out, n, "%.1fms", us / 1000.0);
    else if (us < 10000000) snprintf(out, n, "%.2fs", us / 1e6);
    else                    snprintf(out, n, "%.0fs", us / 1e6);
}

/* "4:07" up to 99 minutes, "3h12" after. */
static inline void burst_fmt_clock(uint32_t s, char *out, size_t n)
{
    if (s < 6000) snprintf(out, n, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
    else          snprintf(out, n, "%luh%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60));
}

/* How long after a retune a reading through a `bw_hz` filter is worth
   having: 300 us at 500 kHz, longer as the filter narrows, never shorter. */
static inline uint32_t burst_settle_us(uint32_t bw_hz)
{
    if (!bw_hz || bw_hz > 500000u) bw_hz = 500000u;
    return (300u * 500000u + bw_hz - 1) / bw_hz;
}

#endif /* BURST_DET_H */
