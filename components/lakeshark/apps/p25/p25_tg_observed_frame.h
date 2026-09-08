#ifndef P25_TG_OBSERVED_FRAME_H
#define P25_TG_OBSERVED_FRAME_H
#include "dsd.h"
#include "p25_tg_observed.h"
/* Call only after the acquisition epoch accepts this completed frame, using
 * the effective frequency captured BEFORE decoding. A later tune must never
 * relabel observations from the preceding channel. Grants are captured before
 * the follower consumes their batch; no follower policy is modified here. */
static inline void p25_tg_observed_frame(const dsd_state *s, uint32_t prior_lcw,
                                        uint64_t hz, uint32_t now_ms)
{
    if (!s || !s->p25_frame_valid || s->nac < 0 || s->nac > 0xfff) return;
    if (s->p25_lcw_ok_count != prior_lcw && s->p25_lcw_valid &&
        !s->p25_lcw_is_unit_to_unit)
        p25_tg_observed_record(hz, (uint16_t)s->nac, s->p25_lcw_talkgroup,
                               P25_TG_SEEN_LCW, now_ms);
    /* The validated HDU publisher clears ESS at entry and publishes it only
     * after RS success. Never use lasttg for another DUID or stale ESS. */
    if (s->p25_frame_duid == 0 && s->p25_ess_valid)
        p25_tg_observed_record(hz, (uint16_t)s->nac, (uint16_t)s->lasttg,
                               P25_TG_SEEN_HDU, now_ms);
    if (s->p25_frame_duid == 7 && s->p25_grant_batch_valid)
        for (unsigned i = 0; i < s->p25_grant_count && i < P25_GRANTS_PER_TSDU; ++i)
            p25_tg_observed_record(hz, (uint16_t)s->nac,
                s->p25_grants[i].talkgroup, P25_TG_SEEN_GRANT, now_ms);
}
#endif
