#ifndef P25_P2_FOLLOW_H
#define P25_P2_FOLLOW_H

/* When a followed Phase II call ends, from the slot's MAC PDUs. Pure policy,
 * clock passed in. A two-slot carrier stays up while the other slot talks,
 * so carrier and sync alone do not mean the call is still going. */

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
    /* last seen decoder counts; a drop means a fresh decoder */
    uint32_t seen_bursts, seen_voice, seen_control;
    p25_p2f_verdict_t last_verdict;
    uint32_t calls, leaves[P25_P2F_VERDICTS];
} p25_p2_follow_t;

void p25_p2_follow_defaults(p25_p2f_config_t *cfg);
void p25_p2_follow_init(p25_p2_follow_t *f);

void p25_p2_follow_start(p25_p2_follow_t *f, uint16_t talkgroup,
                         uint32_t now_ms);
/* status is NULL until the decoder is configured for this call */
p25_p2f_verdict_t p25_p2_follow_tick(p25_p2_follow_t *f,
                                     const p25p2_status_t *status,
                                     uint32_t now_ms);

void p25_p2_follow_stop(p25_p2_follow_t *f);
bool p25_p2_follow_active(const p25_p2_follow_t *f);
const char *p25_p2_follow_verdict_name(p25_p2f_verdict_t v);
const char *p25_p2_follow_phase_name(p25_p2f_phase_t p);

#endif
