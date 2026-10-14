/* Deterministic observations through production rules; no radio hardware. */
#include "ls_detector.h"
#include "ls_sweep_app.h"
#include "esp_timer.h"
#include <stdlib.h>
#include <string.h>
void ls_detector_banner_poll(void);
void ls_sweep_wake(void) {}
void ls_sweep_transport_stats(ls_sweep_transport_stats_t *s) {memset(s,0,sizeof(*s));}
void lssim_detector_seed(void) {
    if(!getenv("LSSIM_DETECTOR"))return;
    const char *rules="{\"version\":1,\"rules\":[{\"id\":\"nearby\",\"enabled\":true,\"sources\":[\"sweep.ble\"],\"match\":{\"category_in\":[\"CAMERA\",\"BODYCAM\",\"DRONE\",\"TRACKER\",\"ATTACK\"]},\"sinks\":[\"banner\"],\"cooldown_s\":0,\"rule_cooldown_s\":0}]}";
    ls_detector_init();ls_detector_tick();
    ls_detector_parse(rules,strlen(rules));ls_detector_start(true);
    ls_detector_policy(DET_BLE,0);
    if(!getenv("LSSIM_DETECTOR_IDLE"))for(int i=0;i<18;i++) {
        ls_shim_time_set(100000000LL+i*3000000LL); uint8_t mac[6]={1,2,3,4,5,(uint8_t)i};
        ls_detector_observe(mac,0,DET_BLE,i%5,-90+i*3,false,false,SW_NEW,esp_timer_get_time());
        ls_detector_tick();
    }
    ls_detector_policy(DET_BLE,DET_BANNER|DET_VIBRATE);
    if(getenv("LSSIM_DETECTOR_TEST")){ls_detector_request(true);ls_detector_tick();}
    if(getenv("LSSIM_DETECTOR_BANNER"))ls_detector_banner_poll();
}


void lssim_detector_work(void) {
    if(getenv("LSSIM_DETECTOR"))ls_detector_tick();
}
