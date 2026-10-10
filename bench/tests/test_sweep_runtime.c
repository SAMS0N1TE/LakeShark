#include "ls_test.h"
#include "ls_sweep_app.h"
#include "ls_wifi.h"
#include "ls_gps.h"
#include "ble_link.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
static bool external_wifi,external_ble,owned_wifi,owned_ble,ble_fail,wifi_fail,muted,busy;
static int scans,cached,starts,stops,sounds,volume=23;
static ls_gps_state_t gps;
void ls_gps_get(ls_gps_state_t *out) {*out=gps;}
void ls_wireless_wake(void) {}
bool ls_wifi_sta_running(void) {return external_wifi;}
bool ls_wifi_running(void) {return false;}
esp_err_t ls_wifi_survey_mode(bool on) {if(on && wifi_fail) return ESP_ERR_INVALID_STATE;owned_wifi=on;return ESP_OK;}
ble_link_state_t ble_link_state(void) {return external_ble || owned_ble?BLE_LINK_SCANNING:BLE_LINK_OFF;}
esp_err_t ble_link_listen(void) {starts++;if(ble_fail) return ESP_FAIL;owned_ble=true;return ESP_OK;}
void ble_link_stop(void) {stops++;owned_ble=false;}
int ls_wifi_survey_scan(ls_wifi_scan_ap_t *out,int cap) {
    LS_CHECK(!external_wifi && cap>=1);if(!owned_wifi) return -1;
    scans++;*out=(ls_wifi_scan_ap_t){.bssid={0,0x40,0x8c,1,2,3},.rssi=-50};return 1;
}
int ls_wifi_scan_cached(ls_wifi_scan_ap_t *out,int cap,int64_t *us) {
    LS_CHECK(external_wifi && cap>=1);cached++;*us=esp_timer_get_time();
    *out=(ls_wifi_scan_ap_t){.bssid={0,0x40,0x8c,1,2,3},.rssi=-50};return 1;
}
bool audio_is_muted(void) {return muted;}
int audio_volume_get(void) {return volume;}
bool snd_test_busy(void) {return busy;}
bool snd_sweep_click(void) {LS_CHECK(!muted && volume==23);sounds++;return true;}
bool snd_sweep_start(int category) {LS_CHECK(!muted && volume==23 && category<SW_CATS);sounds++;return true;}
LS_CASE(runtime_ownership_rollback_scan_rate_and_global_mute) {
    scans=cached=starts=stops=sounds=0;volume=23;
    external_wifi=external_ble=owned_wifi=owned_ble=ble_fail=wifi_fail=muted=busy=false;
    ls_sweep_init();ls_sweep_clear();
    ls_sweep_settings_t settings={.version=1,.enabled=15,.receive_only=true};ls_sweep_settings_set(&settings);
    ls_shim_time_set(1000000);
    external_wifi=true;external_ble=true;muted=true;
    ls_sweep_start(true);ls_sweep_tick();LS_CHECK(!owned_wifi && !owned_ble && starts==0 && cached==1 && scans==0);
    ls_sweep_status_t status;ls_sweep_status(&status,esp_timer_get_time());LS_CHECK(status.requested);
    ls_sweep_start(false);ls_sweep_tick();LS_CHECK(stops==0 && external_ble && external_wifi);
    external_wifi=false;ls_sweep_start(true);ls_sweep_tick();LS_CHECK(owned_wifi && !owned_ble && starts==0 && scans==1);
    ls_sweep_start(false);ls_sweep_tick();LS_CHECK(!owned_wifi && stops==0);
    wifi_fail=true;ls_sweep_start(true);ls_sweep_tick();
    ls_sweep_status(&status,esp_timer_get_time());LS_CHECK(status.running && !owned_wifi && !owned_ble);
    ls_sweep_start(false);ls_sweep_tick();LS_CHECK(stops==0);
    wifi_fail=false;
    external_ble=false;ble_fail=true;ls_sweep_start(true);ls_sweep_tick();LS_CHECK(!owned_wifi && !owned_ble && starts==1 && stops==0);
    ble_fail=false;wifi_fail=true;external_wifi=true;
    ls_sweep_start(true);ls_sweep_tick();LS_CHECK(owned_ble && !owned_wifi && cached==2);
    ls_sweep_start(false);ls_sweep_tick();LS_CHECK(stops==1 && external_wifi);
    ls_sweep_clear();
    external_wifi=false;wifi_fail=false;muted=true;ls_sweep_start(true);ls_sweep_tick();LS_CHECK(owned_wifi && owned_ble && scans==2 && sounds==0);
    ls_shim_time_set(2000000);muted=false;ls_sweep_tick();LS_CHECK(scans==2 && sounds==0); /* no mute backlog */
    ls_shim_time_set(6000000);ls_sweep_tick();LS_CHECK(scans==3 && sounds==0);
    ls_shim_time_set(32000000);ls_sweep_tick();LS_CHECK(scans==4 && sounds==1 && volume==23);
    ls_sweep_start(false);ls_sweep_tick();LS_CHECK(!owned_wifi && !owned_ble && stops==2 && !ls_sweep_pending());
}
LS_CASE(logging_is_opt_in_and_rules_reload_retains_previous_on_error) {
    ls_sweep_defaults();ls_sweep_clear();ls_sweep_start(true);
    ls_sweep_settings_t s;ls_sweep_settings_get(&s);s.logging=false;ls_sweep_settings_set(&s);
    uint8_t mac[6]={0,0x40,0x8c,1,2,3};ls_sweep_wifi(mac,"no location",-50,1000000);
    ls_sweep_device_t e;LS_CHECK(!ls_sweep_log_event(&e));
    s.logging=true;s.muted=1<<SW_CAMERA;ls_sweep_settings_set(&s);ls_sweep_wifi(mac,"no location",-50,32000000);
    LS_CHECK(ls_sweep_log_event(&e) && e.match.category==SW_CAMERA && e.seen_us==32000000);LS_CHECK(!ls_sweep_log_event(&e));
    ls_test_mkdir(LS_SWEEP_DIR);FILE *f=fopen(LS_SWEEP_DIR "/rules.json","wb");LS_CHECK(f);if(f) {fputs("invalid",f);fclose(f);}
    LS_CHECK(!ls_sweep_rules_reload());ls_sweep_match_t m;LS_CHECK(ls_sweep_parse_wifi(mac,"",&m));
    ls_sweep_start(false);
}

LS_CASE(sweep_mute_preserves_system_volume_and_receive_runtime) {
    ls_sweep_clear();ls_sweep_defaults();external_wifi=external_ble=true;muted=false;busy=false;volume=23;
    ls_sweep_settings_t s={.version=2,.enabled=15,.muted=2,.receive_only=true};LS_CHECK(ls_sweep_settings_set(&s));
    char *on[]={"sweep","mute","on"},*off[]={"sweep","mute","off"};
    ls_shim_time_set(60000000);ls_sweep_start(true);LS_EQ_INT(ls_sweep_command(3,on),0);
    int before=sounds;ls_sweep_tick();LS_CHECK(sounds==before && volume==23 && !muted);
    ls_sweep_status_t status;ls_sweep_status(&status,esp_timer_get_time());LS_CHECK(status.requested && status.running);
    ls_sweep_device_t device;LS_EQ_UINT(ls_sweep_snapshot(&device,1,esp_timer_get_time()),1);
    LS_CHECK(ls_sweep_hunt(device.mac,device.radio,device.address_type));ls_sweep_tick();LS_CHECK(sounds==before);
    LS_EQ_INT(ls_sweep_command(3,off),0);ls_shim_time_set(61000000);ls_sweep_tick();
    LS_CHECK(sounds==before && volume==23 && !muted);
    ls_shim_time_set(65000000);ls_sweep_tick();LS_CHECK(sounds==before+1 && volume==23);ls_sweep_settings_get(&s);LS_CHECK(s.muted==2 && s.enabled==15);
    ls_sweep_start(false);ls_sweep_tick();
}

LS_CASE(gps_movement_uses_fresh_measured_speed_and_no_stale_intervals) {
    ls_sweep_clear();ls_sweep_defaults();external_wifi=external_ble=true;busy=true;
    ls_sweep_settings_t s={.version=2,.enabled=15,.alerts_muted=true,.receive_only=true};ls_sweep_settings_set(&s);
    uint8_t tracker[6]={99,2,3,4,5,6};ls_sweep_match_t m={.category=SW_TRACKER,.state=SW_STATE_SEPARATED};
    ls_shim_time_set(1000000);ls_sweep_start(true);ls_sweep_tick();
    for(int i=0;i<=600;i++) {
        int64_t now=1000000+i*1000000LL;ls_shim_time_set(now);
        gps.fix=true;gps.last_fix_us=now;gps.speed_kts=2;
        ls_sweep_observe(tracker,0,1,-55,&m,now);ls_sweep_tick();
    }
    static ls_sweep_device_t d[LS_SWEEP_CAP];size_t n=ls_sweep_snapshot(d,LS_SWEEP_CAP,601000000);
    bool found=false;float moved=0;
    for(size_t i=0;i<n;i++) if(!memcmp(d[i].mac,tracker,6)) {found=true;LS_CHECK(d[i].with_you);moved=d[i].moved_m;LS_CHECK(moved>=500 && moved<620);}
    LS_CHECK(found);
    gps.last_fix_us=601000000;ls_shim_time_set(620000000);ls_sweep_tick();
    n=ls_sweep_snapshot(d,LS_SWEEP_CAP,620000000);for(size_t i=0;i<n;i++) if(!memcmp(d[i].mac,tracker,6)) LS_CHECK(d[i].moved_m==moved);
    ls_sweep_start(false);ls_sweep_tick();memset(&gps,0,sizeof(gps));busy=false;
}
