#include "p25_p2_follow.h"

#include <string.h>

/* Tuned on the public recording; hang matches Phase I's 2 s. */
void p25_p2_follow_defaults(p25_p2f_config_t *cfg)
{
    cfg->acquire_ms = 1500;
    cfg->lost_ms = 1000;
    cfg->quiet_ms = 2000;
    cfg->end_ms = 500;
    cfg->hang_ms = 2000;
    cfg->leave_encrypted = true;
}

void p25_p2_follow_init(p25_p2_follow_t *f)
{
    memset(f, 0, sizeof(*f));
    p25_p2_follow_defaults(&f->cfg);
}

void p25_p2_follow_start(p25_p2_follow_t *f, uint16_t talkgroup,
                         uint32_t now_ms)
{
    f->phase = P25_P2F_ACQUIRE;
    f->talkgroup = talkgroup;
    f->ever_synced = false;
    f->start_ms = f->last_activity_ms = f->last_sync_ms = now_ms;
    f->deadline_ms = 0;
    f->seen_bursts = f->seen_voice = f->seen_control = 0;
    f->last_verdict = P25_P2F_STAY;
    f->calls++;
}

void p25_p2_follow_stop(p25_p2_follow_t *f)
{
    f->phase = P25_P2F_OFF;
}

bool p25_p2_follow_active(const p25_p2_follow_t *f)
{
    return f->phase != P25_P2F_OFF;
}

static uint32_t grew(uint32_t now, uint32_t *seen)
{
    uint32_t d = now >= *seen ? now - *seen : now;
    *seen = now;
    return d;
}

static p25_p2f_verdict_t leave(p25_p2_follow_t *f, p25_p2f_verdict_t v)
{
    f->phase = P25_P2F_OFF;
    f->last_verdict = v;
    f->leaves[v]++;
    return v;
}

static bool reached(uint32_t now, uint32_t since, uint32_t span)
{
    return (uint32_t)(now - since) >= span;
}

p25_p2f_verdict_t p25_p2_follow_tick(p25_p2_follow_t *f,
                                     const p25p2_status_t *s,
                                     uint32_t now_ms)
{
    if (f->phase == P25_P2F_OFF) return P25_P2F_STAY;

    if (s) {
        uint32_t bursts = grew(s->bursts, &f->seen_bursts);
        uint32_t voice = grew(s->voice_frames, &f->seen_voice);
        uint32_t control = grew(s->control_ok, &f->seen_control);
        if (bursts && s->synchronized) {
            f->ever_synced = true;
            f->last_sync_ms = now_ms;
        }
        if (f->cfg.leave_encrypted && s->algorithm != 0xff &&
            s->algorithm != 0x80)
            return leave(f, P25_P2F_LEAVE_ENCRYPTED);
        if (f->talkgroup && s->talkgroup && s->talkgroup != f->talkgroup)
            return leave(f, P25_P2F_LEAVE_OTHER_TG);
        /* latest PDU wins; voice after it outranks it */
        if (control) {
            switch (s->last_mac_opcode) {
            case P25P2_MAC_PTT:
            case P25P2_MAC_ACTIVE:
                f->phase = P25_P2F_CALL;
                f->last_activity_ms = now_ms;
                break;
            case P25P2_MAC_END_PTT:
                if (f->phase != P25_P2F_ENDING && f->phase != P25_P2F_HANG) {
                    f->phase = P25_P2F_ENDING;
                    f->deadline_ms = now_ms + f->cfg.end_ms;
                }
                break;
            case P25P2_MAC_HANGTIME:
                /* deadline set once; repeated HANGTIME does not extend it */
                if (f->phase != P25_P2F_HANG) {
                    f->phase = P25_P2F_HANG;
                    f->deadline_ms = now_ms + f->cfg.hang_ms;
                }
                break;
            case P25P2_MAC_IDLE:
                if (!voice || s->last_voice_symbol < s->last_mac_symbol)
                    return leave(f, P25_P2F_LEAVE_IDLE);
                break;
            default:
                break;
            }
        }
        /* voice is typed before descrambling, so it counts only after a
           PDU has passed CRC (wrong system ids otherwise hold forever) */
        if (voice && s->control_ok &&
            (!control || s->last_voice_symbol > s->last_mac_symbol)) {
            f->phase = P25_P2F_CALL;
            f->last_activity_ms = now_ms;
        }
    }

    if (!f->ever_synced) {
        if (reached(now_ms, f->start_ms, f->cfg.acquire_ms))
            return leave(f, P25_P2F_LEAVE_NO_SYNC);
        return P25_P2F_STAY;
    }
    if (reached(now_ms, f->last_sync_ms, f->cfg.lost_ms))
        return leave(f, P25_P2F_LEAVE_LOST);
    switch (f->phase) {
    case P25_P2F_ENDING:
        if ((int32_t)(now_ms - f->deadline_ms) >= 0)
            return leave(f, P25_P2F_LEAVE_ENDED);
        break;
    case P25_P2F_HANG:
        if ((int32_t)(now_ms - f->deadline_ms) >= 0)
            return leave(f, P25_P2F_LEAVE_HANG);
        break;
    case P25_P2F_ACQUIRE:
        /* synced but silent since the grant */
        if (reached(now_ms, f->last_activity_ms,
                    f->cfg.acquire_ms + f->cfg.quiet_ms))
            return leave(f, P25_P2F_LEAVE_QUIET);
        break;
    case P25_P2F_CALL:
        if (reached(now_ms, f->last_activity_ms, f->cfg.quiet_ms))
            return leave(f, P25_P2F_LEAVE_QUIET);
        break;
    default:
        break;
    }
    return P25_P2F_STAY;
}

const char *p25_p2_follow_verdict_name(p25_p2f_verdict_t v)
{
    static const char *const names[P25_P2F_VERDICTS] = {
        "stay", "ended", "idle", "hang", "no-sync", "lost", "quiet",
        "encrypted", "other-tg",
    };
    return (unsigned)v < P25_P2F_VERDICTS ? names[v] : "?";
}

const char *p25_p2_follow_phase_name(p25_p2f_phase_t p)
{
    switch (p) {
    case P25_P2F_ACQUIRE: return "ACQUIRE";
    case P25_P2F_CALL:    return "CALL";
    case P25_P2F_ENDING:  return "ENDING";
    case P25_P2F_HANG:    return "HANG";
    default:              return "OFF";
    }
}
