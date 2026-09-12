/* See ls_xl9535.h, especially the note on pin numbering. */
#include "ls_xl9535.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "xl9535";

/* TCA9535 register map.  Config bit 1 = input, 0 = output; every pin comes
   out of reset as an input, which is why a rail stays dead until something
   both clears the config bit and writes the output register. */
#define REG_INPUT0   0x00
#define REG_INPUT1   0x01
#define REG_OUTPUT0  0x02
#define REG_OUTPUT1  0x03
#define REG_CONFIG0  0x06
#define REG_CONFIG1  0x07

#define I2C_HZ       400000
#define I2C_TIMEOUT  100

static i2c_master_dev_handle_t s_dev;
static bool s_ready;

/* Shadow copies.  The part has no read-modify-write and no per-bit access,
   so touching one pin means rewriting a whole bank; keeping the shadow means
   that rewrite does not disturb the other seven. */
static uint8_t s_out[2];
static uint8_t s_cfg[2];

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    const uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), I2C_TIMEOUT);
}

static esp_err_t reg_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, I2C_TIMEOUT);
}

bool ls_xl9535_ready(void) { return s_ready; }

esp_err_t ls_xl9535_init(ls_i2c_bus_id_t bus, uint8_t addr)
{
    if (s_ready) return ESP_OK;

    /* Retry the probe. */

    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 3; attempt++) {
        err = ls_i2c_probe(bus, addr, I2C_TIMEOUT);
        if (err == ESP_OK) {
            if (attempt) ESP_LOGW(TAG, "0x%02x answered on attempt %d",
                                  addr, attempt + 1);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no ACK at 0x%02x on bus %d: %s", addr, (int)bus,
                 esp_err_to_name(err));
        return ESP_ERR_NOT_FOUND;
    }

    err = ls_i2c_device(bus, addr, I2C_HZ, &s_dev);
    if (err != ESP_OK) return err;

    if (reg_read(REG_OUTPUT0, &s_out[0]) != ESP_OK ||
        reg_read(REG_OUTPUT1, &s_out[1]) != ESP_OK ||
        reg_read(REG_CONFIG0, &s_cfg[0]) != ESP_OK ||
        reg_read(REG_CONFIG1, &s_cfg[1]) != ESP_OK) {
        ESP_LOGE(TAG, "found at 0x%02x but register read failed", addr);
        return ESP_FAIL;
    }

    s_ready = true;
    ESP_LOGI(TAG, "up at 0x%02x: out=%02x%02x cfg=%02x%02x", addr,
             s_out[1], s_out[0], s_cfg[1], s_cfg[0]);
    return ESP_OK;
}

esp_err_t ls_xl9535_set_dir(int pin, bool output)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (pin < 0 || pin >= LS_XL9535_PIN_COUNT) return ESP_ERR_INVALID_ARG;

    const int bank = pin >> 3;
    const uint8_t mask = (uint8_t)(1u << (pin & 7));
    const uint8_t next = output ? (uint8_t)(s_cfg[bank] & ~mask)
                                : (uint8_t)(s_cfg[bank] | mask);
    if (next == s_cfg[bank]) return ESP_OK;

    const esp_err_t err = reg_write(bank ? REG_CONFIG1 : REG_CONFIG0, next);
    if (err == ESP_OK) s_cfg[bank] = next;
    return err;
}

esp_err_t ls_xl9535_set(int pin, bool level)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (pin < 0 || pin >= LS_XL9535_PIN_COUNT) return ESP_ERR_INVALID_ARG;

    const int bank = pin >> 3;
    const uint8_t mask = (uint8_t)(1u << (pin & 7));
    const uint8_t next = level ? (uint8_t)(s_out[bank] | mask)
                               : (uint8_t)(s_out[bank] & ~mask);
    if (next == s_out[bank]) return ESP_OK;

    const esp_err_t err = reg_write(bank ? REG_OUTPUT1 : REG_OUTPUT0, next);
    if (err == ESP_OK) s_out[bank] = next;
    return err;
}

esp_err_t ls_xl9535_get(int pin, bool *level)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (pin < 0 || pin >= LS_XL9535_PIN_COUNT || !level)
        return ESP_ERR_INVALID_ARG;

    uint8_t val = 0;
    const int bank = pin >> 3;
    const esp_err_t err = reg_read(bank ? REG_INPUT1 : REG_INPUT0, &val);
    if (err != ESP_OK) return err;
    *level = (val >> (pin & 7)) & 1u;
    return ESP_OK;
}

esp_err_t ls_xl9535_out(int pin, bool level)
{
    /* Drive the level before enabling the output, so a rail never glitches
       through the opposite state on its way up. */
    esp_err_t err = ls_xl9535_set(pin, level);
    if (err != ESP_OK) return err;
    return ls_xl9535_set_dir(pin, true);
}
