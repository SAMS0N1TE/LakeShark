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

static ls_i2c_bus_id_t s_bus;

/* LS-1239: I2C0 can also lock after the part has answered (seen on the
   first write after the GT9895 left reset), and nothing cleared it, so
   every later transaction failed. On failure: log SDA/SCL, clear, back
   off, retry. Only runs once the bus has already failed. */
static const uint16_t RETRY_MS[] = { 10, 50, 150 };
#define RETRIES ((int)(sizeof(RETRY_MS) / sizeof(RETRY_MS[0])))

static bool recover(const char *op, uint8_t reg, esp_err_t err, int attempt)
{
    int sda = -1, scl = -1;
    ls_i2c_line_levels(s_bus, &sda, &scl);
    const esp_err_t clear = ls_i2c_bus_clear(s_bus);
    if (attempt >= RETRIES) {
        ESP_LOGE(TAG, "%s reg 0x%02x failed %d times: %s, SDA %d SCL %d, "
                 "bus clear %s", op, reg, attempt + 1, esp_err_to_name(err),
                 sda, scl, esp_err_to_name(clear));
        return false;
    }
    ESP_LOGW(TAG, "%s reg 0x%02x: %s, SDA %d SCL %d, bus clear %s - retry "
             "in %u ms", op, reg, esp_err_to_name(err), sda, scl,
             esp_err_to_name(clear), (unsigned)RETRY_MS[attempt]);
    vTaskDelay(pdMS_TO_TICKS(RETRY_MS[attempt]));
    return true;
}

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    const uint8_t buf[2] = { reg, val };
    for (int attempt = 0;; attempt++) {
        const esp_err_t err = i2c_master_transmit(s_dev, buf, sizeof(buf),
                                                  I2C_TIMEOUT);
        if (err == ESP_OK) {
            if (attempt) ESP_LOGW(TAG, "write reg 0x%02x recovered on "
                                  "attempt %d", reg, attempt + 1);
            return ESP_OK;
        }
        if (!recover("write", reg, err, attempt)) return err;
    }
}

static esp_err_t reg_read(uint8_t reg, uint8_t *val)
{
    for (int attempt = 0;; attempt++) {
        const esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1,
                                                          val, 1, I2C_TIMEOUT);
        if (err == ESP_OK) {
            if (attempt) ESP_LOGW(TAG, "read reg 0x%02x recovered on "
                                  "attempt %d", reg, attempt + 1);
            return ESP_OK;
        }
        if (!recover("read", reg, err, attempt)) return err;
    }
}

bool ls_xl9535_ready(void) { return s_ready; }

esp_err_t ls_xl9535_init(ls_i2c_bus_id_t bus, uint8_t addr)
{
    if (s_ready) return ESP_OK;
    s_bus = bus;

    /* LS-1239: 3 probes 50 ms apart failed 1 boot in 48 (SDA held low
       after a mid-transfer reset) and left every rail off. Clear between
       attempts and back off up to ~1 s. */
    static const uint16_t BACKOFF_MS[] = { 25, 50, 100, 200, 300, 300 };
    const int attempts = (int)(sizeof(BACKOFF_MS) / sizeof(BACKOFF_MS[0])) + 1;

    esp_err_t err = ESP_FAIL;
    int sda = -1, scl = -1;
    for (int attempt = 0; attempt < attempts; attempt++) {
        err = ls_i2c_probe(bus, addr, I2C_TIMEOUT);
        if (err == ESP_OK) {
            if (attempt) ESP_LOGW(TAG, "0x%02x answered on attempt %d",
                                  addr, attempt + 1);
            break;
        }
        ls_i2c_line_levels(bus, &sda, &scl);
        const esp_err_t clear = ls_i2c_bus_clear(bus);
        ESP_LOGW(TAG, "probe %d/%d at 0x%02x: %s, SDA %d SCL %d, bus clear %s",
                 attempt + 1, attempts, addr, esp_err_to_name(err), sda, scl,
                 esp_err_to_name(clear));
        if (attempt + 1 < attempts)
            vTaskDelay(pdMS_TO_TICKS(BACKOFF_MS[attempt]));
    }
    if (err != ESP_OK) {
        ls_i2c_line_levels(bus, &sda, &scl);
        ESP_LOGE(TAG, "no ACK at 0x%02x on bus %d after %d probes: %s, "
                 "now SDA %d SCL %d", addr, (int)bus, attempts,
                 esp_err_to_name(err), sda, scl);
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
