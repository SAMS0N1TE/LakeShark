/* SPDX-License-Identifier: GPL-3.0-or-later
 * Bounded accounting of IQ blocks a consumer actually processed.
 * Sample duration uses nominal configured rate. It is NOT wall-clock RF
 * coverage: buffered samples, settling, gaps and RF provenance are unknown.
 * The owner serializes writes/snapshot copies; this portable core has no I/O. */
#ifndef LS_IQ_OBSERVATION_H
#define LS_IQ_OBSERVATION_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LS_IQ_OBS_UNAVAILABLE = 0,
    LS_IQ_OBS_DELIVERING,
    LS_IQ_OBS_TUNE,
    LS_IQ_OBS_MODE,
    LS_IQ_OBS_READ_ERROR,
    LS_IQ_OBS_RECEIVER_ERROR,
    LS_IQ_OBS_STOPPED,
    LS_IQ_OBS_UNSUPPORTED,
} ls_iq_observation_state_t;

typedef struct {
    bool known, accepting, saturated;
    uint32_t generation, nominal_rate_hz;
    uint64_t configured_center_hz;
    uint64_t delivered_bytes, delivered_blocks;
    uint64_t first_delivery_us, last_delivery_us;
    ls_iq_observation_state_t state;
} ls_iq_observation_t;

/* A new segment discards previous segment counts, including after a tune,
 * mode/session change or read error. No lifetime/mission completeness claim. */
void ls_iq_observation_begin(ls_iq_observation_t *s, uint32_t rate_hz,
                             uint64_t configured_center_hz);
void ls_iq_observation_close(ls_iq_observation_t *s,
                             ls_iq_observation_state_t reason);
/* Only complete, successfully consumed U8 interleaved I/Q blocks qualify.
 * Partial or failed reads must never call this with their unknown progress. */
bool ls_iq_observation_deliver(ls_iq_observation_t *s, size_t bytes,
                               uint64_t delivered_at_us);
bool ls_iq_observation_nominal_us(const ls_iq_observation_t *s,
                                  uint64_t *out);
const char *ls_iq_observation_state_name(ls_iq_observation_state_t state);
#ifdef __cplusplus
}
#endif
#endif
