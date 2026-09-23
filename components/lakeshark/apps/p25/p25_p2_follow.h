#ifndef P25_P2_FOLLOW_H
#define P25_P2_FOLLOW_H

/* How long a followed Phase II call lasts, read from the traffic slot.
 *
 * Pure policy with the clock passed in, like p25_voice_hold: the app feeds
 * it the decoder's status and gets back "stay" or the reason to go back to
 * the control channel. It owns no radio and no decoder.
 *
 * A call's end is read from the slot's MAC PDUs, not from voice or sync. A
 * two-slot carrier stays up while the other slot talks, so a carrier is no
 * evidence that this slot's call is still going. On the public recording
 * (docs/P25_PHASE2.md) every transmission carries MAC_ACTIVE every 360 ms and
 * closes with two END_PTT; the idle slot sends nothing but MAC_IDLE. */

#include "p25_phase2.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    P25_P2F_OFF = 0,
    P25_P2F_ACQUIRE,   /* tuned; waiting for the slot to sync and speak */
    P25_P2F_CALL,      /* PTT, ACTIVE or voice */
    P25_P2F_ENDING,    /* END_PTT: HANGTIME or a new PTT may still follow */
    P25_P2F_HANG,      /* HANGTIME: the channel is held for the talkgroup */
} p25_p2f_phase_t;

typedef enum {
    P25_P2F_STAY = 0,
    P25_P2F_LEAVE_ENDED,     /* END_PTT and nothing after it */
    P25_P2F_LEAVE_IDLE,      /* the slot says it is idle */
    P25_P2F_LEAVE_HANG,      /* hangtime ran out with no one keying */
    P25_P2F_LEAVE_NO_SYNC,   /* the slot never synchronised */
    P25_P2F_LEAVE_LOST,      /* sync lost and not regained */
    P25_P2F_LEAVE_QUIET,     /* synchronised, nothing said */
    P25_P2F_LEAVE_ENCRYPTED, /* a known algorithm that is not clear */
    P25_P2F_LEAVE_OTHER_TG,  /* the slot's PTT names another talkgroup */
    P25_P2F_VERDICTS
} p25_p2f_verdict_t;

typedef struct {
    uint32_t acquire_ms; /* grant to first sync, retune included */
    uint32_t lost_ms;    /* sync gone this long ends the call */
    uint32_t quiet_ms;   /* synced with no PTT, ACTIVE or voice */
    uint32_t end_ms;     /* after END_PTT, wait this long for HANGTIME/PTT */
    uint32_t hang_ms;    /* HANGTIME holds the channel at most this long */
    bool leave_encrypted; /* the follower's leave_on_encrypted */
} p25_p2f_config_t;

typedef struct {
    p25_p2f_config_t cfg;
    p25_p2f_phase_t phase;
    uint16_t talkgroup;
    bool ever_synced;
    uint32_t start_ms, deadline_ms, last_sync_ms, last_activity_ms;
    /* Decoder counters as last seen. The decoder restarts its counts when
       it is reconfigured, so a count that goes down is a fresh decoder. */
    uint32_t seen_bursts, seen_voice, seen_control;
    p25_p2f_verdict_t last_verdict;
    uint32_t calls, leaves[P25_P2F_VERDICTS];
} p25_p2_follow_t;

void p25_p2_follow_defaults(p25_p2f_config_t *cfg);
void p25_p2_follow_init(p25_p2_follow_t *f);
/* A grant was followed: the radio is being retuned to its carrier. */
void p25_p2_follow_start(p25_p2_follow_t *f, uint16_t talkgroup,
                         uint32_t now_ms);
/* status is NULL until the decoder has been configured for this call. */
p25_p2f_verdict_t p25_p2_follow_tick(p25_p2_follow_t *f,
                                     const p25p2_status_t *status,
                                     uint32_t now_ms);
/* The call ended some other way (retune, scan, operator). */
void p25_p2_follow_stop(p25_p2_follow_t *f);
bool p25_p2_follow_active(const p25_p2_follow_t *f);
const char *p25_p2_follow_verdict_name(p25_p2f_verdict_t v);
const char *p25_p2_follow_phase_name(p25_p2f_phase_t p);

#endif
