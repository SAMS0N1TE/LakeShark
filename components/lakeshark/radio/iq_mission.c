/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "iq_mission.h"
#include <limits.h>
#include <string.h>
_Static_assert(sizeof(ls_iq_mission_t) <= 4096, "mission PSRAM budget");

static uint64_t sum(uint64_t a, uint64_t b, bool *saturated)
{
    if (b > UINT64_MAX - a) { *saturated = true; return UINT64_MAX; }
    return a + b;
}
bool ls_iq_mission_name_valid(const char *name)
{
    if (!name || !name[0]) return false;
    for (size_t i = 0; i < LS_IQ_MISSION_NAME_BYTES; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (!c) return true;
        if (c < 32 || c > 126) return false;
    }
    return false;
}
static bool clock_check(ls_iq_mission_t *m, uint64_t at)
{
    bool regressed = at < m->last_at_us;
    if (regressed) {
        m->status.clock_regressed = true;
        m->status.clock_regressions = sum(m->status.clock_regressions, 1,
                                           &m->status.counter_saturated);
    }
    m->last_at_us = at;
    return regressed;
}
static ls_iq_mission_entry_t *append(ls_iq_mission_t *m,
    ls_iq_mission_kind_t kind, const ls_iq_observation_t *s, uint64_t at)
{
    size_t i;
    if (m->status.retained_entries < LS_IQ_MISSION_CAPACITY) {
        i = (m->head + m->status.retained_entries++) % LS_IQ_MISSION_CAPACITY;
    } else {
        i = m->head; m->head = (m->head + 1) % LS_IQ_MISSION_CAPACITY;
        m->status.overwritten_entries = sum(m->status.overwritten_entries, 1,
                                             &m->status.counter_saturated);
    }
    m->sequence = sum(m->sequence, 1, &m->status.counter_saturated);
    ls_iq_mission_entry_t *e = &m->entries[i];
    *e = (ls_iq_mission_entry_t){.sequence=m->sequence, .kind=kind, .at_us=at,
        .generation=s->generation, .state=s->state,
        .configured_center_hz=s->configured_center_hz,
        .nominal_rate_hz=s->nominal_rate_hz};
    return e;
}
bool ls_iq_mission_start(ls_iq_mission_t *m, const char *name,
                         const ls_iq_observation_t *s, uint64_t at)
{
    if (!m || !s || m->status.active || m->status.mission_id == UINT32_MAX ||
        !ls_iq_mission_name_valid(name)) return false;
    uint32_t id = m->status.mission_id + 1;
    memset(m, 0, sizeof(*m));
    m->status.mission_id = id; m->status.active = true;
    memcpy(m->status.name, name, strlen(name) + 1);
    m->status.started_us = m->last_at_us = at;
    m->baseline = *s; /* Exclude samples delivered before operator start. */
    (void)append(m, LS_IQ_MISSION_START, s, at);
    return true;
}
void ls_iq_mission_observe(ls_iq_mission_t *m,
                           const ls_iq_observation_t *s, uint64_t at)
{
    if (!m || !s || !m->status.active || !s->known || !s->accepting) return;
    bool same = s->generation == m->baseline.generation;
    bool valid = !s->saturated && (!same ||
        (s->delivered_bytes >= m->baseline.delivered_bytes &&
         s->delivered_blocks >= m->baseline.delivered_blocks));
    uint64_t bytes = same ? s->delivered_bytes - m->baseline.delivered_bytes : s->delivered_bytes;
    uint64_t blocks = same ? s->delivered_blocks - m->baseline.delivered_blocks : s->delivered_blocks;
    if (valid && !blocks && !bytes) return; /* Repeated snapshots earn nothing. */
    bool regressed = clock_check(m, at);
    ls_iq_mission_entry_t *e = NULL;
    if (m->status.retained_entries) {
        e = &m->entries[(m->head + m->status.retained_entries - 1) % LS_IQ_MISSION_CAPACITY];
        if (e->kind != LS_IQ_MISSION_SEGMENT || e->generation != s->generation ||
            e->nominal_rate_hz != s->nominal_rate_hz ||
            e->configured_center_hz != s->configured_center_hz) e = NULL;
    }
    if (!e) e = append(m, LS_IQ_MISSION_SEGMENT, s, at);
    e->last_delivery_us = at; e->clock_regressed |= regressed;
    e->sample_accounting_known = valid && !e->counter_saturated;
    if (valid) {
        e->delivered_bytes = sum(e->delivered_bytes, bytes, &e->counter_saturated);
        e->delivered_blocks = sum(e->delivered_blocks, blocks, &e->counter_saturated);
        m->status.delivered_bytes = sum(m->status.delivered_bytes, bytes, &m->status.counter_saturated);
        m->status.delivered_blocks = sum(m->status.delivered_blocks, blocks, &m->status.counter_saturated);
    } else e->counter_saturated = true;
    e->sample_accounting_known &= !e->counter_saturated;
    m->status.counter_saturated |= e->counter_saturated;
    m->baseline = *s;
}
void ls_iq_mission_boundary(ls_iq_mission_t *m,
                            const ls_iq_observation_t *s, uint64_t at)
{
    if (!m || !s || !m->status.active) return;
    bool regressed = clock_check(m, at);
    append(m, LS_IQ_MISSION_BOUNDARY, s, at)->clock_regressed = regressed;
    m->baseline = *s;
}
bool ls_iq_mission_stop(ls_iq_mission_t *m, uint64_t at)
{
    if (!m || !m->status.active) return false;
    bool regressed = clock_check(m, at);
    append(m, LS_IQ_MISSION_STOP, &m->baseline, at)->clock_regressed = regressed;
    m->status.active = false; m->status.stopped_us = at;
    return true;
}
bool ls_iq_mission_snapshot(const ls_iq_mission_t *m,
                            ls_iq_mission_status_t *status,
                            ls_iq_mission_entry_t *entries, size_t cap, size_t *count)
{
    if (count) *count = 0;
    if (status) *status = (ls_iq_mission_status_t){0};
    if (!m || !status || !m->status.mission_id || (cap && !entries)) return false;
    *status = m->status;
    size_t n = cap < m->status.retained_entries ? cap : m->status.retained_entries;
    for (size_t i = 0; i < n; ++i)
        entries[i] = m->entries[(m->head + i) % LS_IQ_MISSION_CAPACITY];
    if (count) *count = n;
    return true;
}
const char *ls_iq_mission_kind_name(ls_iq_mission_kind_t kind)
{
    switch (kind) {
    case LS_IQ_MISSION_START: return "start";
    case LS_IQ_MISSION_SEGMENT: return "fm_segment";
    case LS_IQ_MISSION_BOUNDARY: return "boundary";
    case LS_IQ_MISSION_STOP: return "stop";
    }
    return "unknown";
}
