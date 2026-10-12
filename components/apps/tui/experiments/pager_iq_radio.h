#ifndef PAGER_IQ_RADIO_H
#define PAGER_IQ_RADIO_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define PGR_IQ_RADIO_BLOCK_SAMPLES 64u
typedef struct {
    uint64_t bytes;
    uint32_t blocks, gaps, errors, queue_peak;
    esp_err_t error, recovery_error;
    uint32_t max_fifo_level;
    uint8_t fifo_flags;
    bool active;
} pgr_iq_radio_stats_t;

/* One owner calls all APIs, holding the experiment's radio lease. Do not
   poll RSSI, configure, or use other radio APIs until end returns. */
bool pgr_iq_radio_supported(void);
esp_err_t pgr_iq_radio_begin(uint32_t hz);
/* 0 empty, >0 discriminator samples at 25ksps, -1 fault. Faults stay latched
   until end; discard decoder alignment and end/retry, never bridge them.
   Capacity must be at least PGR_IQ_RADIO_BLOCK_SAMPLES. */
int pgr_iq_radio_read(float *out, size_t capacity);
void pgr_iq_radio_end(void);
/* Last session's stats remain available after end. */
void pgr_iq_radio_stats(pgr_iq_radio_stats_t *out);
#endif
