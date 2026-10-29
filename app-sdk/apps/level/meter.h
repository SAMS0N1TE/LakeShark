/* CARRIER LEVEL's engine: I/Q in, the level of a narrow channel at the
 * tuning, and the readout. Pure (no lap_* calls), for the bench
 * (bench/tests/test_carrier_app.c). */
#ifndef CARRIER_METER_H
#define CARRIER_METER_H

#include "../burst/burst_det.h"

#define METER_LINE 52
#define METER_FILTER_HZ  12500u  /* the channel */
#define METER_READING_US 2000u   /* one reading */
/* The bar's ends. */
#define METER_BAR_LO (-100.0f)
#define METER_BAR_HI 0.0f
#define METER_BAR_CELLS 30

typedef struct { float lo, hi; double sum; uint32_t n; } meter_span_t;

typedef struct {
    burst_level_t lvl;
    uint32_t hz;
    float now;
    bool have_now;
    meter_span_t second;        /* the last whole second */
    meter_span_t total;         /* since start */
    meter_span_t acc;           /* the second being gathered */
    int64_t acc_t0;
    bool started;
    uint32_t reads, gaps;
    int64_t tt[32];
    float db[32];
} meter_t;

void meter_start(meter_t *m, uint32_t hz, uint32_t rate);
void meter_feed(meter_t *m, const int16_t *iq, int n, uint64_t first, bool gap);
/* The readout; before the first run, the frequency the next one uses. */
int meter_lines(const meter_t *m, uint32_t next_hz, char (*out)[METER_LINE], int max);

#endif /* CARRIER_METER_H */
