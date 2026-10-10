#include "ls_test.h"
#include "../../main/ls_wireless.c"

static int starts, scans, leaves, rid_starts, rid_stops;
static bool running, scanning, scan_failed;
static esp_err_t rid_result;
static char saved_join[33];
bool ls_survey_pending(void) { return false; }
void ls_survey_tick(void) {}
bool ls_sweep_pending(void) { return false; }
void ls_sweep_tick(void) {}
TaskHandle_t ls_flash_task_create_static(TaskFunction_t fn, const char *name,
    uint32_t bytes, void *arg, UBaseType_t priority, StackType_t *stack,
    StaticTask_t *tcb, BaseType_t core) { starts++; return (TaskHandle_t)1; }
bool ls_wifi_sta_connected(void) { return false; }
esp_err_t ls_wifi_sta_info(ls_wifi_scan_ap_t *ap) { return ESP_FAIL; }
void ls_wifi_sta_ip(char *out, int cap) { if (cap) out[0] = 0; }
void ls_wifi_sta_status(char *out, int cap) { snprintf(out, cap, "Disconnected"); }
int ls_wifi_saved_list(char names[][33], int cap) { return 0; }
int ls_wifi_sta_scan(ls_wifi_scan_ap_t *ap, int cap) { return 0; }
esp_err_t ls_wifi_sta_join(const char *ssid, const char *pass) { return ESP_OK; }
esp_err_t ls_wifi_sta_join_saved(const char *ssid) { snprintf(saved_join, 33, "%s", ssid); return ESP_OK; }
esp_err_t ls_wifi_sta_autojoin(void) { saved_join[0] = 0; return ESP_OK; }
esp_err_t ls_wifi_sta_leave(void) { return ESP_OK; }
esp_err_t ls_wifi_sta_forget_ssid(const char *ssid) { return ESP_OK; }
ble_link_state_t ble_link_state(void) { return BLE_LINK_SCANNING; }
const char *ble_link_state_name_of(ble_link_state_t state) { return "Scanning"; }
esp_err_t ble_link_start(void) { scans++; return ESP_OK; }
void ble_link_stop(void) { leaves++; }
void ble_link_peer(char *name, size_t nl, char *addr, size_t al) { if(nl) *name=0; if(al) *addr=0; }
void ble_link_stats(uint32_t *rx, uint32_t *tx, uint32_t *drop) {}
esp_err_t ble_link_rssi(int *rssi) { return ESP_FAIL; }
bool ble_link_stock_head_seen(void) { return false; }
bool ble_link_get_pinned(char *out, size_t n) { *out=0; return false; }
void ble_link_get_name_filter(char *out, size_t n) { *out=0; }
int ble_link_tel_hz(void) { return 5; }
esp_err_t ble_link_pin_peer(const char *addr) { return ESP_OK; }
void ble_link_unpin_peer(void) {}
void ble_link_set_name_filter(const char *name) {}
void ble_link_set_tel_hz(int hz) {}
LS_CASE(cold_snapshot_reports_fitted_radios_without_starting_worker) {
    static ls_wireless_snapshot_t out;
    ls_wireless_get(&out);
    LS_CHECK(out.wifi_available && out.bt_available);
    LS_EQ_INT(starts, 0);
    LS_EQ_INT(out.bt_state, BLE_LINK_OFF);
}
LS_CASE(scan_acquires_head_intent_even_when_observer_is_scanning) {
    LS_EQ_INT(perform(LS_WIRELESS_BT_RESCAN, "", ""), ESP_OK);
    LS_EQ_INT(scans, 1);
    LS_EQ_INT(perform(LS_WIRELESS_BT_STOP, "", ""), ESP_OK);
    LS_EQ_INT(leaves, 1);
}
LS_CASE(saved_join_and_forget_have_explicit_targets) {
    LS_EQ_INT(perform(LS_WIRELESS_SAVED, "weaker AP", ""), ESP_OK);
    LS_CHECK(!strcmp(saved_join, "weaker AP"));
    LS_EQ_INT(perform(LS_WIRELESS_FORGET, "", ""), ESP_ERR_INVALID_ARG);
}

esp_err_t ble_link_rid_observer(bool on) { if(on) rid_starts++;else rid_stops++;return rid_result; }
void ble_link_receiver(bool *run,bool *scan,bool *failed) { *run=running;*scan=scanning;*failed=scan_failed; }
LS_CASE(drones_receiver_request_runs_on_worker_and_reports_real_state) {
    LS_EQ_INT(ls_wireless_rid_status(),LS_RID_RX_OFF);
    int initial_starts=starts, initial_leaves=leaves;
    int before=rid_starts;ls_wireless_rid_request(true);LS_EQ_INT(rid_starts,before);
    LS_EQ_INT(ls_wireless_rid_status(),LS_RID_RX_STARTING);
    rid_tick();LS_EQ_INT(rid_starts,before+1);LS_CHECK(s_rid_owned);
    ls_wireless_rid_request(true);rid_tick();LS_EQ_INT(rid_starts,before+2);
    running=true;LS_EQ_INT(ls_wireless_rid_status(),LS_RID_RX_PAUSED);
    scan_failed=true;LS_EQ_INT(ls_wireless_rid_status(),LS_RID_RX_FAILED);scan_failed=false;
    scanning=true;LS_EQ_INT(ls_wireless_rid_status(),LS_RID_RX_LISTENING);
    ls_wireless_rid_request(false);rid_tick();LS_EQ_INT(rid_stops,1);
    LS_EQ_INT(leaves,initial_leaves);LS_EQ_INT(ls_wireless_rid_status(),LS_RID_RX_LISTENING);
    running=scanning=false;rid_result=ESP_FAIL;
    ls_wireless_rid_request(true);rid_tick();LS_EQ_INT(ls_wireless_rid_status(),LS_RID_RX_FAILED);
    LS_CHECK(!s_rid_owned && !s_rid_requested);
    rid_result=ESP_OK;ls_wireless_rid_request(true);rid_tick();LS_CHECK(s_rid_owned);
    ls_wireless_rid_request(false);rid_tick();
    starts=initial_starts;s_worker=false;s_worker_task=NULL;
}

static int find_starts, find_stops;
static esp_err_t find_result;
esp_err_t ble_link_find_observer(bool on) {
    if (on) find_starts++; else find_stops++;
    return find_result;
}
LS_CASE(compass_observer_reconciles_on_worker_and_retries_without_head_rescan) {
    int initial_starts=starts, initial_scans=scans, initial_leaves=leaves;
    ls_wireless_find_ble(true);
    LS_CHECK(s_find_ble_requested && !s_find_ble_owned);
    LS_EQ_INT(find_starts, 0);
    find_result=ESP_FAIL;find_ble_tick(s_find_ble_requested);
    LS_CHECK(!s_find_ble_owned);
    find_result=ESP_OK;find_ble_tick(s_find_ble_requested);
    LS_CHECK(s_find_ble_owned);LS_EQ_INT(find_starts, 2);
    find_ble_tick(true);LS_EQ_INT(find_starts, 2);
    ls_wireless_find_ble(false);LS_CHECK(s_find_ble_owned);
    find_ble_tick(s_find_ble_requested);LS_CHECK(!s_find_ble_owned);
    LS_EQ_INT(find_stops, 1);LS_EQ_INT(scans, initial_scans);LS_EQ_INT(leaves, initial_leaves);
    starts=initial_starts;s_worker=false;s_worker_task=NULL;
}
