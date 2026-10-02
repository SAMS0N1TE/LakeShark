/* What the other built-in experiments link against, for a test that
   registers every built-in but drives only one: none of these is reached. */
#include <string.h>

#include "ls_gps.h"
#include "ls_lora.h"

esp_err_t ls_lora_fsk_retune(uint32_t hz) { (void)hz; return ESP_ERR_NOT_SUPPORTED; }
const ls_lora_cfg_t *ls_lora_cfg(void) { return NULL; }
void ls_lora_cfg_default(ls_lora_cfg_t *out) { memset(out, 0, sizeof(*out)); out->freq_hz = 910525000u; }
esp_err_t ls_lora_configure(const ls_lora_cfg_t *cfg) { (void)cfg; return ESP_OK; }
bool settings_get_home(float *lat, float *lon) { (void)lat; (void)lon; return false; }
void ls_gps_get(ls_gps_state_t *out) { memset(out, 0, sizeof(*out)); }
