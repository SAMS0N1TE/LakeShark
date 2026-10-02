/* The LoRa chip's RSSI sweep for the screen tests, which link the
   experiments but not the simulator's mesh board: a flat floor near
   -120 dBm, every row complete in one pass, with one carrier a quarter of
   the way up any band (BAND SURVEY) and one at 403.410 MHz on RADIOSONDE's
   400-406 MHz, 601-bin row. The simulator itself links lssim_mesh.c, whose
   sweep refuses, instead. */
#include "ls_lora.h"

static bool s_scan;

esp_err_t ls_lora_scan_begin(uint32_t min_hz, uint32_t max_hz)
{
    if (max_hz <= min_hz) return ESP_ERR_INVALID_ARG;
    s_scan = true;
    return ESP_OK;
}

int ls_lora_scan_pass(float *dbm, int n, bool *row_done)
{
    if (row_done) *row_done = false;
    if (!s_scan || !dbm || n <= 0) return 0;
    for (int i = 0; i < n; i++) dbm[i] = -120.0f + (float)(i % 3);
    dbm[n / 4] = -70.0f;
    if (n > 341) dbm[341] = -84.0f;
    if (row_done) *row_done = true;
    return n;
}

esp_err_t ls_lora_scan_end(void) { s_scan = false; return ESP_OK; }
