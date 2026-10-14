#include "ls_test.h"
#include "ls_detector.h"
#include "ls_sweep_app.h"
#include "ls_wifi.h"
#include "ls_gps.h"
#include "ble_link.h"
#include "esp_timer.h"
#include <string.h>
static bool owned,fail,external;
static unsigned sounds,deliveries,cache_reads;
void ls_wireless_wake(void) {}
esp_err_t ble_link_sweep_observer(bool on) {if(on && fail)return ESP_FAIL;owned=on;return ESP_OK;}
void ls_sweep_transport_stats(ls_sweep_transport_stats_t *out) {memset(out,0,sizeof(*out));out->scanning=owned || external;}
void ls_gps_get(ls_gps_state_t *out) {memset(out,0,sizeof(*out));}
int ls_wifi_scan_cached(ls_wifi_scan_ap_t *out,int cap,int64_t *at) {
    LS_CHECK(cap>0);cache_reads++;*at=esp_timer_get_time();
    *out=(ls_wifi_scan_ap_t){.bssid={0,0x40,0x8c,1,2,3},.rssi=-55};return 1;
}
bool snd_test_busy(void) {return false;}
bool audio_is_muted(void) {return false;}
int audio_volume_get(void) {return 50;}
bool snd_sweep_click(void) {sounds++;return true;}
bool snd_sweep_start(int category) {(void)category;sounds++;return true;}
uint8_t ls_detector_deliver(const ls_detector_event_t *e,uint8_t mask) {(void)e;deliveries++;return mask;}
void ls_detector_log_begin(void) {}
bool ls_detector_log_end(void) {return true;}
bool ls_detector_write(const ls_detector_event_t *e,uint64_t boot) {(void)e;(void)boot;return true;}
LS_CASE(detector_worker_shared_ownership_cache_only_and_stop) {
    ls_shim_time_set(10000000);ls_sweep_init();ls_sweep_start(false);ls_detector_start(true);
    ls_detector_tick();ls_sweep_tick();LS_CHECK(owned && cache_reads==1);
    ls_detector_tick();LS_CHECK(deliveries==1 && sounds==0);
    ls_detector_start(false);ls_sweep_tick();LS_CHECK(!owned);
    ls_sweep_start(true);ls_sweep_tick();LS_CHECK(owned);
    ls_detector_start(true);ls_detector_tick();ls_sweep_tick();ls_detector_start(false);ls_sweep_tick();LS_CHECK(owned);
    ls_sweep_start(false);ls_sweep_tick();LS_CHECK(!owned);
    ls_detector_start(true);ls_detector_policy(DET_WIFI,DET_BANNER);ls_sweep_tick();LS_CHECK(!owned);
    ls_shim_time_set(20000000);ls_sweep_tick();ls_detector_tick();LS_CHECK(!sounds);
    ls_detector_policy(DET_RID,DET_BANNER);fail=true;ls_sweep_tick();ls_sweep_status_t s;ls_sweep_status(&s,20000000);LS_CHECK(!s.running);
    fail=false;ls_shim_time_set(26000000);ls_sweep_tick();LS_CHECK(owned);
    ls_detector_start(false);ls_sweep_tick();LS_CHECK(!owned && !ls_detector_pending());
}
