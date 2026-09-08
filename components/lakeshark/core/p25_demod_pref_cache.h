#ifndef P25_DEMOD_PREF_CACHE_H
#define P25_DEMOD_PREF_CACHE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* LS-719: p25_rx_task has a PSRAM stack, so its startup path must consume
 * this bounded RAM value rather than reading NVS while flash disables the
 * cache.  The storage representation remains 0..3 for manual modes and 4
 * for AUTO; absent and malformed values also select AUTO. */
void p25_demod_pref_cache_init(bool found, uint8_t stored);
int  p25_demod_pref_cache_get(void);
bool p25_demod_pref_cache_update(int preference, uint8_t *stored);

#ifdef __cplusplus
}
#endif

#endif
