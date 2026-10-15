#include "ls_test.h"
#include "ls_detector.h"
#include "ls_sweep_app.h"
#include "ls_wifi.h"
#include "ls_gps.h"
#include "apps/adsb/adsb_app.h"
#include "ble_link.h"
#include "esp_timer.h"
#include <string.h>
static bool owned,fail,external;
static unsigned sounds,deliveries,cache_reads;
void ls_wireless_wake(void) {}
esp_err_t ble_link_sweep_observer(bool on) {if(on && fail)return ESP_FAIL;owned=on;return ESP_OK;}
void ls_sweep_transport_stats(ls_sweep_transport_stats_t *out) {memset(out,0,sizeof(*out));out->scanning=owned || external;}
void ls_gps_get(ls_gps_state_t *out) {memset(out,0,sizeof(*out));}
/* No ADS-B receiver runs in this test: aircraft stay "source off". */
adsb_source_t adsb_active_source(void) {return ADSB_SRC_NONE;}
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

/* Runs first (cases register last-in, first-out): the rules in force before any file is read. */
LS_CASE(detector_builtin_aircraft_rules_default_emergencies_on_everything_else_off) {
    ls_detector_status_t s;ls_detector_status(&s);
    LS_CHECK(s.rules==11 && !strcmp(s.adsb,"OFF") && (s.sources&DET_ADSB));
    struct {const char *id;bool enabled;uint8_t mask;} want[]={
        {"air-emergency-squawk",true,DET_AIR_SQUAWK},{"air-emergency-status",true,DET_AIR_EMERG},
        {"air-nearby",false,DET_AIR_KM},{"air-low",false,DET_AIR_ALT},{"air-category",false,DET_AIR_CAT}};
    for(unsigned i=0;i<5;i++) {
        ls_detector_rule_t r;
        LS_CHECK(ls_detector_rule(6+i,&r) && !strcmp(r.id,want[i].id) && r.enabled==want[i].enabled);
        LS_CHECK(r.sources==DET_ADSB && r.air_mask==want[i].mask);
        /* No watch list is built in: callsigns and addresses come from the SD rules file. */
        LS_CHECK(r.air_calls==0 && r.air_icaos==0);
    }
    ls_detector_rule_t r;
    LS_CHECK(ls_detector_rule(6,&r) && r.air_squawks==3 && r.air_squawk[0]==7500 && r.air_squawk[1]==7600 && r.air_squawk[2]==7700);
    LS_CHECK(ls_detector_rule(8,&r) && r.air_km==10);
    LS_CHECK(ls_detector_rule(9,&r) && r.air_alt_below_ft==1500);
}
