#include "tui/ls_wireless.h"
#include "ls_wifi.h"
#include "ble_link.h"
#include "ls_flash_task.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include <stdio.h>
#include <string.h>

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_active, s_worker;
/* LINK could not allocate a 6 KiB DRAM stack after the radio and
 * C6 started. Reserve it and keep one sleeping worker across app visits. */
static DRAM_ATTR StackType_t s_worker_stack[6144 / sizeof(StackType_t)]
    __attribute__((aligned(16)));
static DRAM_ATTR StaticTask_t s_worker_tcb;
static TaskHandle_t s_worker_task;
static ls_wireless_op_t s_pending;
static char s_ssid[33], s_pass[65];
static ls_wireless_snapshot_t s_public, s_model;
static ls_wifi_scan_ap_t s_scan[LS_WIRELESS_APS];

static void wipe(char *text, size_t n)
{
    volatile char *p = text;
    while (n--) *p++ = 0;
}

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void publish(void)
{
    portENTER_CRITICAL(&s_mux);
    s_public = s_model;
    if (s_pending != LS_WIRELESS_NONE) {
        s_public.busy = true;
        s_public.operation = s_pending;
    }
    portEXIT_CRITICAL(&s_mux);
}

static void poll_radios(void)
{
    ls_wifi_scan_ap_t ap = {0};
    s_model.wifi_connected = ls_wifi_sta_connected();
    s_model.wifi_signal = s_model.wifi_connected && ls_wifi_sta_info(&ap) == ESP_OK;
    s_model.wifi_rssi = ap.rssi;
    s_model.channel = ap.channel;
    snprintf(s_model.ssid, sizeof(s_model.ssid), "%s", ap.ssid);
    ls_wifi_sta_ip(s_model.ip, sizeof(s_model.ip));
    ls_wifi_sta_status(s_model.wifi_status, sizeof(s_model.wifi_status));
    s_model.bt_state = ble_link_state();
    s_model.bt_ready = s_model.bt_state == BLE_LINK_READY;
    snprintf(s_model.bt_status, sizeof(s_model.bt_status), "%s",
             ble_link_state_name_of(s_model.bt_state));
    ble_link_peer(s_model.peer, sizeof(s_model.peer), s_model.address, sizeof(s_model.address));
    ble_link_stats(&s_model.rx, &s_model.tx, &s_model.drops);
    s_model.bt_signal = s_model.bt_ready && ble_link_rssi(&s_model.bt_rssi) == ESP_OK;
    s_model.stock_head_seen = ble_link_stock_head_seen();
    s_model.now_ms = now_ms();
    ls_wireless_history_push(&s_model.history, s_model.now_ms,
        s_model.wifi_rssi, s_model.wifi_signal, s_model.bt_rssi, s_model.bt_signal,
        s_model.bt_ready, s_model.rx, s_model.tx);
}

static esp_err_t perform(ls_wireless_op_t op, const char *ssid, const char *pass)
{
    switch (op) {
    case LS_WIRELESS_SCAN: {
        int n = ls_wifi_sta_scan(s_scan, LS_WIRELESS_APS);
        if (n < 0) return ESP_FAIL;
        s_model.ap_count = n;
        for (int i = 0; i < n; i++) {
            snprintf(s_model.aps[i].ssid, sizeof(s_model.aps[i].ssid), "%s", s_scan[i].ssid);
            s_model.aps[i].rssi = s_scan[i].rssi;
            s_model.aps[i].secure = s_scan[i].secure;
            s_model.aps[i].channel = s_scan[i].channel;
        }
        s_model.scan_ms = now_ms();
        s_model.scan_revision++;
        return ESP_OK;
    }
    case LS_WIRELESS_JOIN: return ls_wifi_sta_join(ssid, pass);
    case LS_WIRELESS_SAVED: return ls_wifi_sta_autojoin();
    case LS_WIRELESS_LEAVE: return ls_wifi_sta_leave();
    case LS_WIRELESS_FORGET: return ls_wifi_sta_forget();
    case LS_WIRELESS_BT_START: return ble_link_start();
    case LS_WIRELESS_BT_STOP: ble_link_stop(); return ESP_OK;
    case LS_WIRELESS_BT_RESCAN:
        if (ble_link_state() == BLE_LINK_OFF) return ble_link_start();
        ble_link_rescan(); return ESP_OK;
    default: return ESP_ERR_INVALID_ARG;
    }
}

static void worker(void *arg)
{
    (void)arg;
    uint32_t last_poll = 0;
#if CONFIG_LS_C6_LINK
    s_model.wifi_available = true;
#endif
#if CONFIG_LS_BLE_HEAD
    s_model.bt_available = true;
#endif
    for (;;) {
        char ssid[33], pass[65];
        portENTER_CRITICAL(&s_mux);
        ls_wireless_op_t op = s_pending;
        s_pending = LS_WIRELESS_NONE;
        memcpy(ssid, s_ssid, sizeof(ssid));
        memcpy(pass, s_pass, sizeof(pass));
        wipe(s_pass, sizeof(s_pass));
        bool stop = !s_active && op == LS_WIRELESS_NONE;
        portEXIT_CRITICAL(&s_mux);
        if (stop) {
            wipe(pass, sizeof(pass));
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            last_poll = 0;
            continue;
        }
        if (op != LS_WIRELESS_NONE) {
            s_model.busy = true;
            s_model.operation = op;
            snprintf(s_model.message, sizeof(s_model.message), "%s",
                op == LS_WIRELESS_SCAN ? "Scanning nearby networks..." : "Updating connection...");
            publish();
            esp_err_t rc = perform(op, ssid, pass);
            wipe(pass, sizeof(pass));
            if (rc == ESP_OK) {
                const char *done = "Connection updated";
                if (op == LS_WIRELESS_SCAN) done = "Scan complete; tap a network to join";
                if (op == LS_WIRELESS_JOIN || op == LS_WIRELESS_SAVED) done = "Joining network; credentials saved";
                if (op == LS_WIRELESS_FORGET) done = "Saved network removed";
                snprintf(s_model.message, sizeof(s_model.message), "%s", done);
            } else {
                snprintf(s_model.message, sizeof(s_model.message), "Could not complete: %s", esp_err_to_name(rc));
            }
            s_model.busy = false;
            s_model.operation = LS_WIRELESS_NONE;
            last_poll = 0;
        }
        wipe(pass, sizeof(pass));
        uint32_t now = now_ms();
        if (!last_poll || now - last_poll >= 1000) {
            poll_radios();
            publish();
            last_poll = now_ms();
        }
        vTaskDelay(pdMS_TO_TICKS(125));
    }
}

static void start_worker(void)
{
    portENTER_CRITICAL(&s_mux);
    bool start = !s_worker;
    if (start) s_worker = true;
    portEXIT_CRITICAL(&s_mux);
    if (start) {
        TaskHandle_t task = ls_flash_task_create_static(worker, "wireless_ui",
            sizeof(s_worker_stack), NULL, 3, s_worker_stack, &s_worker_tcb,
            tskNO_AFFINITY);
        portENTER_CRITICAL(&s_mux);
        s_worker_task = task;
        if (!task) {
            s_worker = false;
            s_pending = LS_WIRELESS_NONE;
            s_public.busy = false;
            wipe(s_pass, sizeof(s_pass));
            snprintf(s_public.message, sizeof(s_public.message), "Wireless worker unavailable");
        }
        portEXIT_CRITICAL(&s_mux);
    }
    portENTER_CRITICAL(&s_mux);
    TaskHandle_t task = s_worker_task;
    portEXIT_CRITICAL(&s_mux);
    if (task) xTaskNotifyGive(task);
}

void ls_wireless_set_active(bool active)
{
    portENTER_CRITICAL(&s_mux);
    s_active = active;
    portEXIT_CRITICAL(&s_mux);
    if (active) start_worker();
}

void ls_wireless_get(ls_wireless_snapshot_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_mux);
    *out = s_public;
    portEXIT_CRITICAL(&s_mux);
    out->now_ms = now_ms();
}

bool ls_wireless_request(ls_wireless_op_t op, const char *ssid, const char *pass)
{
    if (op <= LS_WIRELESS_NONE || op > LS_WIRELESS_BT_RESCAN ||
        (ssid && strlen(ssid) > 32) || (pass && strlen(pass) > 64)) return false;
    portENTER_CRITICAL(&s_mux);
    bool accept = s_pending == LS_WIRELESS_NONE && !s_public.busy;
    if (accept) {
        s_pending = op;
        snprintf(s_ssid, sizeof(s_ssid), "%s", ssid ? ssid : "");
        snprintf(s_pass, sizeof(s_pass), "%s", pass ? pass : "");
        s_public.busy = true;
        s_public.operation = op;
    }
    portEXIT_CRITICAL(&s_mux);
    if (accept) start_worker();
    return accept;
}
