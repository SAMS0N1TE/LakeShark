/* SPDX-License-Identifier: GPL-3.0-or-later
 * Software read/DSP windows, never physical RF coverage. Caller serializes. */
#ifndef LS_IQ_COVERAGE_H
#define LS_IQ_COVERAGE_H
#include "radio_endpoint.h"
#ifdef __cplusplus
extern "C" {
#endif
#define LS_IQ_COVERAGE_CAPACITY 32u
#define LS_IQ_COVERAGE_GUARD_US UINT64_C(100000)
#define LS_IQ_COVERAGE_GUARD_BLOCKS 2u
typedef enum {
    LS_IQ_COVERAGE_UNKNOWN, LS_IQ_COVERAGE_RECEIVING,
    LS_IQ_COVERAGE_SETTLING, LS_IQ_COVERAGE_BUSY,
    LS_IQ_COVERAGE_UNAVAILABLE, LS_IQ_COVERAGE_STATE_COUNT
} ls_iq_coverage_state_t;
typedef struct {
    uint64_t sequence, started_us, ended_us, context_epoch, stream_epoch;
    uint64_t configured_center_hz, delivered_bytes, delivered_blocks;
    uint64_t settling_discarded_bytes;
    uint32_t nominal_rate_hz, plan_id, plan_step, plan_repeat;
    ls_iq_coverage_state_t state;
    bool context_known, calls_suppressed;
} ls_iq_coverage_interval_t;
typedef struct {
    bool active, timing_invalid, counter_saturated, calls_suppressed;
    uint32_t session_id;
    uint64_t started_us, stopped_us, last_update_us;
    uint64_t state_us[LS_IQ_COVERAGE_STATE_COUNT];
    uint64_t delivered_bytes, delivered_blocks, settling_discarded_bytes;
    uint64_t ring_rejected_bytes, transfer_error_events, submission_error_events;
    uint64_t flush_discarded_bytes, provenance_rejected_blocks;
    uint64_t retained_intervals, overwritten_intervals;
} ls_iq_coverage_status_t;
typedef struct {
    ls_iq_coverage_status_t status;
    ls_iq_coverage_interval_t intervals[LS_IQ_COVERAGE_CAPACITY];
    ls_radio_iq_read_receipt_t pending;
    ls_radio_iq_transport_t transport_baseline;
    uint64_t cursor_us, sequence, guard_started_us, guard_epoch, guard_context;
    uint32_t guard_blocks, plan_id, plan_step, plan_repeat;
    size_t head;
    ls_iq_coverage_state_t state;
    bool pending_accepted;
} ls_iq_coverage_t;
bool ls_iq_coverage_start(ls_iq_coverage_t *s,uint64_t now);
bool ls_iq_coverage_stop(ls_iq_coverage_t *s,uint64_t now);
void ls_iq_coverage_bind_plan(ls_iq_coverage_t *s,uint32_t id,uint32_t step,uint32_t repeat);
/* Close a boundary at the transition timestamp before changing these flags. */
void ls_iq_coverage_calls_suppressed(ls_iq_coverage_t *s,bool suppressed);
/* Before each tune/start/control attempt; guard restarts even at same tune. */
void ls_iq_coverage_boundary(ls_iq_coverage_t *s,ls_iq_coverage_state_t state,uint64_t now);
/* Before DSP: rejects unknown/mixed epochs and discards both >=2 blocks and
 * >=100ms after first confirmed post-reset read. This is policy, not measured
 * tuner settling. Rejected blocks must not reach the decoder. */
bool ls_iq_coverage_accept(ls_iq_coverage_t *s,const ls_radio_iq_read_receipt_t *r,size_t bytes);
/* After successful DSP only. RECEIVING interval is read-start..DSP-complete;
 * every intervening gap is UNKNOWN. Never interpolate RF coverage. */
bool ls_iq_coverage_delivered(ls_iq_coverage_t *s,size_t bytes,uint64_t now);
bool ls_iq_coverage_snapshot(const ls_iq_coverage_t *s,ls_iq_coverage_status_t *status,
    ls_iq_coverage_interval_t *entries,size_t capacity,size_t *count);
const char *ls_iq_coverage_state_name(ls_iq_coverage_state_t state);
#ifdef __cplusplus
}
#endif
#endif
