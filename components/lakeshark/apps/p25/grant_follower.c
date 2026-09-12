#include "grant_follower.h"

#include <string.h>

/* TSBK grants were parsed and the resolved frequency was written into dsd_state and then never read. */

#define P25_GRANT_DEFAULT_HANG_US 2000000LL

static bool talkgroup_in_list(const p25_grant_follower_t *f, uint16_t tg)
{
    for (size_t i = 0; i < f->filter_count; i++)
        if (f->filter[i] == tg) return true;
    return false;
}

static bool filter_allows(const p25_grant_follower_t *f, uint16_t tg)
{
    switch (f->filter_mode) {
    case P25_GRANT_FILTER_ALLOW: return talkgroup_in_list(f, tg);
    case P25_GRANT_FILTER_DENY:  return !talkgroup_in_list(f, tg);
    case P25_GRANT_FILTER_OFF:
    default:                     return true;
    }
}

/* linear scan is fine - the table caps at P25_GRANT_TG_STATE_MAX (32),
 * and grants land at most a few per second. If a future site pushes past that
 * we'd want a hash, but the memory bound matters more here than the speed. */
static p25_grant_tg_state_t *tg_state_find(p25_grant_follower_t *f, uint16_t tg)
{
    for (size_t i = 0; i < f->tg_state_count; i++)
        if (f->tg_state[i].talkgroup == tg) return &f->tg_state[i];
    return NULL;
}

/* evict the oldest entry (smallest last_seen_us) when the table is
 * full. "Oldest by last_seen" beats "oldest by insertion" because a busy TG
 * that keeps landing grants deserves to stay in the table and a quiet TG
 * that skipped once weeks ago does not. If the evicted entry had an active
 * skip we lose the mute, but the alternative is to grow without bound. */
static p25_grant_tg_state_t *tg_state_evict_oldest(p25_grant_follower_t *f)
{
    size_t oldest = 0;
    int64_t oldest_ts = f->tg_state[0].last_seen_us;
    for (size_t i = 1; i < f->tg_state_count; i++) {
        if (f->tg_state[i].last_seen_us < oldest_ts) {
            oldest = i;
            oldest_ts = f->tg_state[i].last_seen_us;
        }
    }
    f->tg_state_evictions++;
    return &f->tg_state[oldest];
}

static p25_grant_tg_state_t *tg_state_get_or_make(p25_grant_follower_t *f,
                                                  uint16_t tg)
{
    p25_grant_tg_state_t *e = tg_state_find(f, tg);
    if (e) return e;
    if (f->tg_state_count < P25_GRANT_TG_STATE_MAX) {
        e = &f->tg_state[f->tg_state_count++];
    } else {
        e = tg_state_evict_oldest(f);
    }
    memset(e, 0, sizeof(*e));
    e->talkgroup = tg;
    return e;
}

bool p25_grant_tg_is_skipped(const p25_grant_follower_t *f,
                             uint16_t talkgroup, int64_t now_us)
{
    if (!f) return false;
    for (size_t i = 0; i < f->tg_state_count; i++) {
        const p25_grant_tg_state_t *e = &f->tg_state[i];
        if (e->talkgroup != talkgroup) continue;
        if (e->skip_expires_us == 0) return false;
        return now_us < e->skip_expires_us;
    }
    return false;
}

size_t p25_grant_tg_state_count(const p25_grant_follower_t *f)
{
    return f ? f->tg_state_count : 0;
}

const p25_grant_tg_state_t *p25_grant_tg_state(const p25_grant_follower_t *f,
                                               size_t index)
{
    if (!f || index >= f->tg_state_count) return NULL;
    return &f->tg_state[index];
}

static void retune_to_control(p25_grant_follower_t *f)
{
    f->state = P25_GRANT_ON_CONTROL;
    f->traffic_hz = 0;
    f->talkgroup = 0;
    f->source = 0;
    memset(&f->active_call, 0, sizeof(f->active_call));
    f->last_activity_us = 0;
    f->receive_state = P25_RX_CONTROL_SEARCH;
    f->returned_count++;
    if (f->retune) f->retune(f->user, f->control_hz, false);
}

void p25_grant_init(p25_grant_follower_t *f,
                    uint64_t control_hz,
                    p25_grant_retune_fn retune,
                    void *user)
{
    memset(f, 0, sizeof(*f));
    f->control_hz = control_hz;
    f->retune = retune;
    f->user = user;
    f->state = P25_GRANT_ON_CONTROL;
    f->hang_us = P25_GRANT_DEFAULT_HANG_US;
    f->filter_mode = P25_GRANT_FILTER_OFF;
    f->leave_on_encrypted = true;
    f->encrypted_skip_us =
        (int64_t)P25_GRANT_DEFAULT_ENCRYPTED_SKIP_MS * 1000LL;
}

void p25_grant_set_control(p25_grant_follower_t *f, uint64_t control_hz)
{
    if (!f || !control_hz || control_hz > UINT32_MAX) return;
    if (f->control_hz != control_hz) {
        memset(f->tg_state, 0, sizeof(f->tg_state));
        f->tg_state_count = 0;
        f->system_valid = false;
        f->control_nac_valid = false;
        memset(&f->observed_grant, 0, sizeof(f->observed_grant));
        f->receive_state = P25_RX_CONTROL_SEARCH;
    }
    f->control_hz = control_hz;
}

void p25_grant_set_hang_ms(p25_grant_follower_t *f, unsigned int ms)
{
    f->hang_us = (int64_t)ms * 1000LL;
}

void p25_grant_set_filter(p25_grant_follower_t *f,
                          p25_grant_filter_mode_t mode,
                          const uint16_t *talkgroups, size_t count)
{
    f->filter_mode = mode;
    f->filter_count = 0;
    if (!talkgroups) return;
    for (size_t i = 0; i < count && f->filter_count < P25_GRANT_FILTER_MAX; i++)
        f->filter[f->filter_count++] = talkgroups[i];
}

void p25_grant_set_leave_on_encrypted(p25_grant_follower_t *f, bool enabled)
{
    if (!f) return;
    f->leave_on_encrypted = enabled;
}

void p25_grant_set_encrypted_skip_ms(p25_grant_follower_t *f, unsigned int ms)
{
    if (!f) return;
    f->encrypted_skip_us =
        (int64_t)p25_controls_clamp_encrypted_skip_ms(ms) * 1000LL;
}

static bool on_grant(p25_grant_follower_t *f,
                     uint16_t talkgroup, uint32_t source,
                     uint64_t freq_hz, int64_t now_us,
                     const p25_call_info_t *call)
{
    if (!f) return false;
    if (freq_hz == 0 || freq_hz > UINT32_MAX || talkgroup == 0) return false;

    if (f->state == P25_GRANT_ON_TRAFFIC) {
        if (f->talkgroup == talkgroup && f->traffic_hz == freq_hz) {
            /* A GRANT_UPDATE for the call already being decoded. Refresh
             * the hang timer (it proves the caller is still keyed) and
             * record the source if the update carries one, but do not
             * retune - retuning here loses audio. */
            if (source) f->source = f->active_call.source = source;
            f->last_activity_us = now_us;
            f->duplicate_grants++;
            return false;
        }
        /* Different talkgroup while a call is in progress. Do not thrash:
         * ignoring is what a scanner does. */
        f->foreign_grants++;
        return false;
    }

    if (!filter_allows(f, talkgroup)) {
        f->filtered_grants++;
        return false;
    }

    if (p25_grant_tg_is_skipped(f, talkgroup, now_us)) {
        f->encrypted_skips++;
        return false;
    }

    f->state = P25_GRANT_ON_TRAFFIC;
    f->traffic_hz = freq_hz;
    f->talkgroup = talkgroup;
    f->source = source;
    memset(&f->active_call, 0, sizeof(f->active_call));
    f->active_call.carrier_hz = freq_hz;
    f->active_call.talkgroup = talkgroup;
    f->active_call.source = source;
    f->active_call.slots_per_carrier = 1;
    f->active_call.support = P25_CALL_PHASE1;
    if (call) f->active_call = *call;
    f->receive_state = P25_RX_TRAFFIC_WAIT;
    f->last_activity_us = now_us;
    f->followed_count++;
    if (f->retune) f->retune(f->user, freq_hz, true);
    return true;
}

bool p25_grant_on_grant(p25_grant_follower_t *f,
                        uint16_t talkgroup, uint32_t source,
                        uint64_t freq_hz, int64_t now_us)
{
    return on_grant(f, talkgroup, source, freq_hz, now_us, NULL);
}

void p25_grant_on_voice(p25_grant_follower_t *f, int64_t now_us)
{
    if (!f) return;
    if (f->state == P25_GRANT_ON_TRAFFIC) f->last_activity_us = now_us;
}

bool p25_grant_on_terminator(p25_grant_follower_t *f, int64_t now_us)
{
    (void)now_us;
    if (!f) return false;
    if (f->state != P25_GRANT_ON_TRAFFIC) return false;
    retune_to_control(f);
    return true;
}

bool p25_grant_force_return_to_control(p25_grant_follower_t *f)
{
    if (!f) return false;
    if (f->state != P25_GRANT_ON_TRAFFIC) return false;
    retune_to_control(f);
    return true;
}

bool p25_grant_tick(p25_grant_follower_t *f, int64_t now_us)
{
    if (!f) return false;
    if (f->state != P25_GRANT_ON_TRAFFIC) return false;
    if (now_us - f->last_activity_us < f->hang_us) return false;
    retune_to_control(f);
    return true;
}

bool p25_grant_from_state(p25_grant_follower_t *f, const dsd_state *state,
                          int64_t now_us)
{
    if (!f || !state) return false;
    if (state->p25_grant_batch_valid) {
        if (!p25_grant_take_batch(f, state)) return false;
        bool tuned = false;
        for (unsigned int i = 0; i < state->p25_grant_count; i++)
            tuned |= p25_grant_on_call(f, &state->p25_grants[i], now_us);
        return tuned;
    }
    uint8_t op = state->p25_tsbk_last_opcode;
    /* Voice-grant whitelist, matching the set_voice_grant call sites in
     * p25_tsbk.c: 0x00 GRP_V_CH_GRANT, 0x02 GRP_V_CH_GRANT_UPDATE, 0x03
     * GRP_V_CH_GRANT_UPDT_EXP (). Data grants, telephone interconnect,
     * unit-to-unit, and every status/registration broadcast never write
     * the voice-grant fields, so this whitelist is the structural gate
     * that keeps the follower on the control channel for those opcodes. */
    if (op != 0x00 && op != 0x02 && op != 0x03) return false;
    return p25_grant_on_grant(f,
                              state->p25_tsbk_talkgroup,
                              state->p25_tsbk_source,
                              state->p25_tsbk_frequency_hz,
                              now_us);
}

bool p25_grant_take_batch(p25_grant_follower_t *f, const dsd_state *s)
{
    if (!f || !s || !s->p25_grant_batch_valid ||
        s->p25_grant_count > P25_GRANTS_PER_TSDU) return false;
    if (f->consumed_grant_valid &&
        f->consumed_grant_generation == s->p25_grant_generation) return false;
    f->consumed_grant_generation = s->p25_grant_generation;
    f->consumed_grant_valid = true;
    if (s->p25_control_nac_valid) {
        if (f->control_nac_valid && f->control_nac != s->p25_control_nac) {
            (void)p25_grant_force_return_to_control(f);
            memset(f->tg_state, 0, sizeof(f->tg_state));
            f->tg_state_count = 0;
            f->system_valid = false;
            memset(&f->observed_grant, 0, sizeof(f->observed_grant));
        }
        f->control_nac = s->p25_control_nac;
        f->control_nac_valid = true;
    }
    if (s->p25_net_valid) {
        if (f->system_valid && (f->system_wacn != s->p25_tsbk_wacn ||
                               f->system_sysid != s->p25_tsbk_sysid)) {
            /* TG numbers and encryption skips are system-local. */
            (void)p25_grant_force_return_to_control(f);
            memset(f->tg_state, 0, sizeof(f->tg_state));
            f->tg_state_count = 0;
            memset(&f->observed_grant, 0, sizeof(f->observed_grant));
        }
        f->system_valid = true;
        f->system_wacn = s->p25_tsbk_wacn;
        f->system_sysid = s->p25_tsbk_sysid;
    }
    return true;
}

void p25_grant_observe(p25_grant_follower_t *f, const p25_call_info_t *call)
{
    if (!f || !call) return;
    f->observed_grant = *call;
    if (call->support == P25_CALL_UNSUPPORTED) f->unsupported_grants++;
    else if (call->support != P25_CALL_PHASE1) f->unresolved_grants++;
}

bool p25_grant_on_call(p25_grant_follower_t *f, const p25_call_info_t *call,
                       int64_t now_us)
{
    if (!f || !call) return false;
    p25_grant_observe(f, call);
    if (call->support != P25_CALL_PHASE1 || call->slots_per_carrier != 1 ||
        call->slot != 0) return false;
    if (f->state == P25_GRANT_ON_TRAFFIC &&
        (f->active_call.slot != call->slot ||
         (f->active_call.system_valid && call->system_valid &&
          (f->active_call.wacn != call->wacn ||
           f->active_call.sysid != call->sysid)))) {
        f->foreign_grants++;
        return false;
    }
    bool tuned = on_grant(f, call->talkgroup, call->source,
                          call->carrier_hz, now_us, call);
    return tuned;
}

void p25_grant_on_frame(p25_grant_follower_t *f, const dsd_state *s,
                        int64_t now_us)
{
    if (!f || !s) return;
    if (!s->p25_frame_valid) {
        f->receive_state = f->state == P25_GRANT_ON_TRAFFIC
            ? P25_RX_TRAFFIC_WAIT : P25_RX_CONTROL_SEARCH;
        return;
    }
    if (f->state == P25_GRANT_ON_CONTROL) {
        if (s->p25_frame_duid == 7 && s->p25_frame_tsbks) {
            if (f->receive_state != P25_RX_CONTROL_LOCKED) f->control_relocks++;
            f->receive_state = P25_RX_CONTROL_LOCKED;
        }
        return;
    }
    f->receive_state = P25_RX_TRAFFIC_SYNC;
    /* lastp25type also becomes zero for an ignored LDU2 or unknown
     * DUID. Only a validated TDU/TDULC is a terminator. */
    if (s->p25_frame_duid == 3 || s->p25_frame_duid == 15) {
        (void)p25_grant_on_terminator(f, now_us);
    } else if (s->p25_frame_duid == 5 || s->p25_frame_duid == 10) {
        if ((s->p25_frame_duid == 5 && s->lastp25type != 1) ||
            (s->p25_frame_duid == 10 && s->lastp25type != 2)) return;
        p25_grant_on_voice(f, now_us);
        f->receive_state = s->pcm_out_write > 0 ? P25_RX_AUDIO :
            s->p25_ess_valid && p25_algid_is_encrypted(s->p25_algid)
                ? P25_RX_ENCRYPTED : P25_RX_TRAFFIC_SYNC;
    }
}

bool p25_grant_on_ess(p25_grant_follower_t *f, uint16_t talkgroup,
                      uint8_t algid, uint16_t kid, int64_t now_us)
{
    if (!f) return false;
    /* An ESS with no TG context (LCW not yet parsed on a mid-stream join, or
     * the caller passing 0 because it does not know) still updates the
     * follower's currently-followed TG. Only skip the record entirely if
     * neither the caller nor the follower has a TG to key on. */
    uint16_t effective_tg = talkgroup ? talkgroup : f->talkgroup;
    if (effective_tg == 0) return false;

    p25_grant_tg_state_t *e = tg_state_get_or_make(f, effective_tg);
    e->algid = algid;
    e->kid   = kid;
    e->last_seen_us = now_us;

    if (!p25_algid_is_encrypted(algid)) {
        /* A clear ESS clears any prior skip on this TG - if the system moved
         * from ADP back to CLEAR, we should follow again immediately. */
        e->skip_expires_us = 0;
        return false;
    }
    if (!f->leave_on_encrypted) return false;
    if (f->state != P25_GRANT_ON_TRAFFIC) return false;

    if (f->talkgroup != effective_tg) return false;

    e->skip_expires_us = now_us + f->encrypted_skip_us;
    f->encrypted_returns++;
    retune_to_control(f);
    return true;
}
