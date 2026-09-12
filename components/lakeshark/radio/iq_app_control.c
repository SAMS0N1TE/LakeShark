/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. Not librtlsdr - see UPSTREAM.md in this
   directory for which files here are third-party and which are ours. */
#include "iq_app_control.h"
#include "freertos/FreeRTOS.h"

#include <string.h>

/* rec_rx was captured spinning here while starving IDLE1. A bare
 * atomic flag lets a high-priority reader preempt its lower-priority owner
 * on the same core forever. Hold the FreeRTOS SMP critical section before
 * taking the flag so its owner cannot be descheduled until it releases it.
 * These sections only copy control state; no I/O or blocking calls occur. */
static portMUX_TYPE s_control_mux = portMUX_INITIALIZER_UNLOCKED;

static void control_lock(ls_iq_control_t *control)
{
    portENTER_CRITICAL(&s_control_mux);
    while (__atomic_test_and_set(&control->writer_lock, __ATOMIC_ACQUIRE)) {
    }
}

static void control_unlock(ls_iq_control_t *control)
{
    __atomic_clear(&control->writer_lock, __ATOMIC_RELEASE);
    portEXIT_CRITICAL(&s_control_mux);
}

void ls_iq_control_reset(ls_iq_control_t *control)
{
    if (!control) return;
    control_lock(control);
    __atomic_store_n(&control->pending, 0, __ATOMIC_RELEASE);
    control->center_hz = 0;
    control->gain_tenths_db = 0;
    control->bandwidth_hz = 0;
    control->fast_retune = false;
    control->tune_generation = 0;
    control->gain_generation = 0;
    control->effective_center_hz = 0;
    control->effective_gain_tenths_db = 0;
    control->effective_center_known = false;
    control->effective_gain_known = false;
    control->tune_state = LS_IQ_RESULT_UNKNOWN;
    control->gain_state = LS_IQ_RESULT_UNKNOWN;
    control->tune_error = LS_RADIO_OK;
    control->gain_error = LS_RADIO_OK;
    control->receiver_streaming = false;
    control->receiver_error = LS_RADIO_ERR_UNAVAILABLE;
    control_unlock(control);
}

void ls_iq_control_set_initial(ls_iq_control_t *control, uint32_t center_hz,
                               int gain_tenths_db, uint32_t bandwidth_hz)
{
    if (!control) return;
    control_lock(control);
    control->center_hz = center_hz;
    control->gain_tenths_db = gain_tenths_db;
    control->bandwidth_hz = bandwidth_hz;
    control_unlock(control);
}

void ls_iq_control_request_tune(ls_iq_control_t *control, uint32_t center_hz,
                                bool fast)
{
    if (!control) return;
    control_lock(control);
    __atomic_store_n(&control->center_hz, center_hz, __ATOMIC_RELAXED);
    __atomic_store_n(&control->fast_retune, fast, __ATOMIC_RELAXED);
    control->tune_generation++;
    control->tune_state = LS_IQ_RESULT_PENDING;
    control->tune_error = LS_RADIO_OK;
    __atomic_fetch_or(&control->pending, LS_IQ_CONTROL_TUNE,
                      __ATOMIC_RELEASE);
    control_unlock(control);
}

void ls_iq_control_request_gain(ls_iq_control_t *control,
                                int gain_tenths_db)
{
    if (!control) return;
    control_lock(control);
    __atomic_store_n(&control->gain_tenths_db, gain_tenths_db,
                     __ATOMIC_RELAXED);
    control->gain_generation++;
    control->gain_state = LS_IQ_RESULT_PENDING;
    control->gain_error = LS_RADIO_OK;
    __atomic_fetch_or(&control->pending, LS_IQ_CONTROL_GAIN,
                      __ATOMIC_RELEASE);
    control_unlock(control);
}

void ls_iq_control_request_bandwidth(ls_iq_control_t *control,
                                     uint32_t bandwidth_hz)
{
    if (!control) return;
    control_lock(control);
    __atomic_store_n(&control->bandwidth_hz, bandwidth_hz,
                     __ATOMIC_RELAXED);
    __atomic_fetch_or(&control->pending, LS_IQ_CONTROL_BANDWIDTH,
                      __ATOMIC_RELEASE);
    control_unlock(control);
}

bool ls_iq_control_take(ls_iq_control_t *control,
                        ls_iq_control_request_t *request)
{
    if (!control || !request) return false;
    control_lock(control);
    memset(request, 0, sizeof(*request));
    request->flags = __atomic_exchange_n(&control->pending, 0,
                                         __ATOMIC_ACQUIRE);
    if (request->flags & LS_IQ_CONTROL_TUNE) {
        request->center_hz = __atomic_load_n(&control->center_hz,
                                             __ATOMIC_RELAXED);
        request->fast_retune = __atomic_load_n(&control->fast_retune,
                                               __ATOMIC_RELAXED);
        request->tune_generation = control->tune_generation;
    }
    if (request->flags & LS_IQ_CONTROL_GAIN)
        request->gain_tenths_db = __atomic_load_n(&control->gain_tenths_db,
                                                  __ATOMIC_RELAXED);
    if (request->flags & LS_IQ_CONTROL_GAIN)
        request->gain_generation = control->gain_generation;
    if (request->flags & LS_IQ_CONTROL_BANDWIDTH)
        request->bandwidth_hz = __atomic_load_n(&control->bandwidth_hz,
                                                __ATOMIC_RELAXED);
    control_unlock(control);
    return request->flags != 0;
}

void ls_iq_control_status(ls_iq_control_t *control,
                          ls_iq_control_status_t *status)
{
    if (!control || !status) return;
    control_lock(control);
    status->requested_center_hz = control->center_hz;
    status->effective_center_hz = control->effective_center_hz;
    status->requested_gain_tenths_db = control->gain_tenths_db;
    status->effective_gain_tenths_db = control->effective_gain_tenths_db;
    status->effective_center_known = control->effective_center_known;
    status->effective_gain_known = control->effective_gain_known;
    status->tune_state = (ls_iq_result_state_t)control->tune_state;
    status->gain_state = (ls_iq_result_state_t)control->gain_state;
    status->tune_error = (ls_radio_err_t)control->tune_error;
    status->gain_error = (ls_radio_err_t)control->gain_error;
    status->receiver_streaming = control->receiver_streaming;
    status->receiver_error = (ls_radio_err_t)control->receiver_error;
    control_unlock(control);
}

static void note_tune_result(ls_iq_control_t *control, uint64_t requested_hz,
                             uint32_t generation, ls_radio_err_t error,
                             uint64_t actual_hz)
{
    control_lock(control);
    if (error == LS_RADIO_OK) {
        control->effective_center_hz = actual_hz;
        control->effective_center_known = true;
    }
    if ((generation == 0 && control->center_hz == requested_hz) ||
        (generation != 0 && control->tune_generation == generation)) {
        control->tune_state = error == LS_RADIO_OK
                                  ? LS_IQ_RESULT_EFFECTIVE
                                  : LS_IQ_RESULT_FAILED;
        control->tune_error = error;
    }
    control_unlock(control);
}

static void note_gain_result(ls_iq_control_t *control, int requested_gain,
                             uint32_t generation, ls_radio_err_t error,
                             int actual_gain)
{
    control_lock(control);
    if (error == LS_RADIO_OK) {
        control->effective_gain_tenths_db = actual_gain;
        control->effective_gain_known = true;
    }
    if ((generation == 0 && control->gain_tenths_db == requested_gain) ||
        (generation != 0 && control->gain_generation == generation)) {
        control->gain_state = error == LS_RADIO_OK
                                  ? LS_IQ_RESULT_EFFECTIVE
                                  : LS_IQ_RESULT_FAILED;
        control->gain_error = error;
    }
    control_unlock(control);
}

ls_radio_err_t ls_iq_control_configure(
    ls_iq_control_t *control, ls_radio_session_t *session,
    const ls_radio_iq_config_t *requested, ls_radio_iq_config_t *actual)
{
    if (!control || !requested || !actual) return LS_RADIO_ERR_INVALID;
    ls_radio_err_t error = ls_radio_iq_configure(session, requested, actual);
    note_tune_result(control, requested->center_hz, 0, error,
                     error == LS_RADIO_OK ? actual->center_hz : 0);
    note_gain_result(control, requested->gain_tenths_db, 0, error,
                     error == LS_RADIO_OK ? actual->gain_tenths_db : 0);
    return error;
}

ls_radio_err_t ls_iq_control_apply_tune(
    ls_iq_control_t *control, ls_radio_session_t *session,
    const ls_iq_control_request_t *request)
{
    if (!control || !request || !(request->flags & LS_IQ_CONTROL_TUNE))
        return LS_RADIO_ERR_INVALID;
    uint64_t actual_hz = 0;
    ls_radio_err_t error = ls_radio_iq_retune(
        session, request->center_hz, request->fast_retune, &actual_hz);
    note_tune_result(control, request->center_hz, request->tune_generation,
                     error, actual_hz);
    return error;
}

ls_radio_err_t ls_iq_control_apply_gain(
    ls_iq_control_t *control, ls_radio_session_t *session,
    const ls_iq_control_request_t *request)
{
    if (!control || !request || !(request->flags & LS_IQ_CONTROL_GAIN))
        return LS_RADIO_ERR_INVALID;
    int actual_gain = 0;
    ls_radio_err_t error = ls_radio_iq_set_gain(
        session, request->gain_tenths_db == 0 ? LS_RADIO_GAIN_AUTO
                                               : LS_RADIO_GAIN_MANUAL,
        request->gain_tenths_db, &actual_gain);
    note_gain_result(control, request->gain_tenths_db,
                     request->gain_generation, error, actual_gain);
    return error;
}

void ls_iq_control_note_tune_result(ls_iq_control_t *control,
                                    uint64_t requested_hz,
                                    ls_radio_err_t error,
                                    uint64_t actual_hz)
{
    if (!control) return;
    note_tune_result(control, requested_hz, 0, error, actual_hz);
}

void ls_iq_control_set_streaming(ls_iq_control_t *control, bool streaming,
                                 ls_radio_err_t error)
{
    if (!control) return;
    control_lock(control);
    control->receiver_streaming = streaming;
    control->receiver_error = error;
    control_unlock(control);
}

void ls_iq_control_receiver_lost(ls_iq_control_t *control,
                                 ls_radio_err_t error)
{
    if (!control) return;
    control_lock(control);
    control->receiver_streaming = false;
    control->receiver_error = error == LS_RADIO_OK
                                  ? LS_RADIO_ERR_UNAVAILABLE : error;
    control_unlock(control);
}
