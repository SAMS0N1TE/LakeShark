/* Runs on the existing wireless worker. Never takes another app's radio. */
#include "tui/ls_sweep_app.h"
#include "tui/ls_detector.h"
#pragma weak ls_detector_running
#pragma weak ls_detector_status
#include "ls_wifi.h"
#include "ls_gps.h"
#include <math.h>
#pragma weak ls_gps_get
#include "ble_link.h"
#include "audio_out.h"
#include "tone.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include <stdio.h>
#include <inttypes.h>
#ifndef LS_SWEEP_DIR
#define LS_SWEEP_DIR "/sdcard/sweep"
#endif
extern void ls_wireless_wake(void);
static bool started,sd_loaded,cache_busy,ble_failed;
static int64_t last_scan, motion_us, consumed_scan, retry_us;
static EXT_RAM_BSS_ATTR ls_wifi_scan_ap_t aps[64];
static EXT_RAM_BSS_ATTR ls_sweep_device_t event;
void ls_sweep_wake(void) {ls_wireless_wake();}
void ls_sweep_tick(void) {
    ls_sweep_status_t s;int64_t now=esp_timer_get_time();ls_sweep_status(&s,now);
    ls_detector_status_t detector={0};
    if(ls_detector_status) ls_detector_status(&detector);
    bool need_ble=s.requested || (detector.running && (detector.sources&(DET_RID|DET_BLE)));
    s.requested |= detector.running && detector.sources;
    if(ls_sweep_reload_pending()) {
        bool ok=ls_sweep_rules_reload();ls_sweep_runtime_status(started,ok?"SD rules reloaded":"Rules rejected / previous rules retained");
    }
    if(s.requested && !started) {
        if(now<retry_us) return;
        if(ble_link_sweep_observer(need_ble)!=ESP_OK) {
            retry_us=now+5000000;
            ls_sweep_runtime_status(false,"BLE start failed / retrying");return;
        }
        /* BLE remains in discovery for the whole SWEEP session. Do not cycle
         * the shared C6 Wi-Fi mode/start/stop every five seconds alongside it.
         * Consume only records supplied by explicit Wi-Fi operations. */
        if(retry_us) {retry_us=0;ls_sweep_runtime_status(false,"BLE recovered");}
        started=true;consumed_scan=0;cache_busy=false;last_scan=now-5000000;
        ls_sweep_runtime_status(false,NULL);
        if(!sd_loaded) {ls_sweep_rules_seed();
            bool ok=ls_sweep_rules_reload();sd_loaded=true;
            ls_sweep_runtime_status(false,ok?"SD rules loaded":"SD rules unavailable/rejected / defaults retained");}
    }
    if(!s.requested && started) {
        ble_link_sweep_observer(false);retry_us=0;
        started=false;ls_sweep_runtime_status(false,NULL);return;
    }
    if(!started) {motion_us=0;retry_us=0;return;}
    ls_sweep_transport_stats_t rx;
    bool available=false;
    if(now>=retry_us) {
        available=ble_link_sweep_observer(need_ble)==ESP_OK;
        retry_us=available?0:now+5000000;
    }
    ls_sweep_transport_stats(&rx);
    if(!need_ble) rx.scanning=false;
    if(!available) {ble_failed=true;rx.scanning=false;ls_sweep_runtime_status(false,"BLE unavailable / retrying");}
    else {
        ls_sweep_runtime_status(rx.scanning,ble_failed && rx.scanning?"BLE reception restored":NULL);
        if(rx.scanning) ble_failed=false;
    }
    if(ls_gps_get) {
        ls_gps_state_t g;ls_gps_get(&g);
        bool fix=g.fix && now-g.last_fix_us<5000000;
        float dt=motion_us && now>motion_us?(now-motion_us)/1e6f:0;
        /* Ignore gaps and stationary jitter; speed is receiver-derived. */
        float moved=fix && dt<=2 && isfinite(g.speed_kts) && g.speed_kts>=1?g.speed_kts*.514444f*dt:0;
        ls_sweep_motion(fix,moved,0,now);motion_us=now;
    }
    if(now-last_scan>=5000000) {
        last_scan=now;int64_t seen_us=now;
        int n=ls_wifi_scan_cached(aps,64,&seen_us);
        if(n<0) {
            /* A busy operation gate must not cause continual retries. */
            last_scan=now+55000000;
            cache_busy=true;ls_sweep_runtime_status(rx.scanning,"Wi-Fi cache busy / retry in 60s");
        }
        else {
            if(seen_us>consumed_scan) {
                for(int i=0;i<n;i++) ls_sweep_wifi(aps[i].bssid,aps[i].ssid,aps[i].rssi,seen_us);
                consumed_scan=seen_us;
            }
            if(cache_busy) {cache_busy=false;ls_sweep_runtime_status(rx.scanning,"Wi-Fi cache available / no scan");}
        }
    }
    now=esp_timer_get_time();
    /* Consume alerts even while muted: unmuting never plays a stale backlog. */
    if(!snd_test_busy()) {int c=ls_sweep_alert(now);if(c>=0 && !audio_is_muted() && audio_volume_get()>0) {ls_sweep_settings_t o;ls_sweep_settings_get(&o);if(o.hunt) snd_sweep_click();else snd_sweep_start(c);}}
    while(ls_sweep_log_event(&event)) {
        FILE *f=fopen(LS_SWEEP_DIR "/alerts.csv","a");
        if(!f) {ls_sweep_runtime_status(rx.scanning,"Alert log unavailable");break;}
        char address[18];ls_sweep_mac_text(address,&event);
        /* No payload, name, GPS fix or Remote ID/operator coordinates. */
        int written=fprintf(f,"%" PRId64 ",%s,%u,%s,%d\n",event.seen_us,ls_sweep_category(event.match.category),event.radio,address,event.rssi);
        int closed=fclose(f);
        ls_sweep_runtime_status(rx.scanning,written<0 || closed?"Alert log write failed":"Alert logged to SD");
    }
}
