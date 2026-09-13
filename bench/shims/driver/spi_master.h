#ifndef LS_SHIM_SPI_MASTER_H
#define LS_SHIM_SPI_MASTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { SPI1_HOST = 0, SPI2_HOST = 1, SPI3_HOST = 2 } spi_host_device_t;

#define SPI_DMA_CH_AUTO 3

typedef struct {
    int mosi_io_num, miso_io_num, sclk_io_num;
    int quadwp_io_num, quadhd_io_num;
    int max_transfer_sz;
} spi_bus_config_t;

typedef struct {
    uint8_t mode;
    int     clock_speed_hz;
    int     spics_io_num;
    int     queue_size;
} spi_device_interface_config_t;

typedef struct {
    size_t      length;      /* bits */
    const void *tx_buffer;
    void       *rx_buffer;
} spi_transaction_t;

typedef struct spi_device_t *spi_device_handle_t;

esp_err_t spi_bus_initialize(spi_host_device_t host,
                             const spi_bus_config_t *cfg, int dma);
esp_err_t spi_bus_add_device(spi_host_device_t host,
                             const spi_device_interface_config_t *dev,
                             spi_device_handle_t *out);
esp_err_t spi_device_transmit(spi_device_handle_t dev, spi_transaction_t *t);
esp_err_t spi_bus_remove_device(spi_device_handle_t dev);
esp_err_t spi_device_acquire_bus(spi_device_handle_t dev, uint32_t wait);
void      spi_device_release_bus(spi_device_handle_t dev);

/* ---- host-test controls ------------------------------------------------ */

/* Answer a transfer. `tx` is what the driver clocked out, `rx` is what it
   will read back; both are `len` bytes and alias the same buffer in the
   driver, exactly as they do on the real full-duplex bus. */
typedef void (*ls_shim_spi_responder_t)(const uint8_t *tx, uint8_t *rx,
                                        size_t len, void *ctx);

void     ls_shim_spi_reset(void);
/* Clears the responder and the counters and leaves the bus and its devices
   registered. ls_spi.c caches the fact that a bus is up and offers no
   teardown - deliberately, since four parts share it - so a full reset makes
   the two sides disagree and every later add_device fails. */
void     ls_shim_spi_reset_counters(void);
void     ls_shim_spi_on_transfer(ls_shim_spi_responder_t fn, void *ctx);
unsigned ls_shim_spi_transfers(void);
/* The bytes of the most recent transfer, as the driver sent them. */
const uint8_t *ls_shim_spi_last_tx(size_t *len);
bool     ls_shim_spi_bus_up(spi_host_device_t host);
unsigned ls_shim_spi_devices(void);
void     ls_shim_spi_fail_next_init(esp_err_t err);

#ifdef __cplusplus
}
#endif
#endif
