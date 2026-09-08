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
} ls_receiver_diag_t;

const char *ls_receiver_rx_state_name(ls_receiver_rx_state_t state);
int ls_receiver_diag_format(const ls_receiver_diag_t *snapshot,
                            char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif
