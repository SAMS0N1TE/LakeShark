/* SPDX-License-Identifier: GPL-3.0-or-later
 * Passive, current-boot operator session. Wall-clock RF coverage, sample
 * continuity and sample RF frequency remain unknown, even between deliveries.
 * Caller serializes access and supplies monotonic completion/control times. */
#ifndef LS_IQ_MISSION_H
#define LS_IQ_MISSION_H
#include "iq_observation.h"
#ifdef __cplusplus
extern "C" {
#endif
#define LS_IQ_MISSION_CAPACITY 32u
#define LS_IQ_MISSION_NAME_BYTES 32u
typedef enum {
    LS_IQ_MISSION_START, LS_IQ_MISSION_SEGMENT,
    LS_IQ_MISSION_BOUNDARY, LS_IQ_MISSION_STOP
} ls_iq_mission_kind_t;
typedef struct {
    uint64_t sequence, at_us, last_delivery_us;
    uint64_t configured_center_hz, delivered_bytes, delivered_blocks;
    uint32_t generation, nominal_rate_hz;
    ls_iq_mission_kind_t kind;
    ls_iq_observation_state_t state;
    bool sample_accounting_known, counter_saturated, clock_regressed;
} ls_iq_mission_entry_t;
typedef struct {
    bool active, counter_saturated, clock_regressed;
    uint32_t mission_id;
    char name[LS_IQ_MISSION_NAME_BYTES];
    uint64_t started_us, stopped_us, delivered_bytes, delivered_blocks;
    uint64_t overwritten_entries, clock_regressions;
    size_t retained_entries;
} ls_iq_mission_status_t;
typedef struct {
    ls_iq_mission_status_t status;
    ls_iq_mission_entry_t entries[LS_IQ_MISSION_CAPACITY];
    ls_iq_observation_t baseline;
    uint64_t sequence, last_at_us;
    size_t head;
} ls_iq_mission_t;
/* Printable ASCII, 1..31 bytes; no truncation. A stopped session is replaced
 * only by another explicit start; there is no persistence or multi-session DB. */
bool ls_iq_mission_name_valid(const char *name);
bool ls_iq_mission_start(ls_iq_mission_t *m, const char *name,
                         const ls_iq_observation_t *baseline, uint64_t at_us);
bool ls_iq_mission_stop(ls_iq_mission_t *m, uint64_t at_us);
/* Call once after successful observation delivery, and once at every exact
 * owner transition. Never synthesize delivery from leases, polls or tuning. */
void ls_iq_mission_observe(ls_iq_mission_t *m,
                           const ls_iq_observation_t *s, uint64_t at_us);
void ls_iq_mission_boundary(ls_iq_mission_t *m,
                            const ls_iq_observation_t *s, uint64_t at_us);
bool ls_iq_mission_snapshot(const ls_iq_mission_t *m,
                            ls_iq_mission_status_t *status,
                            ls_iq_mission_entry_t *entries, size_t capacity,
                            size_t *count);
const char *ls_iq_mission_kind_name(ls_iq_mission_kind_t kind);
#ifdef __cplusplus
}
#endif
#endif
