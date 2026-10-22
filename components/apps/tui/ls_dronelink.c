/* Receive-only ELRS-compatible LoRa activity, never positive drone identity.
 * Public ELRS 3.5.3 parameters (not copied protocol code):
 * https://github.com/ExpressLRS/ExpressLRS/blob/3.5.3/src/lib/FHSS/FHSS.cpp
 * https://github.com/ExpressLRS/ExpressLRS/blob/3.5.3/src/src/common.cpp
 * https://github.com/ExpressLRS/ExpressLRS/blob/3.5.3/src/lib/SX127xDriver/SX127xRegs.h
 * FCC915: 40 channels 903.5..926.9 MHz; EU868: 13, 865.275..869.575.
 * SF6..9/BW500, sync 0x12. Target FHSS seed is unknown; scan all channels.
 * Preamble/header activity also matches other LoRa users. No ELRS payload
 * authentication is claimed. Fixed 8/13-byte implicit RX, CRC handled by ELRS
 * itself (not checked here). Rate is a profile guess, not a measured rate.
 * 125ms worker polls count latched detections (lower bound), not all packets. */
#include "ls_dronelink.h"
#include "ls_drone_watch.h"
#ifndef ESP_PLATFORM
#pragma weak ls_drone_watch_lora900
#endif
#include "ls_detector.h"
#include "board/ls_lora.h"
#include "ls_mesh.h"
#include <string.h>
#include "esp_attr.h"
#include <math.h>
static bool held, saved_rx;
static ls_lora_cfg_t saved;
static unsigned channel, profile;
static bool resume;
static int64_t hop_at, retry_at;
static unsigned state; /* off, scanning, unavailable, conflict */
static EXT_RAM_BSS_ATTR struct { int64_t second; uint16_t hits[6]; } channels[53];
static unsigned load_state(void) {return __atomic_load_n(&state,__ATOMIC_ACQUIRE);}
static void status(unsigned s) {__atomic_store_n(&state,s,__ATOMIC_RELEASE);}
const char *ls_dronelink_status(void) {
    static const char *const names[]={"OFF","SCANNING (LoRa activity)","UNAVAILABLE","CONFLICT"};
    return names[load_state()];
}
bool ls_dronelink_pending(void) {return load_state()==1;}
static bool waiting;            /* hold_bg refused once; still registered */
static volatile bool may_pause; /* DRONES on screen: MeshCore may be paused */
void ls_dronelink_may_pause_mesh(bool on) {may_pause=on;}
static void release(void) {
    if(waiting && !held) {ls_mesh_radio_hold_bg(false);waiting=false;}
    if(!held)return;
    /* Configure parks in standby. Restore before releasing our task's lease. */
    ls_lora_configure(&saved);if(saved_rx)ls_lora_receive();
    held=false;ls_mesh_radio_hold_bg(false);
}
static bool tune(void) {
    ls_lora_cfg_t cfg={0};
    cfg.freq_hz=channel<40?903500000u+channel*600000u:
        865275000u+(uint32_t)((uint64_t)(channel-40)*4300000u/12u);
    static const uint8_t sf[]={6,6,7,8,9};
    cfg.sf=sf[profile];cfg.bw_hz=500000;cfg.cr=profile==1?8:7;cfg.preamble=cfg.sf<8?8:10;
    cfg.implicit_rx_bytes=profile==1?13:8;
    cfg.sync_word=0x12;cfg.power_dbm=0;
    cfg.cal_min_mhz=channel<40?902:863;cfg.cal_max_mhz=channel<40?928:870;
    return ls_lora_configure(&cfg)==ESP_OK && ls_lora_receive()==ESP_OK;
}
void ls_dronelink_suspend(void) {
    if(held) {release();resume=true;retry_at=0;status(0);}
}
void ls_dronelink_tick(bool enabled,int64_t now) {
    if(!enabled) {release();resume=false;status(0);return;}
    /* A running mesh is paused only while DRONES is on screen; a foreground
       owner or another background holder always wins. */
    if(ls_mesh_radio_wanted() || (!may_pause && ls_mesh_running()) ||
       (!held && !waiting && ls_mesh_radio_bg())) {
        release();status(3);return;
    }
    if(!held) {
        if(now<retry_at)return;
        retry_at=now+(waiting?100000:5000000);
        if(!ls_lora_present() || !(ls_lora_caps()&LS_LORA_CAP_LORA)) {status(2);return;}
        /* Refused: stay registered so the mesh task pauses and hands over. */
        if(!ls_mesh_radio_hold_bg(true)) {waiting=true;status(3);return;}
        waiting=false;
        const ls_lora_cfg_t *cfg=ls_lora_cfg();
        if(cfg)saved=*cfg;else ls_lora_cfg_default(&saved);
        saved_rx=ls_lora_is_receiving();held=true;
        if(!resume) {channel=profile=0;memset(channels,0,sizeof(channels));}
        resume=false;
        if(!tune()) {release();status(2);return;}
        hop_at=now+250000;status(1);return;
    }
    float rssi=0;int flags=ls_lora_activity(&rssi);
    if(flags<0) {release();status(2);retry_at=now+5000000;return;}
    if((flags&3) && isfinite(rssi) && rssi<=0 && rssi>=-127) {
        /* Six 10s buckets: observed hits in the last 50..60s, never extrapolated. */
        int64_t second=now/10000000;
        int64_t delta=second-channels[channel].second;
        if(delta>=6 || delta<0)memset(channels[channel].hits,0,sizeof(channels[channel].hits));
        else for(int64_t d=1;d<=delta;d++)channels[channel].hits[(channels[channel].second+d)%6]=0;
        channels[channel].second=second;
        uint16_t *bin=&channels[channel].hits[second%6];if(*bin<65535)(*bin)++;
        unsigned hits=0;for(unsigned b=0;b<6;b++)hits+=channels[channel].hits[b];
        static const unsigned rates[]={200,100,100,50,25};
        ls_detector_drone(channel<40?915:868,channel,rates[profile],(int)rssi,hits,now);
#ifndef ESP_PLATFORM
        if(ls_drone_watch_lora900)
#endif
        ls_drone_watch_lora900(channel<40?915:868,channel<40?channel:channel-40,rssi,now);
    }
    if(now>=hop_at) {
        if(++channel==53) {channel=0;profile=(profile+1)%5;}
        if(!tune()) {release();status(2);retry_at=now+5000000;return;}
        hop_at=now+250000;
    }
}
