#include "tui/ls_wireless.h"
#include "ls_wifi.h"
#include "tui/ls_sweep_app.h"
#include "tui/ls_seen.h"
#include "ble_link.h"
#include "ls_flash_task.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern bool ls_survey_pending(void);
extern void ls_survey_tick(void);

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_active, s_worker, s_observer;
static bool s_find_ble_requested, s_find_ble_owned;
static bool s_rid_requested, s_rid_owned, s_rid_failed, s_rid_retry;
static bool s_drone_rid;
/* Keep DRAM: JOIN/FORGET and survey operations reach NVS/storage. Nano's
 * measured 3100 B unused at 6144 permits 4608, retaining 1564 B for the
 * deeper paths. Reserve it across visits rather than allocating late. */
#if CONFIG_LS_NANO_RAM_BUDGET
#define WIRELESS_STACK_BYTES 4608
#else
#define WIRELESS_STACK_BYTES 6144
#endif
static StackType_t s_worker_stack[WIRELESS_STACK_BYTES / sizeof(StackType_t)]
    __attribute__((aligned(16)));
static StaticTask_t s_worker_tcb;
static TaskHandle_t s_worker_task;
static ls_wireless_op_t s_pending;
static char s_ssid[33], s_pass[65];
static EXT_RAM_BSS_ATTR ls_wireless_snapshot_t s_public, s_model;
static EXT_RAM_BSS_ATTR ls_wifi_scan_ap_t s_scan[LS_WIRELESS_APS];

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
    s_model.wifi_running = ls_wifi_running() || ls_wifi_sta_running();
    bool scanning;
    ble_link_receiver(&s_model.bt_running, &scanning, &s_model.bt_failed);
    s_model.wifi_signal = s_model.wifi_connected && ls_wifi_sta_info(&ap) == ESP_OK;
    s_model.wifi_rssi = ap.rssi;
    s_model.channel = ap.channel;
    snprintf(s_model.ssid, sizeof(s_model.ssid), "%s", ap.ssid);
    ls_wifi_sta_ip(s_model.ip, sizeof(s_model.ip));
    ls_wifi_sta_status(s_model.wifi_status, sizeof(s_model.wifi_status));
    int saved = ls_wifi_saved_list(s_model.saved, 8);
    if (saved >= 0) s_model.saved_count = saved;
    for (int i = 0; i < s_model.ap_count; ++i) {
        s_model.aps[i].saved = false;
        for (int j = 0; j < s_model.saved_count; ++j)
            if (!strcmp(s_model.aps[i].ssid, s_model.saved[j])) s_model.aps[i].saved = true;
    }
    s_model.bt_state = ble_link_state();
    s_model.bt_ready = s_model.bt_state == BLE_LINK_READY;
    snprintf(s_model.bt_status, sizeof(s_model.bt_status), "%s",
             ble_link_state_name_of(s_model.bt_state));
    ble_link_peer(s_model.peer, sizeof(s_model.peer), s_model.address, sizeof(s_model.address));
    ble_link_get_pinned(s_model.pinned, sizeof(s_model.pinned));
    ble_link_get_name_filter(s_model.name_filter, sizeof(s_model.name_filter));
    s_model.tel_hz = ble_link_tel_hz();
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
    case LS_WIRELESS_SAVED: return ssid && *ssid ? ls_wifi_sta_join_saved(ssid) : ls_wifi_sta_autojoin();
    case LS_WIRELESS_LEAVE: return ls_wifi_sta_leave();
    case LS_WIRELESS_FORGET: return ssid && *ssid ? ls_wifi_sta_forget_ssid(ssid) : ESP_ERR_INVALID_ARG;
    case LS_WIRELESS_BT_START: return ble_link_start();
    case LS_WIRELESS_BT_STOP: ble_link_stop(); return ESP_OK;
    case LS_WIRELESS_BT_RESCAN:
        return ble_link_start();
    case LS_WIRELESS_BT_PIN: {
        char address[20];
        ble_link_peer(NULL, 0, address, sizeof(address));
        if (!ssid || !*ssid || strcmp(ssid, address)) return ESP_ERR_NOT_FOUND;
        return ble_link_pin_peer(NULL);
    }
    case LS_WIRELESS_BT_UNPIN: ble_link_unpin_peer(); return ESP_OK;
    case LS_WIRELESS_BT_FILTER: ble_link_set_name_filter(ssid); return ESP_OK;
    case LS_WIRELESS_BT_TELEMETRY: ble_link_set_tel_hz(atoi(ssid)); return ESP_OK;
    default: return ESP_ERR_INVALID_ARG;
    }
}

static void rid_tick(void)
{
    portENTER_CRITICAL(&s_mux);
    bool requested=s_rid_requested || s_drone_rid, owned=s_rid_owned, retry=s_rid_retry, failed=s_rid_failed;
    s_rid_retry=false;
    portEXIT_CRITICAL(&s_mux);
    if(requested && failed && !retry)return;
    if(requested==owned && !retry) return;
    esp_err_t rc=ble_link_rid_observer(requested);
    portENTER_CRITICAL(&s_mux);
    s_rid_owned=requested && rc==ESP_OK;
    s_rid_failed=rc!=ESP_OK;
    if(s_rid_failed) s_rid_requested=false;
    portEXIT_CRITICAL(&s_mux);
}
static void find_ble_tick(bool want)
{
    if (want != s_find_ble_owned && ble_link_find_observer(want) == ESP_OK)
        s_find_ble_owned = want;
}

static void worker(void *arg)
{
    (void)arg;
    uint32_t last_poll = 0;
    for (;;) {
        char ssid[33], pass[65];
        portENTER_CRITICAL(&s_mux);
        ls_wireless_op_t op = s_pending;
        s_pending = LS_WIRELESS_NONE;
        memcpy(ssid, s_ssid, sizeof(ssid));
        memcpy(pass, s_pass, sizeof(pass));
        wipe(s_pass, sizeof(s_pass));
        bool find_ble = s_find_ble_requested;
        bool stop = find_ble == s_find_ble_owned && !s_active && !s_observer && !s_rid_requested && !s_rid_owned && op == LS_WIRELESS_NONE;
        portEXIT_CRITICAL(&s_mux);
        find_ble_tick(find_ble);
        if (stop && !ls_survey_pending() && !ls_sweep_pending() && !ls_seen_pending()) {
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
                if (op == LS_WIRELESS_JOIN || op == LS_WIRELESS_SAVED) done = "";
                if (op == LS_WIRELESS_FORGET) done = "Saved network removed";
                snprintf(s_model.message, sizeof(s_model.message), "%s", done);
            } else {
                snprintf(s_model.message, sizeof(s_model.message), "%s",
                    rc == ESP_ERR_NO_MEM ? "Not enough memory; stop other work and retry" :
                    rc == ESP_ERR_NOT_FOUND ? "Selected target is no longer available" :
                    rc == ESP_ERR_INVALID_STATE ? "Radio busy; stop other work and retry" :
                    "Connection update failed; retry");
            }
            s_model.busy = false;
            s_model.operation = op;
            last_poll = 0;
        }
        wipe(pass, sizeof(pass));
        uint32_t now = now_ms();
        if (!last_poll || now - last_poll >= 1000) {
            poll_radios();
            publish();
            last_poll = now_ms();
        }
        rid_tick();
        ls_survey_tick();
        ls_sweep_tick();
        ls_seen_tick();
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

void ls_wireless_wake(void) { start_worker(); }

void ls_wireless_set_active(bool active)
{
    portENTER_CRITICAL(&s_mux);
    s_active = active;
    portEXIT_CRITICAL(&s_mux);
    if (active) start_worker();
}

void ls_wireless_observe(bool active)
{
    portENTER_CRITICAL(&s_mux);
    s_observer = active;
    portEXIT_CRITICAL(&s_mux);
    if (active) start_worker();
}

void ls_wireless_find_ble(bool active)
{
    portENTER_CRITICAL(&s_mux);
    bool changed = s_find_ble_requested != active;
    s_find_ble_requested = active;
    portEXIT_CRITICAL(&s_mux);
    if (changed || active) start_worker();
}

void ls_wireless_get(ls_wireless_snapshot_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_mux);
    *out = s_public;
    portEXIT_CRITICAL(&s_mux);
#if CONFIG_LS_C6_LINK
    out->wifi_available = true;
#endif
#if CONFIG_LS_BLE_HEAD
    out->bt_available = true;
#endif
    out->now_ms = now_ms();
}

bool ls_wireless_request(ls_wireless_op_t op, const char *ssid, const char *pass)
{
    if (op <= LS_WIRELESS_NONE || op > LS_WIRELESS_BT_TELEMETRY ||
        (ssid && strlen(ssid) > 32) || (pass && strlen(pass) > 64)) return false;
    if (ls_survey_pending()) return false;
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

void ls_wireless_rid_request(bool on)
{
    portENTER_CRITICAL(&s_mux);
    s_rid_requested=on; s_rid_failed=false; s_rid_retry=on;
    portEXIT_CRITICAL(&s_mux);
    start_worker();
    portENTER_CRITICAL(&s_mux);
    if(!s_worker_task && on) {s_rid_requested=false;s_rid_failed=true;}
    portEXIT_CRITICAL(&s_mux);
}
void ls_wireless_drone_rid(bool on)
{
    portENTER_CRITICAL(&s_mux);
    bool changed=s_drone_rid!=on;
    s_drone_rid=on;
    if(changed) {s_rid_failed=false;s_rid_retry=true;}
    portEXIT_CRITICAL(&s_mux);
    if(changed || on)start_worker();
}
ls_rid_rx_t ls_wireless_rid_status(void)
{
#if !CONFIG_LS_BLE_HEAD
    return LS_RID_RX_UNAVAILABLE;
#else
    portENTER_CRITICAL(&s_mux);
    bool starting=(s_rid_requested || s_drone_rid) && !s_rid_owned, failed=s_rid_failed;
    portEXIT_CRITICAL(&s_mux);
    bool running,scanning,scan_failed; ble_link_receiver(&running,&scanning,&scan_failed);
    if(scanning) return LS_RID_RX_LISTENING;
    if(starting) return LS_RID_RX_STARTING;
    if(failed || scan_failed) return LS_RID_RX_FAILED;
    if(!running) return LS_RID_RX_OFF;
    if(ble_link_state()==BLE_LINK_SYNCING) return LS_RID_RX_STARTING;
    return LS_RID_RX_PAUSED;
#endif
}
