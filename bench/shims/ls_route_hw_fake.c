/* Hardware answers for UI tests which do not link a receiver or motor. */
#include "ls_gps.h"
#include "ls_haptic.h"
#include <string.h>
esp_err_t ls_gps_start(void) { return ESP_OK; }
#ifndef LS_ROUTE_FAKE_START_ONLY
void ls_gps_get(ls_gps_state_t *out) { memset(out,0,sizeof(*out)); }
bool ls_haptic_play(ls_haptic_effect_t effect) { (void)effect; return false; }
#endif
