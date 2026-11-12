/* Hardware answers for UI tests which do not link a receiver or motor. */
#include "ls_gps.h"
#include "ls_haptic.h"
#include <string.h>
esp_err_t ls_gps_start(void) { return ESP_OK; }
#ifndef LS_ROUTE_FAKE_START_ONLY
void ls_gps_get(ls_gps_state_t *out) { memset(out,0,sizeof(*out)); }
bool ls_haptic_play(ls_haptic_effect_t effect) { (void)effect; return false; }
static bool s_fake_haptics = true;
void ls_haptic_set_enabled(bool on) { s_fake_haptics = on; }
bool ls_haptic_enabled(void) { return s_fake_haptics; }
void ls_haptic_stop(void) { }
#endif
