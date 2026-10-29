/* P25's live face inside CALLS: the call on air now (or the last one), the
   busiest talkgroups of the last sixteen minutes, and hold / lockout on the
   talkgroup shown. Everything it draws is read from the receiver's state
   and the observed-talkgroup store; it stores nothing and allocates
   nothing. All functions run on the UI task. */
#ifndef P25_CALLS_LIVE_H
#define P25_CALLS_LIVE_H
#include <stdbool.h>
#include <stdint.h>

/* Hand the live face to CALLS for the P25 receiver. */
void p25_calls_live_register(void);
/* Once a P25 frame, whichever page is up: follows the call (start, end,
   talkgroup, unit) and samples its voice level for the trace, so the card
   is right the moment CALLS opens. Cheap: a few loads and compares. */
void p25_calls_live_tick(void);
/* A talkgroup's name from the names file or the loaded profile, or NULL. */
const char *p25_calls_alias(uint16_t talkgroup);

/* For tests: the call the card is showing and whether it is on air. */
bool p25_calls_live_now(uint16_t *talkgroup, uint32_t *unit, bool *on_air,
                        uint32_t *elapsed_ms);
#endif
