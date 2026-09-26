/* What ls_compass_live reaches for at run time, for the pure tests. */
#include <string.h>
#include "ls_field.h"
#include "ls_gps.h"
#include "core/ls_time.h"
void ls_field_sample_snapshot(ls_field_sample_t *out) { memset(out, 0, sizeof(*out)); }
bool ls_field_compass_cal(ls_compass_cal_t *out) { (void)out; return false; }
void ls_gps_get(ls_gps_state_t *g) { memset(g, 0, sizeof(*g)); }
#include "core/settings.h"
bool settings_get_last_fix(float *lat, float *lon) { (void)lat; (void)lon; return false; }
bool settings_set_last_fix(float lat, float lon) { (void)lat; (void)lon; return false; }
bool settings_get_home(float *lat, float *lon) { (void)lat; (void)lon; return false; }
