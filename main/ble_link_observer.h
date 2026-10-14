#ifndef BLE_LINK_OBSERVER_H
#define BLE_LINK_OBSERVER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "tui/ls_ble_ad.h"

/* One NimBLE producer, one dispatch worker. Complete AD reports and scan
 * responses are separate records, paired by address/type by listeners.
 * Never truncate extended AD: an oversized/incomplete report is dropped. */
#define BLE_LINK_AD_MAX LS_BLE_AD_MAX
#define BLE_LINK_AD_DEPTH 256
#define BLE_LINK_AD_LISTENERS 4
typedef struct {
    uint8_t addr[6], addr_type;
    int8_t rssi;
    bool scan_response;
    uint16_t len;
    int64_t seen_us;
    uint8_t data[BLE_LINK_AD_MAX];
} ble_link_advert_t;
typedef void (*ble_link_advert_fn)(const ble_link_advert_t *, void *);
typedef struct {
    struct {
        uint8_t addr[6], type, sid;
        bool used, response, discard;
        uint16_t len;
        int64_t at;
        uint8_t data[BLE_LINK_AD_MAX];
    } chains[4]; /* Producer-only bounded assembly; never parse partial AD. */
    int64_t chain_quarantine;
    uint32_t write, read, dropped, received, dispatched, overflow, invalid, high_water;
    ble_link_advert_t records[BLE_LINK_AD_DEPTH];
    struct { ble_link_advert_fn fn; void *ctx; } listeners[BLE_LINK_AD_LISTENERS];
    unsigned listener_count;
} ble_link_observer_t;
/* status: 0 complete, 1 more fragments, 2 truncated. Chain key includes SID
 * and scan-response bit. Truncated/overflowed chains are discarded intact. */
bool ble_link_observer_ext(ble_link_observer_t *, const uint8_t addr[6],
    uint8_t type, uint8_t sid, int rssi, bool response, uint8_t status,
    const uint8_t *data, size_t len, int64_t us);
/* Register before producer/dispatch start; no mutation while dispatch runs. */
bool ble_link_observer_subscribe(ble_link_observer_t *, ble_link_advert_fn, void *);
/* Bounded copy only: no allocations, locks, callbacks, logging or waits. */
bool ble_link_observer_push(ble_link_observer_t *, const uint8_t addr[6],
    uint8_t type, int rssi, bool response, const uint8_t *, size_t, int64_t);
/* Callbacks run on the consumer, while its record remains owned by the ring.
 * Do not free or retain record/data pointers, or invoke NimBLE's AD parser.
 * Copy any retained fields; parse with the reentrant ls_ble_ad walker. */
unsigned ble_link_observer_dispatch(ble_link_observer_t *, unsigned budget);
#endif
