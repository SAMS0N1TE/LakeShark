#ifndef P25_ACQUISITION_H
#define P25_ACQUISITION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t attempts;
    uint32_t timeouts;
    uint32_t symbols;
    uint32_t raw_syncs;
    uint32_t inverted_matches;
    int32_t symbol_min;
    int32_t symbol_max;
    uint32_t best_normal_hd;
    uint32_t best_inverted_hd;
} p25_hunt_status_t;

typedef struct {
    uint32_t generation;
    uint32_t resets;
    uint32_t pending_waits;
    uint32_t failed_waits;
    uint32_t discarded_syncs;
    uint32_t discarded_frames;
    uint32_t accepted_syncs;
} p25_acquisition_gate_t;

typedef struct {
    uint32_t sampled_pairs;
    uint32_t clipped_components;
    uint32_t mean_square;
    int32_t mean_i_milli;
    int32_t mean_q_milli;
} p25_iq_probe_t;

typedef struct {
    p25_acquisition_gate_t gate;
    p25_hunt_status_t hunt;
    p25_iq_probe_t iq;
    uint32_t decoder_updated_ms;
    uint32_t iq_updated_ms;
    uint32_t ring_drops;
    uint32_t tune_state;
} p25_acquisition_status_t;

struct dsd_opts;
struct dsd_state;

/* this is the live decoder fence, also used by IQ replay. A transport
 * byte counter cannot distinguish an active hunt from a tune-blocked decoder. */
bool p25_acquisition_prepare(p25_acquisition_gate_t *gate,
    struct dsd_opts *opts, struct dsd_state *state, uint32_t generation,
    bool pending, bool failed);
bool p25_acquisition_accept_sync(p25_acquisition_gate_t *gate,
    uint32_t generation, int sync);
bool p25_acquisition_accept_frame(p25_acquisition_gate_t *gate,
    uint32_t generation);
void p25_acquisition_probe_iq(p25_iq_probe_t *out,
    const uint8_t *iq, size_t bytes);

/* Last completed hunt and one IQ block per second, not proof of target RF.
 * Each timestamp identifies that writer's snapshot; counters wrap at 2^32.
 * No heap allocation, UART initialization or receiver/tuning side effects. */
void p25_get_acquisition_status(p25_acquisition_status_t *out);
size_t p25_acquisition_format(const p25_acquisition_status_t *status,
                              char *out, size_t capacity);

#ifdef __cplusplus
}
#endif
#endif
