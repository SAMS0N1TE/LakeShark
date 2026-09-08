#ifndef P25_DEMOD_CONTROL_H
#define P25_DEMOD_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

#include "dsp_pipeline.h"

#ifdef __cplusplus
extern "C" {
#endif

#define P25_DEMOD_AUTO             (-1)
#define P25_DEMOD_AUTO_WINDOW_MS   1500U
#define P25_DEMOD_LOCK_LOSS_MS     4000U
#define P25_DEMOD_RETRY_MS         1000U

typedef enum {
    P25_DEMOD_HUNT_C4FM,
    P25_DEMOD_HUNT_CQPSK,
    P25_DEMOD_LOCKED,
    P25_DEMOD_NO_LOCK,
} p25_demod_phase_t;

typedef struct {
    int               preference;
    demod_mode_t      active;
    p25_demod_phase_t phase;
    uint32_t          window_start_ms;
    uint32_t          last_protocol_ms;
    uint32_t          base_nids;
    uint32_t          base_tsbks;
    uint32_t          observed_nids;
    uint32_t          observed_tsbks;
    uint32_t          c4fm_nids;
    uint32_t          c4fm_tsbks;
    uint32_t          cqpsk_nids;
    uint32_t          cqpsk_tsbks;
    int               c4fm_score;
    int               cqpsk_score;
    uint32_t          reacquire_count;
    uint32_t          stream_ms;
    uint32_t          stream_fraction;
    uint32_t          stream_rate_hz;
    bool              locked;
} p25_demod_control_t;

int  p25_demod_preference_normalize(int preference);
float p25_demod_output_gain(demod_mode_t mode, bool inverted);
void p25_demod_control_init(p25_demod_control_t *control, int preference,
                            uint32_t now_ms, uint32_t valid_nids,
                            uint32_t valid_tsbks);

/* Returns true when selection/status changes. Reset DSP only if the selected
 * mode actually changes: locking the current winner must preserve timing.
 * Counts are
 * cumulative protocol results from the live DSD decoder. */
bool p25_demod_control_tick(p25_demod_control_t *control, uint32_t now_ms,
                            uint32_t valid_nids, uint32_t valid_tsbks,
                            bool call_active);

/* Advance acquisition using complete IQ actually delivered to DSP, not USB
 * startup, scan, or timeout wall time. Fractional milliseconds carry between
 * blocks so the 8192-pair production block does not shorten either window. */
bool p25_demod_control_feed(p25_demod_control_t *control,
                            uint32_t iq_pairs, uint32_t sample_rate_hz,
                            uint32_t valid_nids, uint32_t valid_tsbks,
                            bool call_active);

bool p25_demod_call_active(int last_p25_type, bool voice_active);

const char *p25_demod_mode_name(demod_mode_t mode);
const char *p25_demod_control_name(const p25_demod_control_t *control);

/* DSD consumes the same signed four-level stream from every DSP mode. Its
 * mod_* flags choose a second slicer/classifier, not the RF demodulator. */
void p25_demod_dsd_flags(demod_mode_t mode, int *c4fm, int *qpsk, int *gfsk);

#ifdef __cplusplus
}
#endif

#endif
