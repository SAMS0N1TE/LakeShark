#ifndef LS_P25_P2_RUNTIME_STATUS_H
#define LS_P25_P2_RUNTIME_STATUS_H
/* Kept apart from p25_p2_runtime.h, which the TUI includes and which must
   not drag the decoder's types into it. */
#include "p25_phase2.h"
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* The decoder's status, but only once the receive task has applied
   configuration `generation` - so a follower never reads the previous
   call's counts as its own. */
bool p25_p2_status_for(uint32_t generation, p25p2_status_t *out);
#ifdef __cplusplus
}
#endif
#endif
