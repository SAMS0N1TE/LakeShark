/* Simulator seam: sessions open, no fabricated on-air packets. */
#include "ls_lora_lr20xx.h"
static bool active;
esp_err_t lr20xx_exp_begin(lr20xx_engine_t engine, unsigned preset, unsigned channel, unsigned fec)
{ (void)engine; (void)preset; (void)channel; (void)fec; active=true; return ESP_OK; }
int lr20xx_exp_poll(uint8_t *buf, size_t size, lr20xx_engine_packet_t *out)
{ (void)buf; (void)size; (void)out; return active ? 0 : -1; }
esp_err_t lr20xx_exp_end(void) { active=false; return ESP_OK; }
