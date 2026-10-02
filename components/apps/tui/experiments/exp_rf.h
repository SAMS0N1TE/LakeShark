/* The arithmetic the RF experiments share: finding bursts in a stream of
   RSSI readings and timing them, and the band survey's bookkeeping. Pure:
   no radio, no clock, no locks; every time is handed in, so the bench can
   drive all of it with made-up series. */

#ifndef EXP_RF_H
#define EXP_RF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------- bursts -- */

/* One burst: when it rose above the floor, how long it stayed, how strong. */
typedef struct {
    int64_t  start_us;
    uint32_t len_us;
    float    peak_dbm;
    float    floor_dbm;   /* the floor it was measured against */
    bool     cut;         /* a hole in the readings ended it, not the signal */
} exp_burst_t;

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
    float   floor_dbm;
    float   spread_db;   /* mean absolute deviation of quiet readings */
    int     pending;     /* readings in a row over the threshold, not yet a burst */
    int64_t pending_us;
    float   pending_peak;
    bool    seeded, open;
    int64_t start_us, last_above_us, last_us;
    float   peak_dbm;
    float   dt_us;       /* the usual spacing of the readings */
} exp_burst_det_t;

/* 3 dB of hysteresis, a 5 ms gap and a 2 s level by default. */
void exp_burst_det_init(exp_burst_det_t *d, float thresh_db);
/* One reading. True, with the burst in *out, when one ended: at most one a
   call. A burst that ends for want of readings is marked cut. */
bool exp_burst_det_feed(exp_burst_det_t *d, int64_t t_us, float dbm, exp_burst_t *out);
/* End an open burst now, at its last reading over the floor, as cut: for a
   listener that is about to look somewhere else. */
bool exp_burst_det_flush(exp_burst_det_t *d, exp_burst_t *out);

/* The last EXP_BURST_RING bursts and totals over all of them. */
#define EXP_BURST_RING 64
typedef struct {
    exp_burst_t ring[EXP_BURST_RING];
    int      n, head;          /* head: where the next one goes */
    uint32_t count;
    float    peak_dbm;
} exp_burst_log_t;

void exp_burst_log_reset(exp_burst_log_t *l);
void exp_burst_log_add(exp_burst_log_t *l, const exp_burst_t *b);
/* k = 0 is the newest; NULL past the oldest kept. */
const exp_burst_t *exp_burst_log_at(const exp_burst_log_t *l, int k);

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
} exp_burst_rhythm_t;
void exp_burst_rhythm(const exp_burst_log_t *l, exp_burst_rhythm_t *out);

/* Log-spaced histogram buckets for display. Lengths: under 0.1, 0.3, 1, 3,
   10, 30, 100 ms, and over. Intervals: under 1, 3, 10, 30, 100, 300 ms, 1,
   3 s, and over. */
#define EXP_LEN_BUCKETS 8
#define EXP_GAP_BUCKETS 9
int  exp_len_bucket(uint32_t us);
int  exp_gap_bucket(uint32_t us);
extern const char *const EXP_LEN_LABEL[EXP_LEN_BUCKETS];
extern const char *const EXP_GAP_LABEL[EXP_GAP_BUCKETS];
/* Both histograms over the bursts kept. */
void exp_burst_hist(const exp_burst_log_t *l, uint32_t len[EXP_LEN_BUCKETS],
                    uint32_t gap[EXP_GAP_BUCKETS]);

/* A strip of `cells` characters for the last `span_us` before `now_us`:
   '#' where a kept burst covers any of the cell, '.' where nothing did, and
   ' ' before `since_us`, when nothing was listening. Terminated; `out`
   holds cells + 1. */
void exp_burst_strip(const exp_burst_log_t *l, int64_t now_us, int64_t span_us,
                     int64_t since_us, char *out, int cells);

/* "12.3ms", "840us", "1.25s": a duration in six characters or fewer. */
void exp_fmt_us(uint32_t us, char *out, size_t n);
/* "4:07" up to 99 minutes, "3h12" after. */
void exp_fmt_clock(uint32_t s, char *out, size_t n);

/* How long after a retune a reading through a `bw_hz` filter is worth
   having: the sweep's measured 300 us at 500 kHz, longer as the filter
   narrows, never shorter. */
uint32_t exp_settle_us(uint32_t bw_hz);

/* ------------------------------------------------------- band survey -- */

/* Readings per row of a survey chunk. */
#define EXP_SURVEY_BINS 64

typedef struct { uint32_t lo_hz, hi_hz; } exp_span_t;

/* [lo_hz, hi_hz] cut into chunks no wider than `max_chunk_hz`, keeping only
   what a radio with capabilities `caps` (LS_LORA_CAP_*) can sweep: 150-960
   MHz, or 150-1100 and 1500-2500 MHz on a part that says it reaches them. A
   piece narrower than 1 MHz is dropped. Returns how many, at most `max`. */
int exp_survey_plan(uint32_t lo_hz, uint32_t hi_hz, uint32_t caps,
                    uint32_t max_chunk_hz, exp_span_t *out, int max);

typedef struct {
    float    peak_dbm;      /* the strongest reading ever, over the floor or not */
    uint32_t above;         /* rows it read over floor + threshold */
    uint32_t first_s, last_s;   /* when it first and last did; valid once above */
    bool     was_above;     /* in the row before */
} exp_survey_bin_t;

typedef struct {
    exp_span_t span;
    float    floor_dbm;     /* each row's median, smoothed */
    bool     seeded;
    uint32_t rows;
    int      n;             /* bins a row */
    exp_survey_bin_t bin[EXP_SURVEY_BINS];
} exp_survey_chunk_t;

/* Something that came up: a run of neighbouring bins that crossed the
   threshold in one row, at its strongest. */
typedef struct { uint32_t hz; float dbm; uint32_t t_s; } exp_survey_event_t;

typedef struct {
    uint32_t hz;
    float    peak_dbm, duty;    /* duty: rows over the threshold / rows */
    uint32_t first_s, last_s;
} exp_survey_emitter_t;

void exp_survey_chunk_init(exp_survey_chunk_t *c, exp_span_t span, int n);
/* The frequency of bin i, as the sweep places it: lo + span * i / (n - 1). */
uint32_t exp_survey_bin_hz(const exp_survey_chunk_t *c, int i);
/* One row of c->n readings, taken at `t_s` seconds into the survey. Returns
   the events (at most `max_ev`). */
int exp_survey_row(exp_survey_chunk_t *c, const float *row, float thresh_db,
                   uint32_t t_s, exp_survey_event_t *ev, int max_ev);
/* The strongest bins that were ever over the threshold, strongest first, no
   two neighbours (one emitter spills into the bins either side). */
int exp_survey_top(const exp_survey_chunk_t *c, exp_survey_emitter_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* EXP_RF_H */
