/* SPDX-License-Identifier: GPL-3.0-or-later
 * Bounded receive-only control schedule. Dwell is policy time, never coverage.
 * Owner serializes, validates the actual endpoint and performs returned actions. */
#ifndef LS_RX_PLAN_H
#define LS_RX_PLAN_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define LS_RX_PLAN_STEPS 8u
#define LS_RX_PLAN_REPEATS 4u
#define LS_RX_PLAN_MAX_US UINT64_C(120000000)
#define LS_RX_PLAN_PHASE_US UINT64_C(2000000)
typedef struct { uint32_t center_hz, dwell_ms; } ls_rx_plan_step_t;
typedef enum {
    LS_RX_PLAN_LOADED, LS_RX_PLAN_QUEUED, LS_RX_PLAN_TUNING,
    LS_RX_PLAN_SETTLING, LS_RX_PLAN_DWELLING, LS_RX_PLAN_RESTORING,
    LS_RX_PLAN_COMPLETED, LS_RX_PLAN_STOPPED, LS_RX_PLAN_CANCELLED, LS_RX_PLAN_FAILED
} ls_rx_plan_state_t;
typedef enum {
    LS_RX_PLAN_OK, LS_RX_PLAN_OPERATOR_STOP, LS_RX_PLAN_MANUAL_OVERRIDE,
    LS_RX_PLAN_CONTEXT_LOST, LS_RX_PLAN_TUNE_FAILED, LS_RX_PLAN_SETTLE_TIMEOUT,
    LS_RX_PLAN_DEADLINE, LS_RX_PLAN_CLOCK_INVALID, LS_RX_PLAN_RESTORE_FAILED
} ls_rx_plan_reason_t;
typedef struct {
    ls_rx_plan_step_t planned;
    uint32_t attempts, completed;
    uint64_t first_tune_us, last_tune_us, last_dwell_started_us, last_dwell_ended_us;
    uint64_t delivered_bytes,delivered_blocks,software_window_us,first_delivery_us,last_delivery_us;
    uint32_t nominal_rate_hz;
    bool counter_saturated,timing_invalid,rate_mixed;
    bool dwell_started, dwell_ended;
} ls_rx_plan_step_status_t;
typedef struct {
    uint32_t plan_id, step_count, repeat_count, current_step, current_repeat;
    char name[32];
    ls_rx_plan_state_t state;
    ls_rx_plan_reason_t reason;
    bool active, started, ended, clock_invalid, restore_attempted, restored,detail_truncated;
    uint32_t restore_center_hz;
    uint64_t started_us, ended_us;
    ls_rx_plan_step_status_t steps[LS_RX_PLAN_STEPS];
} ls_rx_plan_status_t;
typedef struct {
    ls_rx_plan_status_t status;
    uint64_t last_us, phase_deadline_us;
    ls_rx_plan_state_t after_restore;
    bool stop_requested, changed_receiver;
} ls_rx_plan_t;
typedef enum { LS_RX_PLAN_ACTION_NONE, LS_RX_PLAN_ACTION_TUNE, LS_RX_PLAN_ACTION_RESTORE } ls_rx_plan_action_t;
bool ls_rx_plan_load(ls_rx_plan_t *p,const char *name,const ls_rx_plan_step_t *steps,size_t n,uint32_t repeats);
bool ls_rx_plan_start(ls_rx_plan_t *p,uint32_t restore_hz,uint64_t now);
bool ls_rx_plan_stop(ls_rx_plan_t *p);
void ls_rx_plan_cancel(ls_rx_plan_t *p,ls_rx_plan_reason_t why,uint64_t now);
ls_rx_plan_action_t ls_rx_plan_poll(ls_rx_plan_t *p,uint64_t now,bool context_ok,uint32_t *hz);
void ls_rx_plan_tuned(ls_rx_plan_t *p,bool success,uint64_t now);
void ls_rx_plan_qualified(ls_rx_plan_t *p,uint64_t now);
/* Exact post-fence DSP delivery totals, independent of the recent interval
 * ring. Window sum is software read-start..DSP-done, never continuous RF. */
void ls_rx_plan_qualified_block(ls_rx_plan_t *p,uint64_t now,size_t bytes,uint64_t read_started_us,uint32_t nominal_rate_hz);
bool ls_rx_plan_nominal_us(const ls_rx_plan_step_status_t *step,uint64_t *out);
void ls_rx_plan_restore_result(ls_rx_plan_t *p,bool success,uint64_t now);
const char *ls_rx_plan_state_name(ls_rx_plan_state_t state);
const char *ls_rx_plan_reason_name(ls_rx_plan_reason_t why);
#ifdef __cplusplus
}
#endif
#endif
