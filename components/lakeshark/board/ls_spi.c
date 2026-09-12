/* See ls_spi.h for the topology and why it is not up for debate. */
#include "ls_spi.h"

#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "ls_board.h"

static const char *TAG = "ls_spi";

typedef struct {
    int sclk;
    int mosi;
    int miso;
    spi_host_device_t host;
} bus_pins_t;

/* -1 means the board did not declare this bus. The host is fixed per role so
   two boards cannot disagree about which controller is which - the same rule
   ls_i2c.c follows, and for the same reason. */
static const bus_pins_t BUS[LS_SPI_BUS_COUNT] = {
    [LS_SPI_RADIO] = {
#if defined(LS_BOARD_SPI_SCLK_GPIO) && defined(LS_BOARD_SPI_MOSI_GPIO) && \
    defined(LS_BOARD_SPI_MISO_GPIO)
        LS_BOARD_SPI_SCLK_GPIO, LS_BOARD_SPI_MOSI_GPIO,
        LS_BOARD_SPI_MISO_GPIO, SPI2_HOST
#else
        -1, -1, -1, SPI2_HOST
#endif
    },
};

static bool             s_up[LS_SPI_BUS_COUNT];
static int              s_devices[LS_SPI_BUS_COUNT];
static SemaphoreHandle_t s_lock;

static void lock_init_once(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
}

bool ls_spi_bus_present(ls_spi_bus_id_t id)
{
    if (id < 0 || id >= LS_SPI_BUS_COUNT) return false;
    return BUS[id].sclk >= 0 && BUS[id].mosi >= 0 && BUS[id].miso >= 0;
}

bool ls_spi_pins(ls_spi_bus_id_t id, int *sclk, int *mosi, int *miso)
{
    if (!ls_spi_bus_present(id)) return false;
    if (sclk) *sclk = BUS[id].sclk;
    if (mosi) *mosi = BUS[id].mosi;
    if (miso) *miso = BUS[id].miso;
    return true;
}

esp_err_t ls_spi_bus(ls_spi_bus_id_t id)
{
    if (!ls_spi_bus_present(id)) return ESP_ERR_NOT_SUPPORTED;
    lock_init_once();
    if (!s_lock) return ESP_ERR_NO_MEM;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (!s_up[id]) {
        spi_bus_config_t cfg = {
            .mosi_io_num = BUS[id].mosi,
            .miso_io_num = BUS[id].miso,
            .sclk_io_num = BUS[id].sclk,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            /* Big enough for a CC1101 or ST25R3916 FIFO burst in one go.
               Larger costs DMA-capable memory for no gain here. */
            .max_transfer_sz = 512,
        };
        err = spi_bus_initialize(BUS[id].host, &cfg, SPI_DMA_CH_AUTO);
        if (err == ESP_OK) {
            s_up[id] = true;
            ESP_LOGI(TAG, "bus %d up on SCLK%d/MOSI%d/MISO%d (SPI%d)",
                     (int)id, BUS[id].sclk, BUS[id].mosi, BUS[id].miso,
                     (int)BUS[id].host + 1);
        } else {
            ESP_LOGE(TAG, "bus %d init failed: %s", (int)id,
                     esp_err_to_name(err));
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t ls_spi_device(ls_spi_bus_id_t id, int cs_gpio, uint8_t mode,
                        int clock_hz, int queue_size,
                        spi_device_handle_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    esp_err_t err = ls_spi_bus(id);
    if (err != ESP_OK) return err;

    spi_device_interface_config_t dev = {
        .mode = mode,
        .clock_speed_hz = clock_hz,
        /* -1 is deliberate and load-bearing for the CC1101 and the nRF24:
           their ready handshake holds CS low across a wait, which hardware CS
           cannot express, so those drivers own the pin. */
        .spics_io_num = cs_gpio,
        .queue_size = queue_size > 0 ? queue_size : 1,
    };
    err = spi_bus_add_device(BUS[id].host, &dev, out);
    if (err == ESP_OK) {
        s_devices[id]++;
        ESP_LOGI(TAG, "device on bus %d: cs=%d mode=%u %d Hz",
                 (int)id, cs_gpio, (unsigned)mode, clock_hz);
    } else {
        ESP_LOGE(TAG, "add device failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t ls_spi_hold(spi_device_handle_t dev)
{
    return dev ? spi_device_acquire_bus(dev, portMAX_DELAY) : ESP_ERR_INVALID_ARG;
}

void ls_spi_release(spi_device_handle_t dev)
{
    if (dev) spi_device_release_bus(dev);
}

void ls_spi_diagnostics(void)
{
    for (int i = 0; i < LS_SPI_BUS_COUNT; i++) {
        if (!ls_spi_bus_present((ls_spi_bus_id_t)i)) {
            printf("spi bus %d: board declares no pins\n", i);
            continue;
        }
        printf("spi bus %d: sclk=%d mosi=%d miso=%d host=SPI%d up=%s devices=%d\n",
               i, BUS[i].sclk, BUS[i].mosi, BUS[i].miso, (int)BUS[i].host + 1,
               s_up[i] ? "yes" : "no", s_devices[i]);
    }
}
