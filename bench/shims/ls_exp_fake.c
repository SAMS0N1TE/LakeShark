/* The board under ls_experiments for the simulator and the screen tests:
   a socket that is always free, no worker task (whoever waits, or the
   simulator's frame loop, services the request), and a LoRa chip whose FSK
   and OOK sessions open and whose RSSI is a made-up carrier near -100 dBm, so
   the CARRIER LEVEL page has numbers to lay out. */
#include "ls_experiments.h"
#include "ls_lora.h"

#include <string.h>

const char *ls_exp_hw_radio_busy(void) { return NULL; }
bool ls_exp_hw_radio_take(void) { return true; }
void ls_exp_hw_radio_give(void) { }
bool ls_exp_hw_wake(void) { return false; }

static bool s_fsk;
/* A LoRa configuration, which a sweep wants to hand the part back to. */
static ls_lora_cfg_t s_lora_cfg;
static bool s_lora_cfg_set;
const ls_lora_cfg_t *ls_lora_cfg(void) { return s_lora_cfg_set ? &s_lora_cfg : NULL; }
void ls_lora_cfg_default(ls_lora_cfg_t *out) { memset(out, 0, sizeof(*out)); out->freq_hz = 910525000u; }
esp_err_t ls_lora_configure(const ls_lora_cfg_t *cfg) { s_lora_cfg = *cfg; s_lora_cfg_set = true; return ESP_OK; }
static unsigned s_reads;

uint32_t ls_lora_caps(void)
{
    return LS_LORA_CAP_LORA | LS_LORA_CAP_FSK | LS_LORA_CAP_RSSI_INST | LS_LORA_CAP_MODES_RX;
}
uint32_t ls_lora_fsk_bw_snap(uint32_t hz) { return hz; }
esp_err_t ls_lora_fsk_begin(const ls_fsk_cfg_t *cfg) { (void)cfg; s_fsk = true; return ESP_OK; }
esp_err_t ls_lora_fsk_end(void) { s_fsk = false; return ESP_OK; }
esp_err_t ls_lora_fsk_receive(void) { return s_fsk ? ESP_OK : ESP_ERR_INVALID_STATE; }
esp_err_t ls_lora_fsk_retune(uint32_t hz) { (void)hz; return s_fsk ? ESP_OK : ESP_ERR_INVALID_STATE; }
int ls_lora_fsk_poll(uint8_t *buf, size_t size, float *rssi_dbm)
{
    (void)buf; (void)size; (void)rssi_dbm;
    return 0;
}

/* LR433's OOK sessions open and hear nothing. */
static bool s_ook;
esp_err_t ls_lora_ook_begin(const ls_ook_cfg_t *cfg) { (void)cfg; s_ook = true; return ESP_OK; }
int ls_lora_ook_poll(uint8_t *buf, size_t size, float *rssi_dbm)
{
    (void)buf; (void)size; (void)rssi_dbm;
    return s_ook ? 0 : -1;
}
esp_err_t ls_lora_ook_end(void) { s_ook = false; return ESP_OK; }
bool ls_lora_ook_active(void) { return s_ook; }

esp_err_t ls_lora_rssi_inst(float *dbm)
{
    if (!s_fsk) return ESP_ERR_INVALID_STATE;
    static const float WOBBLE[8] = { 0.0f, 1.5f, -2.0f, 3.5f, -1.0f, 0.5f, -3.0f, 2.0f };
    *dbm = -101.0f + WOBBLE[s_reads++ % 8];
    return ESP_OK;
}
