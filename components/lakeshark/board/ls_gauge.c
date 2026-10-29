/* See ls_gauge.h. Register map from the vendor driver, not memory. */
#include "ls_gauge.h"
#include "ls_gauge_soc.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ls_board.h"
#include "ls_i2c.h"
#include "ls_textpack.h"

static const char *TAG = "ls_gauge";

#if LS_HAS_GAUGE

#define REG_TEMPERATURE  0x06
#define REG_VOLTAGE      0x08
#define REG_STATUS       0x0A
#define REG_CURRENT      0x0C
#define REG_REMAINING    0x10
#define REG_FULL         0x12
#define REG_SOC          0x2C

/* A low-priority worker owns cache refresh; drawing never touches I2C. */
#define SAMPLE_MS 30000
static i2c_master_dev_handle_t s_dev;
static bool s_present;
static ls_gauge_t s_cache;
static int64_t s_cache_us;
static TaskHandle_t s_sampler;
static portMUX_TYPE s_cache_mux = portMUX_INITIALIZER_UNLOCKED;

static void gauge_sampler(void *arg)
{
    (void)arg;
    for (;;) {
        ls_gauge_t next;
        if (ls_gauge_read(&next) == ESP_OK) {
            portENTER_CRITICAL(&s_cache_mux);
            s_cache = next;
            s_cache_us = esp_timer_get_time();
            portEXIT_CRITICAL(&s_cache_mux);
        }
        /* Keep the last good sample on a transient bus error. A 30 s
           sample already suppresses rapid load flicker; do not add the old
           per-frame EMA, which would now take minutes to follow discharge. */
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));
    }
}

static bool rd16(uint8_t reg, uint16_t *out)
{
    if (!s_dev) return false;
    uint8_t v[2] = { 0, 0 };
    if (i2c_master_transmit_receive(s_dev, &reg, 1, v, sizeof(v), 100) != ESP_OK)
        return false;
    /* Little endian on the wire, per the datasheet's word commands. */
    *out = (uint16_t)(v[0] | (v[1] << 8));
    return true;
}

esp_err_t ls_gauge_start(void)
{
    if (s_present) {
        if (!s_sampler && xTaskCreate(gauge_sampler, "gauge", 3072, NULL, 1,
                                     &s_sampler) != pdPASS) return ESP_ERR_NO_MEM;
        return ESP_OK;
    }

    esp_err_t err = ls_i2c_device(LS_I2C_PRIMARY, LS_BOARD_GAUGE_I2C_ADDR,
                                  100 * 1000, &s_dev);
    if (err != ESP_OK) return err;

    err = ls_i2c_probe(LS_I2C_PRIMARY, LS_BOARD_GAUGE_I2C_ADDR, 100);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no ACK at 0x%02X", LS_BOARD_GAUGE_I2C_ADDR);
        return ESP_ERR_NOT_FOUND;
    }

    uint16_t mv = 0;
    if (!rd16(REG_VOLTAGE, &mv) || mv == 0) {
        ESP_LOGW(TAG, "answered at 0x%02X but reports no pack voltage",
                 LS_BOARD_GAUGE_I2C_ADDR);
        return ESP_ERR_INVALID_STATE;
    }

    s_present = true;
    ESP_LOGI(TAG, "BQ27220 up, %u mV", (unsigned)mv);
    return ls_gauge_start();
}

bool ls_gauge_present(void) { return s_present; }

esp_err_t ls_gauge_read(ls_gauge_t *out)
{
    if (!s_present) { esp_err_t e = ls_gauge_start(); if (e != ESP_OK) return e; }
    if (!out) return ESP_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));
    uint16_t v = 0;

    if (!rd16(REG_VOLTAGE, &v)) return ESP_FAIL;
    out->millivolts = v;

    if (rd16(REG_CURRENT, &v))   out->milliamps = (int16_t)v;
    if (rd16(REG_SOC, &v))       out->learned_percent = (uint8_t)(v > 100 ? 100 : v);
    if (rd16(REG_REMAINING, &v)) out->remaining_mah = v;
    if (rd16(REG_FULL, &v))      out->full_mah = v;
    if (rd16(REG_TEMPERATURE, &v)) {
        /* 0.1 K on the wire. 2731 is 0 C. */
        out->temp_c10 = (int16_t)((int32_t)v - 2731);
    }

    out->charging = out->milliamps > 0;
    out->present  = true;
    out->percent  = (uint8_t)ls_gauge_soc_from_rest_mv(
                        ls_gauge_rest_mv(out->millivolts, out->milliamps));
    return ESP_OK;
}

bool ls_gauge_get(ls_gauge_t *out)
{
    if (!out) return false;
    portENTER_CRITICAL(&s_cache_mux);
    *out = s_cache;
    portEXIT_CRITICAL(&s_cache_mux);
    return out->present;
}

bool ls_gauge_peek(int16_t *milliamps, int64_t *age_us)
{
    portENTER_CRITICAL(&s_cache_mux);
    const ls_gauge_t cache = s_cache;
    const int64_t at = s_cache_us;
    portEXIT_CRITICAL(&s_cache_mux);
    if (!cache.present) return false;
    if (milliamps) *milliamps = cache.milliamps;
    if (age_us) *age_us = esp_timer_get_time() - at;
    return true;
}

void ls_gauge_diagnostics(void)
{
    ls_gauge_t g;
    esp_err_t err = ls_gauge_read(&g);
    if (err == ESP_ERR_INVALID_STATE) {
        LS_PRINTF(t_ls_gauge_gauge_answers_at_0x, "gauge: answers at 0x%02X but reports no pack - is a battery "
               "connected?\n", LS_BOARD_GAUGE_I2C_ADDR);
        return;
    }
    if (err != ESP_OK) {
        LS_PRINTF(t_ls_gauge_gauge_not_answering_at, "gauge: not answering at 0x%02X (%s)\n",
               LS_BOARD_GAUGE_I2C_ADDR, esp_err_to_name(err));
        return;
    }
    LS_PRINTF(t_ls_gauge_gauge_u_mv_d, "gauge: %u mV  %d mA  %u%% from voltage (gauge learned %u%%)  "
           "%d.%d C  %u/%u mAh  %s\n",
           (unsigned)g.millivolts, (int)g.milliamps, (unsigned)g.percent,
           (unsigned)g.learned_percent,
           g.temp_c10 / 10, (g.temp_c10 < 0 ? -g.temp_c10 : g.temp_c10) % 10,
           (unsigned)g.remaining_mah, (unsigned)g.full_mah,
           g.charging ? "charging" : "discharging");
    LS_PRINTF(t_ls_gauge_gauge_the_percentage_is, "gauge: the percentage is the rest voltage (%d mV after %d mOhm) "
           "on a Li-ion curve; the gauge was never configured for this cell.\n",
           ls_gauge_rest_mv(g.millivolts, g.milliamps), LS_GAUGE_PACK_MOHM);
}

#else  /* board declares no gauge */

esp_err_t ls_gauge_start(void) { return ESP_ERR_NOT_SUPPORTED; }
bool      ls_gauge_present(void) { return false; }
esp_err_t ls_gauge_read(ls_gauge_t *o) { (void)o; return ESP_ERR_NOT_SUPPORTED; }
bool      ls_gauge_get(ls_gauge_t *o) { (void)o; return false; }
bool      ls_gauge_peek(int16_t *ma, int64_t *age) { (void)ma; (void)age; return false; }
void      ls_gauge_diagnostics(void) { printf("gauge: board declares none\n"); }

#endif
