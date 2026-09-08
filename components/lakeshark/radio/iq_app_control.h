/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. Not librtlsdr - see UPSTREAM.md in this
   directory for which files here are third-party and which are ours. */
#ifndef LS_IQ_APP_CONTROL_H
#define LS_IQ_APP_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

#include "radio_endpoint.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    LS_IQ_CONTROL_TUNE      = 1u << 0,
    LS_IQ_CONTROL_GAIN      = 1u << 1,
    LS_IQ_CONTROL_BANDWIDTH = 1u << 2,
};

typedef struct {
    volatile bool writer_lock;
    volatile uint32_t pending;
    volatile uint32_t center_hz;
    volatile int gain_tenths_db;
    volatile uint32_t bandwidth_hz;
    volatile bool fast_retune;
    volatile uint32_t tune_generation;
    volatile uint32_t gain_generation;

    volatile uint64_t effective_center_hz;
    volatile int effective_gain_tenths_db;
    volatile bool effective_center_known;
    volatile bool effective_gain_known;
    volatile int tune_state;
    volatile int gain_state;
    volatile int tune_error;
    volatile int gain_error;
    volatile bool receiver_streaming;
    volatile int receiver_error;
} ls_iq_control_t;

typedef enum {
    LS_IQ_RESULT_UNKNOWN = 0,
    LS_IQ_RESULT_PENDING,
    LS_IQ_RESULT_EFFECTIVE,
    LS_IQ_RESULT_FAILED,
} ls_iq_result_state_t;

typedef struct {
    uint32_t flags;
    uint32_t center_hz;
    int gain_tenths_db;
    uint32_t bandwidth_hz;
    bool fast_retune;
    uint32_t tune_generation;
    uint32_t gain_generation;
} ls_iq_control_request_t;

typedef struct {
    uint64_t requested_center_hz;
    uint64_t effective_center_hz;
    int requested_gain_tenths_db;
    int effective_gain_tenths_db;
    bool effective_center_known;
    bool effective_gain_known;
    ls_iq_result_state_t tune_state;
    ls_iq_result_state_t gain_state;
    ls_radio_err_t tune_error;
    ls_radio_err_t gain_error;
    bool receiver_streaming;
    ls_radio_err_t receiver_error;
} ls_iq_control_status_t;

void ls_iq_control_reset(ls_iq_control_t *control);
void ls_iq_control_set_initial(ls_iq_control_t *control, uint32_t center_hz,
                               int gain_tenths_db, uint32_t bandwidth_hz);
void ls_iq_control_request_tune(ls_iq_control_t *control, uint32_t center_hz,
                                bool fast);
void ls_iq_control_request_gain(ls_iq_control_t *control,
                                int gain_tenths_db);
void ls_iq_control_request_bandwidth(ls_iq_control_t *control,
                                     uint32_t bandwidth_hz);
bool ls_iq_control_take(ls_iq_control_t *control,
                        ls_iq_control_request_t *request);
void ls_iq_control_status(ls_iq_control_t *control,
                          ls_iq_control_status_t *status);

ls_radio_err_t ls_iq_control_configure(
    ls_iq_control_t *control, ls_radio_session_t *session,
    const ls_radio_iq_config_t *requested, ls_radio_iq_config_t *actual);
ls_radio_err_t ls_iq_control_apply_tune(
    ls_iq_control_t *control, ls_radio_session_t *session,
    const ls_iq_control_request_t *request);
ls_radio_err_t ls_iq_control_apply_gain(
    ls_iq_control_t *control, ls_radio_session_t *session,
    const ls_iq_control_request_t *request);
void ls_iq_control_note_tune_result(ls_iq_control_t *control,
                                    uint64_t requested_hz,
                                    ls_radio_err_t error,
                                    uint64_t actual_hz);
void ls_iq_control_set_streaming(ls_iq_control_t *control, bool streaming,
                                 ls_radio_err_t error);
void ls_iq_control_receiver_lost(ls_iq_control_t *control,
                                 ls_radio_err_t error);

#ifdef __cplusplus
}
#endif

#endif
