/* ble_link - the flipper_link protocol carried over BLE instead of a UART.
 *
 * The Flipper Zero's BLE stack is peripheral-only: there is no public API for
 * scanning or acting as a GATT client. So the roles are forced - the P4 is the
 * central and connects out to the Flipper, which advertises its serial service.
 *
 * The Flipper app must have taken the serial profile over with
 * ble_profile_serial_set_event_callback(), otherwise the bytes we write are fed
 * to its RPC layer and parsed as protobuf rather than as our ASCII protocol.
 *
 * Wire format is byte-for-byte identical to the UART link, so both transports
 * share flipper_link's parser and telemetry builder.
 */
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
    BLE_LINK_SYNCING,     /* stack up, waiting for controller sync */
    BLE_LINK_SCANNING,
    BLE_LINK_CONNECTING,
    BLE_LINK_DISCOVERING, /* connected, walking the GATT database */
    BLE_LINK_READY,       /* subscribed; telemetry flowing */
} ble_link_state_t;

/* Start the NimBLE host and begin looking for a head. Safe to call once. */
esp_err_t ble_link_start(void);
void      ble_link_stop(void);

ble_link_state_t ble_link_state(void);
const char      *ble_link_state_name(void);

/* Name substring an advertiser must contain to be considered ours.
 * Defaults to "Flipper". */
void ble_link_set_name_filter(const char *substr);
void ble_link_get_name_filter(char *out, size_t len);

/* Peer name and address of the current/last connection, for the console. */
void ble_link_peer(char *name, size_t name_len, char *addr, size_t addr_len);

void ble_link_stats(uint32_t *rx_lines, uint32_t *tx_frames, uint32_t *drops);

/* Telemetry rate on the BLE transport, frames/sec. 0 disables. */
void ble_link_set_tel_hz(int hz);
int  ble_link_tel_hz(void);

/* Hold telemetry off until the radio pipeline has finished claiming its memory.
 *
 * The BLE stack comes up first (it has to - the controller is on the C6 and the
 * transport cannot get its buffers once the USB host has taken them), so a head
 * that is already advertising gets connected and subscribed at ~5 s, and the
 * 5 Hz stream then runs straight into the USB host posting its IQ transfers at
 * ~6 s. DMA-capable free memory collapses to a few hundred bytes for that
 * moment, the transport's ACL malloc fails, and NimBLE's hard assert reboots
 * the chip - on every boot, so the radio never gets past it.
 *
 * app_main() opens the gate once everything else is up. Command replies are
 * unaffected: they are rare, small, and are the head's only way of finding out
 * what is going on. */
void ble_link_allow_telemetry(bool allow);

void ble_link_set_verbose(bool en);

/* Force a disconnect and go back to scanning. */
void ble_link_rescan(void);

/* Pairing.
 *
 * The Flipper's serial profile is DisplayOnly and demands MITM protection, so
 * the association model is Passkey Entry with the head displaying and us
 * typing. There is no way to answer that from inside the firmware: the code is
 * on the head's screen. ble_link_passkey_pending() reports that a pairing is
 * parked waiting for it, and ble_link_submit_passkey() relays what the operator
 * read off the head.
 *
 * Only needed once per head - the bond persists in NVS. */
bool      ble_link_passkey_pending(void);
esp_err_t ble_link_submit_passkey(uint32_t code);   /* 0..999999 */

#ifdef __cplusplus
}
#endif

#endif /* BLE_LINK_H */
