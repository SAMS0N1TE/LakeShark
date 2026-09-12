#ifndef BLE_LINK_H
#define BLE_LINK_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "ble_link_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ble_link_state_t and ble_link_state_name_of() live in ble_link_core.h so the
   bench can drive the state graph without an ESP-IDF build. */

esp_err_t ble_link_start(void);
void      ble_link_stop(void);

ble_link_state_t ble_link_state(void);
const char      *ble_link_state_name(void);

void ble_link_set_name_filter(const char *substr);
void ble_link_get_name_filter(char *out, size_t len);

void ble_link_peer(char *name, size_t name_len, char *addr, size_t addr_len);

void ble_link_stats(uint32_t *rx_lines, uint32_t *tx_frames, uint32_t *drops);

void ble_link_rx_debug(uint16_t *tx_hnd, uint16_t *svc_start, uint16_t *svc_end,
                       uint32_t *foreign);

void ble_link_notify_stats(uint32_t *notifies, uint32_t *bytes);

void ble_link_set_tel_hz(int hz);
int  ble_link_tel_hz(void);

void ble_link_allow_telemetry(bool allow);

void ble_link_set_verbose(bool en);

void ble_link_rescan(void);

bool      ble_link_passkey_pending(void);
esp_err_t ble_link_submit_passkey(uint32_t code);

/* Peer pin management.  A pinned peer wins over an unknown one
   advertising the same service, so two boards no longer race for one
   Flipper.  The pinned address survives a reboot (NVS-backed). */

/* Report whether a peer is pinned, and if so its address as
   "aa:bb:cc:dd:ee:ff" (matches ble_link_peer's addr format).  Returns
   true when out_addr was filled with a real address, false otherwise. */
bool ble_link_get_pinned(char *out_addr, size_t out_len);

/* True when a Flipper advertising its OWN BLE profile - not
   ours - was heard in the last two minutes. That is a head sitting
   right there with the LakeShark app closed, which looks identical to
   "no head on the air" unless the status line says otherwise. */
bool ble_link_stock_head_seen(void);

/* Pin `addr` as this board's peer, or - if addr is NULL or empty - pin
   whichever peer is currently connected.  Returns ESP_OK on success,
   ESP_ERR_INVALID_ARG when addr is malformed, ESP_ERR_NOT_FOUND when
   asked to pin the current peer and nothing is connected. */
esp_err_t ble_link_pin_peer(const char *addr);

/* Forget the pinned peer.  The scanner falls back to matching by service. */
void ble_link_unpin_peer(void);

/* Forget every stored bond and unpin the peer, dropping the current connection so the next attempt starts from a clean slate. */

esp_err_t ble_link_forget_bonds(void);

/* True when a GATT connection is currently up (used by `ble show`). */
bool ble_link_is_connected(void);
esp_err_t ble_link_rssi(int *rssi);

#ifdef __cplusplus
}
#endif

#endif
