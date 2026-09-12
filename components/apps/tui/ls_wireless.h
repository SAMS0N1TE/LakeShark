#ifndef LS_WIRELESS_H
#define LS_WIRELESS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LS_WIRELESS_APS 16
#define LS_WIRELESS_SAMPLES 60
#define LS_WIRELESS_WIFI_VALID 1
#define LS_WIRELESS_BT_VALID 2
#define LS_WIRELESS_RATE_VALID 4

typedef enum {
    LS_WIRELESS_NONE, LS_WIRELESS_SCAN, LS_WIRELESS_JOIN,
    LS_WIRELESS_SAVED, LS_WIRELESS_LEAVE, LS_WIRELESS_FORGET,
    LS_WIRELESS_BT_START, LS_WIRELESS_BT_STOP, LS_WIRELESS_BT_RESCAN
} ls_wireless_op_t;

typedef struct {
    char ssid[33];
    int rssi, channel;
    bool secure;
} ls_wireless_ap_t;

typedef struct {
    int8_t wifi, bt;
    uint8_t valid;
    uint16_t rx, tx;
} ls_wireless_sample_t;

typedef struct {
    ls_wireless_sample_t samples[LS_WIRELESS_SAMPLES];
    unsigned count, next;
    uint32_t last_ms, last_rx, last_tx;
    bool last_ready;
} ls_wireless_history_t;

typedef struct {
    bool wifi_available, bt_available, busy, wifi_connected, bt_ready;
    ls_wireless_op_t operation;
    uint32_t now_ms, scan_ms, scan_revision;
    int ap_count, bt_state;
    char message[80], wifi_status[112], ssid[33], ip[20];
    char bt_status[24], peer[40], address[20];
    int wifi_rssi, bt_rssi, channel;
    bool wifi_signal, bt_signal, stock_head_seen;
    uint32_t rx, tx, drops;
    ls_wireless_ap_t aps[LS_WIRELESS_APS];
    ls_wireless_history_t history;
} ls_wireless_snapshot_t;

/* only the worker talks to the radios; draw reads one snapshot. */
void ls_wireless_set_active(bool active);
void ls_wireless_get(ls_wireless_snapshot_t *out);
bool ls_wireless_request(ls_wireless_op_t op, const char *ssid, const char *pass);
void ls_wireless_history_push(ls_wireless_history_t *h, uint32_t now_ms,
    int wifi, bool wifi_valid, int bt, bool bt_valid, bool ready,
    uint32_t rx, uint32_t tx);

#ifdef __cplusplus
}
#endif
#endif
