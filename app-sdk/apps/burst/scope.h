/* BURST SCOPE's engine: I/Q in, level readings, bursts and their timing,
 * and the readout. Pure (no lap_* calls), so the bench drives it with a
 * made-up pulse train (bench/tests/test_burst_app.c). */
#ifndef SCOPE_H
#define SCOPE_H

#include "burst_det.h"

#define SCOPE_LINE  52
#define SCOPE_LINES 12
#define SCOPE_READING_US 50u     /* about one reading this often */

typedef struct {
    burst_level_t lvl;
    burst_det_t det;
    burst_log_t log;
    uint32_t hz, filter_hz;     /* the filter as the rate allows it */
    int thresh_db;
    int64_t t_start, t_now;     /* the first and latest reading, radio time */
    bool started;
    uint64_t reads;
    uint32_t gaps;
    float now_db;
    bool have_now;
    int64_t tt[2048];
    float db[2048];
} scope_t;

/* A new run at hz with the radio's rate, the filter asked for and the
   threshold over the floor. */
void scope_start(scope_t *s, uint32_t hz, uint32_t rate, uint32_t filter_hz, int thresh_db);
/* n pairs (up to 2048) whose first is sample number `first`; gap when
   samples were lost before them. */
void scope_feed(scope_t *s, const int16_t *iq, int n, uint64_t first, bool gap);
/* The readout, up to max lines of SCOPE_LINE - 1 characters. */
int scope_lines(const scope_t *s, char (*out)[SCOPE_LINE], int max);
/* Never run: what the next start will do. */
int scope_idle_lines(uint32_t hz, uint32_t filter_hz, int thresh_db, char (*out)[SCOPE_LINE], int max);

#endif /* SCOPE_H */
