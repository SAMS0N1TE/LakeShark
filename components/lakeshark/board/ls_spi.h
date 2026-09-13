/* Shared SPI master bus. */

#ifndef LS_SPI_H
#define LS_SPI_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/spi_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LS_SPI_RADIO = 0,      /* SX1262 and the T-MixRF parts share these wires */
    LS_SPI_BUS_COUNT
} ls_spi_bus_id_t;

/* True when the board declares pins for this bus at all. */
bool ls_spi_bus_present(ls_spi_bus_id_t id);

/* Human-readable pins, for the console. False when the board has no such bus. */
bool ls_spi_pins(ls_spi_bus_id_t id, int *sclk, int *mosi, int *miso);

/* Create the bus if it does not exist. Safe from any task; the first caller
   wins and the rest get the same bus. */
esp_err_t ls_spi_bus(ls_spi_bus_id_t id);

/* Register a device and return its handle.

   cs_gpio may be -1, which means this device drives its own chip select and
   IDF must not touch it. That is not an escape hatch, it is required for the
   CC1101 and the nRF24; see the header note.

   mode is the SPI mode, 0 for everything here except the ST25R3916, which is
   1. queue_size 1 suits register-poking parts; a part that streams wants more. */
esp_err_t ls_spi_device(ls_spi_bus_id_t id, int cs_gpio, uint8_t mode,
                        int clock_hz, int queue_size,
                        spi_device_handle_t *out);

/* Hold the bus across several transactions that are logically one. Always
   pair them, and keep the held region short - nothing else on the bus can
   move until release. */
esp_err_t ls_spi_hold(spi_device_handle_t dev);
void      ls_spi_release(spi_device_handle_t dev);
esp_err_t ls_spi_remove(ls_spi_bus_id_t id, spi_device_handle_t dev);

/* Console helper: describe the bus and what is on it. */
void ls_spi_diagnostics(void);

#ifdef __cplusplus
}
#endif

#endif /* LS_SPI_H */
