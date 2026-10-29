/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "iq_observation.h"
#include <limits.h>

static uint64_t add(uint64_t a, uint64_t b, bool *saturated)
{
    if (b > UINT64_MAX - a) { *saturated = true; return UINT64_MAX; }
    return a + b;
}

void ls_iq_observation_begin(ls_iq_observation_t *s, uint32_t rate,
                             uint64_t center)
{
    if (!s) return;
    uint32_t generation = s->generation;
    *s = (ls_iq_observation_t){0};
    s->generation = generation == UINT32_MAX ? UINT32_MAX : generation + 1;
    s->saturated = generation == UINT32_MAX;
    s->nominal_rate_hz = rate;
    s->configured_center_hz = center;
    s->known = s->accepting = rate != 0;
    s->state = rate ? LS_IQ_OBS_DELIVERING : LS_IQ_OBS_UNAVAILABLE;
}

void ls_iq_observation_close(ls_iq_observation_t *s,
                             ls_iq_observation_state_t reason)
{
    if (!s) return;
    s->accepting = false;
    s->state = reason == LS_IQ_OBS_DELIVERING ? LS_IQ_OBS_UNAVAILABLE : reason;
}

bool ls_iq_observation_deliver(ls_iq_observation_t *s, size_t bytes,
                               uint64_t at)
{
    if (!s || !s->known || !s->accepting || !bytes || (bytes & 1u)) return false;
    if (!s->delivered_blocks) s->first_delivery_us = at;
    s->last_delivery_us = at;
    s->delivered_bytes = add(s->delivered_bytes, (uint64_t)bytes, &s->saturated);
    s->delivered_blocks = add(s->delivered_blocks, 1, &s->saturated);
    return true;
}

bool ls_iq_observation_nominal_us(const ls_iq_observation_t *s, uint64_t *out)
{
    if (!s || !out || !s->known || !s->nominal_rate_hz || s->saturated) return false;
    uint64_t pairs = s->delivered_bytes / 2, rate = s->nominal_rate_hz;
    uint64_t whole = pairs / rate, part = pairs % rate;
    if (whole > UINT64_MAX / 1000000u) return false;
    uint64_t base = whole * 1000000u, fraction = part * 1000000u / rate;
    if (fraction > UINT64_MAX - base) return false;
    *out = base + fraction;
    return true;
}

const char *ls_iq_observation_state_name(ls_iq_observation_state_t state)
{
    switch (state) {
    case LS_IQ_OBS_DELIVERING: return "delivering";
    case LS_IQ_OBS_TUNE: return "tune_boundary";
    case LS_IQ_OBS_MODE: return "mode_boundary";
    case LS_IQ_OBS_READ_ERROR: return "read_error";
    case LS_IQ_OBS_RECEIVER_ERROR: return "receiver_error";
    case LS_IQ_OBS_STOPPED: return "stopped";
    case LS_IQ_OBS_UNSUPPORTED: return "unsupported_path";
    case LS_IQ_OBS_UNAVAILABLE: return "unavailable";
    }
    return "unavailable";
}
