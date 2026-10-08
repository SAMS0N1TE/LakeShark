/* Simulator seam: sessions open, no fabricated on-air packets. */
#include "ls_lora_lr20xx.h"
#include "p25_state.h"
#include <string.h>

/* Simulator has no chip counters or running P25 front-end configuration. */
void p25_lr_get_cfg(p25_lr_cfg_t *out) { memset(out, 0, sizeof(*out)); }
esp_err_t lr20xx_get_gfsk_rx_stats(lr20xx_gfsk_rx_stats_t *out)
{ (void)out; return ESP_ERR_NOT_SUPPORTED; }
static bool active;
esp_err_t lr20xx_exp_begin(lr20xx_engine_t engine, unsigned preset, unsigned channel, unsigned fec)
{ (void)engine; (void)preset; (void)channel; (void)fec; active=true; return ESP_OK; }
int lr20xx_exp_poll(uint8_t *buf, size_t size, lr20xx_engine_packet_t *out)
{ (void)buf; (void)size; (void)out; return active ? 0 : -1; }
esp_err_t lr20xx_exp_end(void) { active=false; return ESP_OK; }
