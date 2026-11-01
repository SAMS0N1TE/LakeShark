#pragma once
#include "sdkconfig.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Version 1: asynchronous operations return a nonzero id, zero means the
 * bounded command queue is full / radio unavailable. IDs are not addresses.
 * Events are polled on the caller's task; no UI callbacks run on NimBLE. */
enum { HID_API_VERSION = 1, HID_CANDIDATES = 24, HID_BONDS = 4, HID_CONNECTIONS = 2 };
typedef enum {
    HID_DISCONNECTED,
    HID_CONNECTING,
    HID_SECURING,
    HID_DISCOVERING,
    HID_SUBSCRIBING,
    HID_READY,
    HID_BACKOFF,
    HID_NEEDS_PAIRING,
    HID_UNSUPPORTED
} hid_state_t;
typedef enum { HID_AUTO, HID_BOOT, HID_REPORT } hid_protocol_t;
typedef enum {
    HID_OK,
    HID_AUTHENTICATION_FAILED,
    HID_BOND_STORE_FULL,
    HID_NO_CONNECTION_SLOT,
    HID_RADIO_UNAVAILABLE,
    HID_UNSUPPORTED_REPORT,
    HID_CANCELLED,
    HID_TIMEOUT,
    HID_NOT_FOUND,
    HID_IO_ERROR,
    HID_BUSY
} hid_error_t;
typedef enum {
    HID_EV_DISCOVERY,
    HID_EV_STATE,
    HID_EV_PASSKEY_DISPLAY,
    HID_EV_PASSKEY_EXPIRED,
    HID_EV_PAIRED,
    HID_EV_BONDS,
    HID_EV_CAPABILITIES,
    HID_EV_DONE,
    HID_EV_ERROR
} hid_event_kind_t;
typedef struct {
    uint32_t id, version, generation, fingerprint;
    uint8_t address[6], address_type;
    char name[32];
    int8_t rssi;
    uint16_t appearance;
    bool connectable, directed, advertised_hid, saved, reconnect, encrypted, authenticated, bonded;
    bool keyboard, mouse, consumer, wheel, raw_state;
    uint8_t buttons, protocol, state, error;
    uint16_t interval, latency, timeout, mtu, unsupported;
    uint64_t last_seen_us;
    uint32_t identity_id;
    char layout[4];
} hid_device_t;
typedef struct {
    uint32_t sequence, operation, device, generation;
    uint64_t timestamp_us;
    uint8_t kind, state, error;
    uint32_t passkey; /* only PASSKEY_DISPLAY; never stored */
} hid_event_t;
typedef struct {
    uint32_t reports, malformed, queue_drops, event_drops, reconnects, decode_max_us;
    size_t psram_bytes;
} hid_stats_t;
/* Explicit raw-input snapshot, separate from ordinary device/settings events. */
typedef struct {
    uint8_t keys[32], buttons;
    int16_t px, py;
    uint32_t sequence;
} hid_input_state_t;
#if CONFIG_LS_BLE_HEAD
uint32_t hid_scan_start(unsigned seconds);
uint32_t hid_scan_stop(void);
uint32_t hid_pair(uint32_t candidate, bool mouse_just_works);
uint32_t hid_cancel(uint32_t operation);
uint32_t hid_connect(uint32_t device);
uint32_t hid_disconnect(uint32_t device);
uint32_t hid_forget(uint32_t device);
uint32_t hid_forget_all(void);
uint32_t hid_set_reconnect(uint32_t device, bool on);
uint32_t hid_set_protocol(uint32_t device, hid_protocol_t protocol);
size_t hid_list_devices(hid_device_t *out, size_t capacity, uint32_t *version);
size_t hid_list_bonds(hid_device_t *out, size_t capacity, uint32_t *version);
bool hid_get_capabilities(uint32_t device, hid_device_t *out);
void hid_get_stats(hid_stats_t *out);
void hid_get_input_state(hid_input_state_t *out);
uint32_t hid_release_input(void); /* focus loss: cancel repeats and release BLE state */
/* Independent subscriptions: keep a cursor per consumer, initially zero.
 * Sequence gaps indicate overflow; recover by requesting a fresh snapshot. */
bool hid_events_next(uint32_t *cursor, hid_event_t *out);
int ble_hid_console(int argc, char **argv);
int ble_hid_input_console(int argc, char **argv);
/* Shared-stack hooks. Only these internal hooks run on NimBLE's task. */
void ble_hid_init(void);
void ble_hid_sync(void);
void ble_hid_reset(void);
bool ble_hid_scan_wanted(void);
bool ble_hid_discovery_active(void);
bool ble_hid_busy(void);
bool ble_hid_head_blocked(void);
void ble_hid_advert(const void *disc);
void ble_hid_console_pump(void);
#endif
#ifdef __cplusplus
}
#endif
