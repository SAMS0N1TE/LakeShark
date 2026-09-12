/* Shared I2C master buses. */

#ifndef LS_I2C_H
#define LS_I2C_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LS_I2C_PRIMARY = 0,
    LS_I2C_SECONDARY = 1,
    LS_I2C_BUS_COUNT
} ls_i2c_bus_id_t;

/* Hand back the bus, creating it on first use.  Safe to call from any task;
   the first caller wins and the rest get the same handle. */
esp_err_t ls_i2c_bus(ls_i2c_bus_id_t id, i2c_master_bus_handle_t *out);

/* True when the board declares pins for this bus at all. */
bool ls_i2c_bus_present(ls_i2c_bus_id_t id);

/* One address, one ACK check.  timeout_ms is per probe. */
esp_err_t ls_i2c_probe(ls_i2c_bus_id_t id, uint8_t addr, int timeout_ms);

/* Register a device on a bus and return its handle. */
esp_err_t ls_i2c_device(ls_i2c_bus_id_t id, uint8_t addr, uint32_t hz,
                        i2c_master_dev_handle_t *out);

/* Human-readable pins for a bus, for the console.  Returns false when the
   board has no such bus. */
bool ls_i2c_pins(ls_i2c_bus_id_t id, int *sda, int *scl);

#ifdef __cplusplus
}
#endif

#endif /* LS_I2C_H */
