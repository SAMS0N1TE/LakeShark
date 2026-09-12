#ifndef LS_SHIM_I2C_MASTER_H
#define LS_SHIM_I2C_MASTER_H
typedef void *i2c_master_bus_handle_t;
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef void *i2c_master_dev_handle_t;

/* The two transfers the register-mapped parts on this board use. Declared
   here and left to the test to define, because what a fake should say back
   depends entirely on which part is being modelled - an expander answers
   from a register file, a keypad from an event queue. */
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *buf,
                              size_t len, int timeout_ms);
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev,
                                      const uint8_t *tx, size_t tx_len,
                                      uint8_t *rx, size_t rx_len,
                                      int timeout_ms);

#endif
