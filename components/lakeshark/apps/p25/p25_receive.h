#ifndef P25_RECEIVE_H
#define P25_RECEIVE_H

#include "scan_ctrl.h"

/* LS-739: used by the real decode loop and deterministic backend replay.
 * Reset at every call/tune boundary, including same-TG rejoin without HDU.
 * IDEN/system state deliberately survives a traffic excursion. */
static inline void p25_receive_call_reset(dsd_state *s)
{
    p25_ess_clear(s);
    p25_lcw_call_clear(s);
    s->lasttg = 0;
    s->lastsrc = 0;
    s->lastp25type = 0;
    s->pcm_out_write = 0;
    s->p25_frame_valid = 0;
    s->p25_frame_tsbks = 0;
    s->p25_grant_count = 0;
    s->p25_grant_batch_valid = 1;
    s->p25_grant_generation++;
}

/* A decoded frame is not necessarily audio: unknown/encrypted ESS can leave
 * PCM empty. Feed frame activity independently of PCM so that a held muted
 * call does not time out, and require a real TDU/TDULC before returning. */
static inline bool p25_receive_frame(p25_scan_ctrl_t *sc,
                                    p25_grant_follower_t *f, dsd_state *s,
                                    int64_t now_us, bool allow_follow)
{
    unsigned int transitions = f->followed_count + f->returned_count;
    uint32_t source = f->source;
    if (f->state == P25_GRANT_ON_TRAFFIC && s->p25_frame_valid &&
        s->p25_frame_duid != 7 && f->active_call.nac &&
        f->active_call.nac != s->nac) {
        /* A valid frame from another NAC is not activity on our call. */
        s->pcm_out_write = 0;
        s->p25_frame_valid = 0;
    }
    if (f->state == P25_GRANT_ON_TRAFFIC && s->p25_frame_valid && s->p25_lcw_valid &&
        !s->p25_lcw_is_unit_to_unit && s->p25_lcw_talkgroup &&
        s->p25_lcw_talkgroup != f->talkgroup) {
        /* LS-739: validated LCW contradicts the grant. Do not attribute its
         * PCM/ESS to the requested TG or poison that TG's encrypted skip. */
        (void)p25_grant_force_return_to_control(f);
        p25_receive_call_reset(s);
        return true;
    }
    p25_grant_on_frame(f, s, now_us);
    if (s->p25_frame_valid && s->p25_frame_duid == 10 && s->p25_ess_valid &&
        f->state == P25_GRANT_ON_TRAFFIC)
        (void)p25_grant_on_ess(f, 0, s->p25_algid, s->p25_kid, now_us);
    if (s->p25_frame_valid && s->p25_frame_duid == 7 && allow_follow)
        (void)p25_scan_apply_from_state(sc, f, s, now_us);
    if (transitions != f->followed_count + f->returned_count ||
        (source && f->source && source != f->source)) {
        p25_receive_call_reset(s);
        return true;
    }
    return false;
}

#endif
