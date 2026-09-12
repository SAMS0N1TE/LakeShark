/* XL9535 / TCA9535-compatible 16-bit I2C GPIO expander. */

#ifndef LS_XL9535_H
#define LS_XL9535_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "ls_i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bank 0 */
#define LS_XL9535_IO0   0
#define LS_XL9535_IO1   1
#define LS_XL9535_IO2   2
#define LS_XL9535_IO3   3
#define LS_XL9535_IO4   4
#define LS_XL9535_IO5   5
#define LS_XL9535_IO6   6
#define LS_XL9535_IO7   7
/* Bank 1 - the vendor's IO10..IO17, which are bits 8..15 */
#define LS_XL9535_IO10  8
#define LS_XL9535_IO11  9
#define LS_XL9535_IO12  10
#define LS_XL9535_IO13  11
#define LS_XL9535_IO14  12
#define LS_XL9535_IO15  13
#define LS_XL9535_IO16  14
#define LS_XL9535_IO17  15

#define LS_XL9535_PIN_COUNT 16

/* Bring the expander up on the given bus.  Idempotent.  Returns
   ESP_ERR_NOT_FOUND when the part does not ACK, which on a board that
   declares one is a wiring or power fault, not a soft condition. */
esp_err_t ls_xl9535_init(ls_i2c_bus_id_t bus, uint8_t addr);

bool ls_xl9535_ready(void);

/* Direction: true drives the pin, false leaves it an input (the reset
   default for every pin on this part). */
esp_err_t ls_xl9535_set_dir(int pin, bool output);

/* Level on an output.  Sets the shadow and writes the whole bank, because
   the part has no per-bit write. */
esp_err_t ls_xl9535_set(int pin, bool level);

/* Level as read back from the input register. */
esp_err_t ls_xl9535_get(int pin, bool *level);

/* Convenience: configure as output and drive, in one call. */
esp_err_t ls_xl9535_out(int pin, bool level);

#ifdef __cplusplus
}
#endif

#endif /* LS_XL9535_H */
