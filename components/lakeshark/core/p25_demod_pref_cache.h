#ifndef P25_DEMOD_PREF_CACHE_H
#define P25_DEMOD_PREF_CACHE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void p25_demod_pref_cache_init(bool found, uint8_t stored);
int  p25_demod_pref_cache_get(void);
bool p25_demod_pref_cache_update(int preference, uint8_t *stored);

#ifdef __cplusplus
}
#endif

#endif
