/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. Not librtlsdr - see UPSTREAM.md in this
   directory for which files here are third-party and which are ours. */
#ifndef RADIO_HEALTH_H
#define RADIO_HEALTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "radio_endpoint.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RH_ABSENT = 0,
    RH_SETTLING,
    RH_OK,
    RH_STALLED,
    RH_RECOVERING,
    RH_POWER_CYCLING,
    RH_FAILED,
} rh_state_t;

typedef struct {
    void (*request_recovery)(const char *endpoint_id);
    bool (*power_cycle)(const char *endpoint_id);
    const char *power_cycle_endpoint_id;
} radio_health_hooks_t;

typedef struct {
    char endpoint_id[LS_RADIO_ENDPOINT_ID_MAX];
    rh_state_t state;
    int stall_s;
    uint32_t bytes_per_second;
    uint32_t recoveries;
    uint32_t attaches;
    uint32_t detaches;
} radio_health_snapshot_t;

void radio_health_init(const radio_health_hooks_t *hooks);
void radio_health_note_fault(const char *endpoint_id);
void radio_health_note_progress_reset(const char *endpoint_id);

bool radio_health_get(const char *endpoint_id,
                      radio_health_snapshot_t *out);
/* Use when the caller already owns a coherent endpoint snapshot. This avoids
 * collecting another large endpoint_info object on a constrained task stack. */
bool radio_health_get_for_endpoint(const ls_radio_endpoint_info_t *endpoint,
                                   radio_health_snapshot_t *out);
const char *radio_health_state_name(rh_state_t state);
int radio_health_report(const char *endpoint_id, char *buf, size_t len);

/* The firmware task calls this sampler. Keeping the state-machine tick public
 * makes attach, detach, recovery, and multiple endpoints host-testable. */
void radio_health_tick(void);

#ifdef __cplusplus
}
#endif

#endif
