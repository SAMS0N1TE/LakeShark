#include "p25_split.h"
#include "p25_tsbk.h"
#include "p25p1_check_nid.h"
#include <string.h>

void p25_split_init(p25_split_t *s, uint64_t hz, int64_t now)
{
    memset(s, 0, sizeof(*s));
    s->control_hz = hz;
    s->window_us = now;
    s->solid_since_us = -1;
}

bool p25_split_service(p25_split_t *s, p25_grant_follower_t *f,
                       bool enabled, bool available, int64_t now)
{
    s->enabled = enabled;
    s->available = available;
    if (!enabled || !available) {
        s->solid_since_us = -1;
        s->good = s->bad = 0;
        s->window_us = now;
        s->crc_percent = 0;
    }
    if (now - s->window_us >= 1000000LL) {
        unsigned total = s->good + s->bad;
        s->crc_percent = total ? 100u * s->good / total : 0;
        if (!available || s->good < 4 || s->crc_percent < 70)
            s->solid_since_us = -1;
        else {
            s->last_quality_us = now;
            if (s->solid_since_us < 0) s->solid_since_us = s->window_us;
        }
        s->good = s->bad = 0;
        s->window_us = now;
    }
    bool active = s->active;
    if (!enabled || !available || now - s->last_quality_us >= P25_SPLIT_LOSS_US)
        s->active = false;
    else if (s->solid_since_us >= 0 &&
             now - s->solid_since_us >= P25_SPLIT_SOLID_US)
        s->active = true;
    if (active == s->active) return false;
    f->consumed_grant_valid = false;
    if (s->active) {
        p25_grant_set_phase2_follow(f, true);
        s->recoveries++;
    } else {
        s->fallbacks++;
        if (!p25_grant_force_return_to_control(f) && f->retune)
            f->retune(f->user, f->control_hz, false);
    }
    return true;
}

bool p25_split_tsdu(p25_split_t *s, p25_scan_ctrl_t *sc,
                    p25_grant_follower_t *f, uint16_t nac,
                    const uint8_t *wire, size_t n, int64_t now)
{
    dsd_state *cc = &s->cc;
    if (cc->p25_control_nac_valid && cc->p25_control_nac != nac)
        p25_tsbk_reset_system(cc);
    cc->nac = nac;
    cc->p25_control_nac = nac;
    cc->p25_control_nac_valid = 1;
    unsigned good = cc->p25_tsbk_valid_count;
    unsigned bad = cc->p25_tsbk_crc_errors + cc->p25_tsbk_trellis_errors;
    (void)p25_tsbk_process_tsdu(cc, wire, n);
    s->good += cc->p25_tsbk_valid_count - good;
    s->bad += cc->p25_tsbk_crc_errors + cc->p25_tsbk_trellis_errors - bad;
    if (cc->p25_tsbk_valid_count != good) s->last_good_us = now;
    if (!s->active) return false;
    for (unsigned i = 0; i < cc->p25_grant_count; ++i) {
        const p25_call_info_t *g = &cc->p25_grants[i];
        p25_scan_decision_t d = p25_scan_decide(sc, f, g->talkgroup,
                                               g->source, g->carrier_hz, now);
        if (d.reason != P25_SCAN_REASON_ALREADY_FOLLOWING) s->reason = d.reason;
        if (d.action == P25_SCAN_STAY &&
            d.reason != P25_SCAN_REASON_ALREADY_FOLLOWING) s->skipped++;
    }
    return p25_scan_apply_from_state(sc, f, cc, now);
}

bool p25_split_feed(p25_split_t *s, p25_scan_ctrl_t *sc,
                    p25_grant_follower_t *f, uint8_t dibit, int64_t now)
{
    if (dibit > 3) { s->collecting = false; s->sync = 0; return false; }
    if (!s->collecting) {
        s->sync = ((s->sync << 2) | dibit) & UINT64_C(0xffffffffffff);
        if (s->sync == UINT64_C(0x5575f5ff77ff) ||
            s->sync == UINT64_C(0xffdf5f55dd55)) {
            s->inverted = s->sync != UINT64_C(0x5575f5ff77ff);
            s->collecting = true;
            s->frame_n = 0;
        }
        return false;
    }
    s->frame[s->frame_n++] = dibit ^ (s->inverted ? 2 : 0);
    if (s->frame_n < sizeof(s->frame)) return false;
    s->collecting = false;
    s->sync = 0;
    char bits[63], duid[3];
    unsigned b = 0;
    for (unsigned i = 0; i < 33; ++i) {
        if (i == 11) continue; /* status symbol in NID */
        bits[b++] = s->frame[i] >> 1;
        if (b < 63) bits[b++] = s->frame[i] & 1;
    }
    int nac;
    if (!check_NID(bits, &nac, duid, s->frame[32] & 1) ||
        duid[0] != '1' || duid[1] != '3') return false;
    return p25_split_tsdu(s, sc, f, (uint16_t)nac, s->frame + 33,
                          P25_TSDU_DIBIT_COUNT, now);
}

const char *p25_split_reason(p25_scan_reason_t r)
{
    switch (r) {
    case P25_SCAN_REASON_FOREIGN_GRANT: return "busy: lower/equal priority";
    case P25_SCAN_REASON_HOLD_MISMATCH: return "hold: other talkgroup";
    case P25_SCAN_REASON_LOCKED_OUT: return "talkgroup locked out";
    case P25_SCAN_REASON_AUTO_FOLLOW_OFF: return "auto follow off";
    case P25_SCAN_REASON_ALLOW_LIST: return "outside allow list";
    case P25_SCAN_REASON_LIST_NONE: return "list follows none";
    case P25_SCAN_REASON_ENCRYPTED_SKIP: return "encrypted skip";
    case P25_SCAN_REASON_PRIORITY: return "priority grant";
    default: return "grant policy";
    }
}
