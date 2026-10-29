/* BAND SURVEY's arithmetic: the plan of chunks a radio can sweep and each
   chunk's bookkeeping. Pure: no radio, no clock, no locks; every time is
   handed in, so the bench can drive all of it with made-up series. The
   burst timing BURST SCOPE and IRIDIUM BURSTS used is in their SD apps
   (app-sdk/apps/burst/burst_det.h). */

#ifndef EXP_RF_H
#define EXP_RF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* "4:07" up to 99 minutes, "3h12" after. */
void exp_fmt_clock(uint32_t s, char *out, size_t n);

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
