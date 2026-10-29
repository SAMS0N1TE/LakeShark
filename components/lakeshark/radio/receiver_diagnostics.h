/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. Not librtlsdr - see UPSTREAM.md in this
   directory for which files here are third-party and which are ours. */
#ifndef LS_RECEIVER_DIAGNOSTICS_H
#define LS_RECEIVER_DIAGNOSTICS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "radio_endpoint.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LS_RECEIVER_DIAG_APP_MAX       12
#define LS_RECEIVER_DIAG_LABEL_MAX     16
#define LS_RECEIVER_DIAG_ERROR_MAX     16

typedef enum {
    LS_RECEIVER_RX_UNKNOWN = 0,
    LS_RECEIVER_RX_DISCONNECTED,
    LS_RECEIVER_RX_PARKED,
    LS_RECEIVER_RX_IDLE,
    LS_RECEIVER_RX_ACTIVE,
} ls_receiver_rx_state_t;

/* A bounded, read-only copy of receiver state.  Every value which may not
 * exist has a separate known bit; zero remains a real counter/value and is
 * never overloaded to mean unavailable. */
typedef struct {
    bool app_known;
    char app[LS_RECEIVER_DIAG_APP_MAX];
    bool parked_known;
    bool parked;
    ls_receiver_rx_state_t rx_state;
    char decoder[LS_RECEIVER_DIAG_LABEL_MAX];
    char demod[LS_RECEIVER_DIAG_LABEL_MAX];

    bool endpoint_known;
    char endpoint_id[LS_RADIO_ENDPOINT_ID_MAX];
    char owner[LS_RADIO_OWNER_MAX];
    bool endpoint_present;
    bool endpoint_streaming;
    bool health_known;
    char health[LS_RECEIVER_DIAG_LABEL_MAX];
    bool health_rate_known;
    uint32_t health_bytes_per_second;

    bool requested_frequency_known;
    uint64_t requested_frequency_hz;
    bool effective_frequency_known;
    uint64_t effective_frequency_hz;
    bool requested_gain_known;
    int requested_gain_tenths_db;
    bool effective_gain_known;
    int effective_gain_tenths_db;

    bool iq_total_known;
    uint64_t iq_bytes_total;
    bool iq_rate_known;
    uint32_t iq_bytes_per_second;
    bool frame_count_known;
    uint32_t frame_count;
    bool valid_count_known;
    uint32_t valid_count;
    bool failed_count_known;
    uint32_t failed_count;
    bool control_count_known;
    uint32_t control_count;
    bool voice_count_known;
    uint32_t voice_count;
    bool audio_drop_count_known;
    uint32_t audio_drop_count;
    bool receiver_error_known;
    char receiver_error[LS_RECEIVER_DIAG_ERROR_MAX];

    bool memory_known;
    uint32_t internal_free;
    uint32_t internal_largest;
    uint32_t dma_free;
    uint32_t dma_largest;

    /* Snapshot time is monotonic uptime, never wall-clock RF time. These
     * optional keys describe the selected IQ endpoint's applied settings. */
    bool captured_known;
    uint64_t captured_us;
    bool source_known;
    char source[LS_RECEIVER_DIAG_LABEL_MAX];
    bool sample_rate_known;
    uint32_t sample_rate_hz;
    bool config_known;
    uint32_t bandwidth_hz;
    int gain_mode;
    bool context_epoch_known;
    uint64_t context_epoch;
    bool consumer_epoch_known;
    uint64_t consumer_epoch;
    bool iq_epoch_known;
    uint64_t iq_epoch;
    /* No current decoder publishes a reset epoch. Keep its deltas unknown. */
    bool decoder_epoch_known;
    uint64_t decoder_epoch;
} ls_receiver_diag_t;

enum {
    LS_TRIAL_APP = 1u << 0, LS_TRIAL_SOURCE = 1u << 1,
    LS_TRIAL_MODE = 1u << 2, LS_TRIAL_TUNE = 1u << 3,
    LS_TRIAL_RATE = 1u << 4, LS_TRIAL_CONFIG = 1u << 5,
    LS_TRIAL_CONTEXT = 1u << 6, LS_TRIAL_IQ_EPOCH = 1u << 7,
    LS_TRIAL_DECODER_EPOCH = 1u << 8,
    LS_TRIAL_CONTEXT_KEYS = (1u << 7) - 1,
};
enum {
    LS_TRIAL_IQ = 1u << 0, LS_TRIAL_FRAME = 1u << 1,
    LS_TRIAL_VALID = 1u << 2, LS_TRIAL_FAILED = 1u << 3,
    LS_TRIAL_CONTROL = 1u << 4, LS_TRIAL_VOICE = 1u << 5,
    LS_TRIAL_AUDIO_DROP = 1u << 6,
};
#define LS_RECEIVER_TRIAL_MAX_US UINT64_C(120000000)
#define LS_RECEIVER_TRIAL_FRESH_US UINT64_C(3000000)
typedef struct {
    bool baseline_set, compared, time_invalid, stale, inactive;
    uint32_t changed, unknown, delta_known, counter_reset;
    uint64_t elapsed_us, iq_delta;
    uint32_t frame_delta, valid_delta, failed_delta, control_delta;
    uint32_t voice_delta, audio_drop_delta;
    ls_receiver_diag_t before, after;
} ls_receiver_trial_t;

/* Pure bounded copy/compare, never control a receiver. Rebaseline is explicit.
 * Any decrease is reset-or-wrap, never modular subtraction. */
void ls_receiver_trial_baseline(ls_receiver_trial_t *trial,
                                const ls_receiver_diag_t *snapshot);
void ls_receiver_trial_compare(ls_receiver_trial_t *trial,
                               const ls_receiver_diag_t *snapshot,
                               uint64_t now_us);
const char *ls_receiver_trial_state(const ls_receiver_trial_t *trial);
int ls_receiver_trial_format(const ls_receiver_trial_t *trial,
                             char *out, size_t out_size);
/* One JSON object; returns -1 and clears output if the buffer is too small. */
int ls_receiver_trial_json(const ls_receiver_trial_t *trial,
                           char *out, size_t out_size);

typedef enum {
    LS_RECEIVER_CHECK_UNKNOWN = 0,
    LS_RECEIVER_CHECK_INFO,
    LS_RECEIVER_CHECK_ATTENTION,
} ls_receiver_check_level_t;

/* Advice explains measured state; it never declares a mode RF-verified.
 * Strings are static, so this read-only assessment allocates nothing. */
typedef struct {
    ls_receiver_check_level_t level;
    const char *title;
    const char *reason;
    const char *next;
} ls_receiver_check_t;

ls_receiver_check_t ls_receiver_diag_assess(const ls_receiver_diag_t *snapshot);
/* Native FM pager sync/batch and REC artifact totals are not validated RF
   frames. Preserve their native counters while qualifying the guided UI. */
void ls_receiver_diag_ui_counters(ls_receiver_diag_t *snapshot);

const char *ls_receiver_rx_state_name(ls_receiver_rx_state_t state);
int ls_receiver_diag_format(const ls_receiver_diag_t *snapshot,
                            char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif
