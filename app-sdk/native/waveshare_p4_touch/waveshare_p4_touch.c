#include "waveshare_p4_touch.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include <string.h>

enum { REPORT = 0x814e, PRODUCT = 0x8140, I2C_TIMEOUT_MS = 5 };
static i2c_master_dev_handle_t touch;
static uint16_t width, height;

static uint16_t le16(const uint8_t *bytes)
{
    return bytes[0] | (uint16_t)bytes[1] << 8;
}

static esp_err_t read_register(i2c_master_dev_handle_t device, uint16_t address,
                               uint8_t *data, size_t length)
{
    const uint8_t reg[] = {address >> 8, address & 0xff};
    return i2c_master_transmit_receive(device, reg, sizeof(reg), data, length, I2C_TIMEOUT_MS);
}

esp_err_t waveshare_p4_touch_init(void)
{
    if (touch) return ESP_OK;
    i2c_master_bus_handle_t bus;
    bool owns_bus = false;
    if (i2c_master_get_bus_handle(I2C_NUM_0, &bus) != ESP_OK) {
        const i2c_master_bus_config_t config = {
            .i2c_port = I2C_NUM_0, .sda_io_num = GPIO_NUM_7, .scl_io_num = GPIO_NUM_8,
            .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        const esp_err_t result = i2c_new_master_bus(&config, &bus);
        if (result != ESP_OK) return result;
        owns_bus = true;
    }
    static const uint8_t addresses[] = {0x5d, 0x14};
    for (size_t i = 0; i < sizeof(addresses); ++i) {
        const i2c_device_config_t config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addresses[i],
            .scl_speed_hz = 400000,
        };
        i2c_master_dev_handle_t device;
        if (i2c_master_bus_add_device(bus, &config, &device) != ESP_OK) continue;
        uint8_t info[10];
        if (read_register(device, PRODUCT, info, sizeof(info)) == ESP_OK &&
            memcmp(info, "911", 3) == 0 && le16(info + 6) && le16(info + 8)) {
            width = le16(info + 6);
            height = le16(info + 8);
            touch = device;
            ESP_LOGI("p4_touch", "GT911 at 0x%02x, %ux%u", addresses[i], width, height);
            return ESP_OK;
        }
        i2c_master_bus_rm_device(device);
    }
    if (owns_bus) i2c_del_master_bus(bus);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t waveshare_p4_touch_read(waveshare_p4_touch_sample *sample)
{
    if (!touch || !sample) return ESP_ERR_INVALID_STATE;
    uint8_t report[6];
    esp_err_t result = read_register(touch, REPORT, report, sizeof(report));
    if (result != ESP_OK) return result;
    if (!(report[0] & 0x80)) return ESP_ERR_NOT_FOUND;
    const uint8_t ack[] = {REPORT >> 8, REPORT & 0xff, 0};
    result = i2c_master_transmit(touch, ack, sizeof(ack), I2C_TIMEOUT_MS);
    if (result != ESP_OK) return result;

    const uint16_t x = le16(report + 2), y = le16(report + 4);
    *sample = (waveshare_p4_touch_sample){
        .x = x, .y = y, .id = report[1] & 0x0f,
        .down = (report[0] & 0x0f) == 1 && x < width && y < height,
    };
    return ESP_OK;
}
