#ifndef BLE_HCI_RX_GUARD_H
#define BLE_HCI_RX_GUARD_H

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ble_hci_rx_prepare(esp_err_t (*init)(void));
esp_err_t ble_hci_controller_prepare(esp_err_t (*init)(void),
    esp_err_t (*disable)(void), esp_err_t (*enable)(void));
uint32_t ble_hci_rx_early_packets(void);

#ifdef __cplusplus
}
#endif
#endif
