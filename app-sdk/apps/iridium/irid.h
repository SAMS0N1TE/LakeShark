/* IRIDIUM BURSTS' engine: the hop plan, bursts per channel, the strongest
 * channel each cycle and the readout. Pure (no lap_* calls): main.c tunes
 * the radio and hands the I/Q in; the bench drives it with a made-up
 * satellite (bench/tests/test_iridium_app.c). */
#ifndef IRID_H
#define IRID_H

#include "../burst/burst_det.h"

#define IRID_LINE    52
#define IRID_LINES   16
#define IRID_CH_MAX  24
#define IRID_TRACE_N 24
#define IRID_READING_US 50u

typedef struct {
    const char *name;
    uint32_t lo_hz, hi_hz;
    int ch;
} irid_plan_t;

/* The simplex band in channels near the 41.667 kHz Iridium raster, and the
   whole downlink band in 500 kHz steps. */
#define IRID_PLAN_N 2
extern const irid_plan_t IRID_PLAN[IRID_PLAN_N];

typedef struct { uint32_t t_s; int ch; float db; } irid_trace_t;

typedef struct {
    irid_plan_t plan;
    uint32_t spacing, rate, settle_us;
    int dwell_ms, thresh_db;
    int ch;
    /* Listening on ch: from the first sample after it settled to the end of
       the dwell, in the radio's sample numbers. */
    bool listening;
    uint64_t listen_from, listen_until;
    burst_level_t lvl;
    burst_det_t det[IRID_CH_MAX];
    burst_log_t log;
    uint32_t ch_count[IRID_CH_MAX];
    float ch_peak[IRID_CH_MAX];
    /* The strongest burst this cycle, and the cycles kept. */
    int cyc_ch;
    float cyc_db;
    uint32_t cycles;
    irid_trace_t trace[IRID_TRACE_N];
    int trace_n, trace_head;
    /* Bursts in each of the last 60 seconds. */
    uint32_t sec_n[60], sec_at[60];
    bool started;
    int64_t t_start, t_now;     /* radio time of the first and latest reading */
    uint64_t reads, listen_pairs;
    uint32_t gaps, hops;
    uint32_t spurs;             /* bursts too short or too long to be an Iridium slot */
    int64_t tt[2048];
    float db[2048];
} irid_t;

/* A run of plan p at the radio's rate. */
void irid_start(irid_t *e, int plan, int dwell_ms, int thresh_db, uint32_t rate);
/* The centre of channel ch. */
uint32_t irid_ch_hz(const irid_t *e, int ch);
/* n pairs (up to 2048) from sample `first`, tuned to center_hz; gap when
   samples were lost before them. Blocks still from the last channel are
   passed over. True when the dwell on this channel is over: hop. */
bool irid_feed(irid_t *e, const int16_t *iq, int n, uint64_t first, uint64_t center_hz, bool gap);
/* On to the next channel; returns its centre, for the radio. */
uint32_t irid_hop(irid_t *e);
int irid_lines(const irid_t *e, char (*out)[IRID_LINE], int max);
/* Never run: what the next start will do. */
int irid_idle_lines(int plan, int dwell_ms, int thresh_db, char (*out)[IRID_LINE], int max);

#endif /* IRID_H */
