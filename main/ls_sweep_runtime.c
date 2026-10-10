/* Runs on the existing wireless worker. Never takes another app's radio. */
#include "tui/ls_sweep_app.h"
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
static bool ble_owned,started,sd_loaded;
static int64_t last_scan, motion_us;
static EXT_RAM_BSS_ATTR ls_wifi_scan_ap_t aps[64];
static EXT_RAM_BSS_ATTR ls_sweep_device_t event;
void ls_sweep_wake(void) {ls_wireless_wake();}
void ls_sweep_tick(void) {
    ls_sweep_status_t s;int64_t now=esp_timer_get_time();ls_sweep_status(&s,now);
    if(ls_sweep_reload_pending()) {
        bool ok=ls_sweep_rules_reload();ls_sweep_runtime_status(started,ok?"SD rules reloaded":"Rules rejected / previous rules retained");
    }
    if(s.requested && !started) {
        if(!sd_loaded) {ls_sweep_rules_seed();ls_sweep_rules_reload();sd_loaded=true;}
        /* Existing LINK/SURVEY adverts arrive through the observer worker.
         * Acquire a passive scanner only when nobody already runs BLE. */
        bool ble_available=ble_link_state()!=BLE_LINK_OFF;
        if(!ble_available) {
            ble_owned=ble_link_listen()==ESP_OK;
            /* Another owner may have won the start race; borrow its feed. */
            ble_available=ble_owned || ble_link_state()!=BLE_LINK_OFF;
        }
        if(!ble_available) {
            ls_sweep_runtime_status(false,"Passive radio unavailable / start failed");ls_sweep_start(false);return;
        }
        /* BLE remains in discovery for the whole SWEEP session. Do not cycle
         * the shared C6 Wi-Fi mode/start/stop every five seconds alongside it.
         * Consume only records supplied by explicit Wi-Fi operations. */
        started=true;last_scan=now-5000000;ls_sweep_runtime_status(true,"Shared BLE feed / cached Wi-Fi only");
    }
    if(!s.requested && started) {
        if(ble_owned) {ble_link_stop();ble_owned=false;}
        started=false;ls_sweep_runtime_status(false,"Stopped / live history cleared");return;
    }
    if(!started) {motion_us=0;return;}
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
            ls_sweep_runtime_status(true,"Shared BLE / Wi-Fi cache busy; retry in 60s");
        }
        else {for(int i=0;i<n;i++) ls_sweep_wifi(aps[i].bssid,aps[i].ssid,aps[i].rssi,seen_us);ls_sweep_runtime_status(true,"Shared BLE / cached Wi-Fi only (no survey scans)");}
    }
    now=esp_timer_get_time();
    /* Consume alerts even while muted: unmuting never plays a stale backlog. */
    if(!snd_test_busy()) {int c=ls_sweep_alert(now);if(c>=0 && !audio_is_muted() && audio_volume_get()>0) {ls_sweep_settings_t o;ls_sweep_settings_get(&o);if(o.hunt) snd_sweep_click();else snd_sweep_start(c);}}
    while(ls_sweep_log_event(&event)) {
        FILE *f=fopen(LS_SWEEP_DIR "/alerts.csv","a");
        if(!f) {ls_sweep_runtime_status(true,"RX active / alert log unavailable");break;}
        const uint8_t *p=event.mac;
        /* No payload, name, GPS fix or Remote ID/operator coordinates. */
        fprintf(f,"%" PRId64 ",%s,%u,%02X%02X%02X%02X%02X%02X,%d\n",event.seen_us,ls_sweep_category(event.match.category),event.radio,p[0],p[1],p[2],p[3],p[4],p[5],event.rssi);
        if(fclose(f)) ls_sweep_runtime_status(true,"RX active / alert log write failed");
    }
}
