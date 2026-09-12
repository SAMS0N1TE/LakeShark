/* See ls_i2c.h for why this exists at all. */
#include "ls_i2c.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "ls_board.h"

static const char *TAG = "ls_i2c";

typedef struct {
    int sda;
    int scl;
    int port;
} bus_pins_t;

/* -1 means the board did not declare this bus.  The IDF port number is fixed
   per role so two boards cannot disagree about which controller is which. */
static const bus_pins_t BUS[LS_I2C_BUS_COUNT] = {
    [LS_I2C_PRIMARY] = {
#if defined(LS_BOARD_I2C_SDA_GPIO) && defined(LS_BOARD_I2C_SCL_GPIO)
        LS_BOARD_I2C_SDA_GPIO, LS_BOARD_I2C_SCL_GPIO, 0
#else
        -1, -1, -1
#endif
    },
    [LS_I2C_SECONDARY] = {
#if defined(LS_BOARD_I2C2_SDA_GPIO) && defined(LS_BOARD_I2C2_SCL_GPIO)
        LS_BOARD_I2C2_SDA_GPIO, LS_BOARD_I2C2_SCL_GPIO, 1
#else
        -1, -1, -1
#endif
    },
};

static i2c_master_bus_handle_t s_bus[LS_I2C_BUS_COUNT];
static SemaphoreHandle_t s_lock;

/* app_main calls into here long before any other task exists, but the codec
   and the RTC come later and from their own tasks, so the cache needs a
   lock.  Created on the first call, which is single-threaded by
   construction. */
static void lock_init_once(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
}

bool ls_i2c_bus_present(ls_i2c_bus_id_t id)
{
    if (id < 0 || id >= LS_I2C_BUS_COUNT) return false;
    return BUS[id].sda >= 0 && BUS[id].scl >= 0;
}

bool ls_i2c_pins(ls_i2c_bus_id_t id, int *sda, int *scl)
{
    if (!ls_i2c_bus_present(id)) return false;
    if (sda) *sda = BUS[id].sda;
    if (scl) *scl = BUS[id].scl;
    return true;
}

esp_err_t ls_i2c_bus(ls_i2c_bus_id_t id, i2c_master_bus_handle_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!ls_i2c_bus_present(id)) return ESP_ERR_NOT_SUPPORTED;

    lock_init_once();
    if (!s_lock) return ESP_ERR_NO_MEM;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (!s_bus[id]) {
        i2c_master_bus_config_t cfg = {
            .i2c_port = BUS[id].port,
            .sda_io_num = (gpio_num_t)BUS[id].sda,
            .scl_io_num = (gpio_num_t)BUS[id].scl,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        err = i2c_new_master_bus(&cfg, &s_bus[id]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "bus %d (SDA%d/SCL%d) init failed: %s", (int)id,
                     BUS[id].sda, BUS[id].scl, esp_err_to_name(err));
            s_bus[id] = NULL;
        } else {
            ESP_LOGI(TAG, "bus %d up on SDA%d/SCL%d (I2C%d)", (int)id,
                     BUS[id].sda, BUS[id].scl, BUS[id].port);
        }
    }
    if (err == ESP_OK) *out = s_bus[id];
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t ls_i2c_probe(ls_i2c_bus_id_t id, uint8_t addr, int timeout_ms)
{
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = ls_i2c_bus(id, &bus);
    if (err != ESP_OK) return err;
    return i2c_master_probe(bus, addr, timeout_ms);
}

esp_err_t ls_i2c_device(ls_i2c_bus_id_t id, uint8_t addr, uint32_t hz,
                        i2c_master_dev_handle_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = ls_i2c_bus(id, &bus);
    if (err != ESP_OK) return err;

    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = hz,
    };
    return i2c_master_bus_add_device(bus, &dev, out);
}
