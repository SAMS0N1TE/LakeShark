#include "ls_i2c.h"

esp_err_t ls_i2c_device(ls_i2c_bus_id_t id, uint8_t addr, uint32_t hz,
                        i2c_master_dev_handle_t *out)
{
    (void)id; (void)addr; (void)hz;
    if (out) *out = NULL;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t ls_i2c_probe(ls_i2c_bus_id_t id, uint8_t addr, int timeout)
{
    (void)id; (void)addr; (void)timeout;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *data,
                              size_t len, int timeout)
{
    (void)dev; (void)data; (void)len; (void)timeout;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev,
                                      const uint8_t *tx, size_t tx_len,
                                      uint8_t *rx, size_t rx_len, int timeout)
{
    (void)dev; (void)tx; (void)tx_len; (void)rx; (void)rx_len; (void)timeout;
    return ESP_ERR_NOT_SUPPORTED;
}
