#ifndef BLE_LINK_H
#define BLE_LINK_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BLE_LINK_OFF,
    BLE_LINK_SYNCING,
    BLE_LINK_SCANNING,
    BLE_LINK_CONNECTING,
    BLE_LINK_DISCOVERING,
    BLE_LINK_READY,
} ble_link_state_t;

esp_err_t ble_link_start(void);
void      ble_link_stop(void);

ble_link_state_t ble_link_state(void);
const char      *ble_link_state_name(void);

void ble_link_set_name_filter(const char *substr);
void ble_link_get_name_filter(char *out, size_t len);

void ble_link_peer(char *name, size_t name_len, char *addr, size_t addr_len);

void ble_link_stats(uint32_t *rx_lines, uint32_t *tx_frames, uint32_t *drops);

void ble_link_set_tel_hz(int hz);
int  ble_link_tel_hz(void);

void ble_link_allow_telemetry(bool allow);

void ble_link_set_verbose(bool en);

void ble_link_rescan(void);

bool      ble_link_passkey_pending(void);
esp_err_t ble_link_submit_passkey(uint32_t code);

#ifdef __cplusplus
}
#endif

#endif
