#ifndef BLE_LINK_SCAN_H
#define BLE_LINK_SCAN_H
#include <stdbool.h>
/* 0.625 ms units: 30 ms / 100 ms. Passive reception requests no scan
 * responses. 70% nominal idle time leaves room for connection events; NimBLE
 * arbitrates collisions. This is a conservative unmeasured duty budget, not
 * a guarantee of link throughput or RF coverage on the P4. */
#define BLE_LINK_OBSERVER_INTERVAL 160
#define BLE_LINK_OBSERVER_WINDOW 48
typedef enum { BLE_SCAN_OFF, BLE_SCAN_HEAD, BLE_SCAN_OBSERVER } ble_scan_mode_t;
static inline ble_scan_mode_t ble_scan_wanted(bool run, bool connected,
    bool connecting, bool observers, bool passive, bool backoff) {
    if (!run || connecting) return BLE_SCAN_OFF;
    if (connected || passive || backoff)
        return observers ? BLE_SCAN_OBSERVER : BLE_SCAN_OFF;
    return BLE_SCAN_HEAD;
}
typedef enum { BLE_SCAN_KEEP, BLE_SCAN_START, BLE_SCAN_RESTART, BLE_SCAN_STOP } ble_scan_action_t;
static inline ble_scan_action_t ble_scan_action(ble_scan_mode_t want,
    ble_scan_mode_t current, bool active) {
    if (want == BLE_SCAN_OFF) return active ? BLE_SCAN_STOP : BLE_SCAN_KEEP;
    if (!active) return BLE_SCAN_START;
    return want == current ? BLE_SCAN_KEEP : BLE_SCAN_RESTART;
}
#endif
