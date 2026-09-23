#ifndef LS_P25_P2_RUNTIME_STATUS_H
#define LS_P25_P2_RUNTIME_STATUS_H
/* separate from p25_p2_runtime.h so the TUI does not see decoder types */
#include "p25_phase2.h"
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* false until the RX task has applied configuration `generation` */
bool p25_p2_status_for(uint32_t generation, p25p2_status_t *out);
#ifdef __cplusplus
}
#endif
#endif
