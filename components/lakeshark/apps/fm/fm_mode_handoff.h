#ifndef FM_MODE_HANDOFF_H
#define FM_MODE_HANDOFF_H

#include <stdbool.h>

#include "fm_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A console/control request can arrive before the GUI has asked the backend
   to enter FM.  Keep exactly one latest-wins request until the entry path or
   the live RX task consumes it. */
typedef struct {
    volatile int pending_mode;
} fm_mode_handoff_t;

#define FM_MODE_HANDOFF_INITIALIZER { -1 }

void fm_mode_handoff_request(fm_mode_handoff_t *handoff, fm_mode_t mode);
bool fm_mode_handoff_take(fm_mode_handoff_t *handoff, fm_mode_t *mode);

/* A pending explicit request wins entry.  Without one, retain the last mode
   used by normal manual FM entry; use fallback only if that state is invalid. */
fm_mode_t fm_mode_handoff_resolve_entry(fm_mode_handoff_t *handoff,
                                        int retained_mode,
                                        fm_mode_t fallback_mode);

#ifdef __cplusplus
}
#endif

#endif
