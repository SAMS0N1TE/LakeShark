/* Synthetic, cleanroom advertisements. Host simulator only; no radio/audio. */
#include "ls_sweep_app.h"
#include "esp_timer.h"
#include <stdlib.h>
#include <string.h>
static void u32(uint8_t *p,int32_t v) {for(unsigned i=0;i<4;i++) p[i]=(uint32_t)v>>(i*8);}
void lssim_sweep_seed(void) {
    if(!getenv("LSSIM_SWEEP")) return;
    ls_sweep_start(true);ls_sweep_runtime_status(true,"BLE + Wi-Fi passive / simulated broadcasts");
    if(getenv("LSSIM_SWEEP_LIST")) ls_shim_time_set(2200000000LL);
    int64_t now=esp_timer_get_time();
    if(getenv("LSSIM_SWEEP_QUIET")) return;
    if(getenv("LSSIM_SWEEP_LIST")) {
        ls_sweep_motion(!getenv("LSSIM_SWEEP_NO_GPS"),0,0,now-700000000);
        ls_sweep_match_t tracker={.category=SW_TRACKER,.state=SW_STATE_SEPARATED,.label="AirTag",.vendor="Apple"};
        uint8_t address[6]={7,2,3,4,5,6};
        for(int i=0;i<=280;i++) {
            int64_t at=now-700000000+(int64_t)i*2500000;
            /* Real observation path with deliberate missing buckets. */
            if(i<260 || i%7!=0) ls_sweep_observe(address,0,1,-80+i%25,&tracker,at);
            ls_sweep_motion(!getenv("LSSIM_SWEEP_NO_GPS"),2,0,at);
        }
        ls_sweep_observe(address,0,1,-58,&tracker,now);
        ls_sweep_match_t cam={.category=SW_CAMERA,.label="Axis hint"},rid={.category=SW_DRONE,.label="Remote ID"};
        address[0]=8;ls_sweep_observe(address,1,0,-72,&cam,now-180000000);
        for(int i=0;i<=72;i++) ls_sweep_observe(address,1,0,-72,&cam,now-180000000+i*2500000LL);
        address[0]=9;for(int i=0;i<24;i++) ls_sweep_observe(address,0,0,-94+i*2,&rid,now-57500000+i*2500000LL);
        tracker.state=SW_STATE_NEAR;
        for(int i=0;i<6;i++) {address[0]=20+i;ls_sweep_observe(address,0,1,-50,&tracker,now);}
        uint8_t flags[]={2,1,6};for(int i=0;i<410;i++) {address[0]=100;ls_sweep_ble(address,0,-80,flags,sizeof(flags),now-i*20000);}
        return;
    }
    if(getenv("LSSIM_SWEEP_RADAR")) {
        /* Deterministic receiver observations, not a UI-only mock. */
        ls_sweep_settings_t settings;ls_sweep_settings_get(&settings);
        settings.enabled=31;ls_sweep_settings_set(&settings);
        static const int ages[]={0,2,5,8,11,14,17,20,23,26,29,32,36,40,44};
        static const char *const labels[]={"Axis camera hint","Bodycam name hint","Remote ID drone","Apple Find My","Attack rule hint"};
        static const char *const vendors[]={"Axis","Unverified","RID","Apple","Unverified"};
        for(unsigned i=0;i<15;i++) {
            uint8_t address[6]={0x12,0x34,0x56,0x78,(uint8_t)(i*17),(uint8_t)(i+1)};
            ls_sweep_match_t match={.category=i%5};
            strncpy(match.label,labels[i%5],sizeof(match.label)-1);
            strncpy(match.vendor,vendors[i%5],sizeof(match.vendor)-1);
            int64_t seen=now-(int64_t)ages[i]*1000000;
            int rssi=-42-(i*13)%48;
            if(i==2) {
                for(int j=0;j<12;j++) ls_sweep_observe(address,0,0,rssi-12+j,&match,seen-(12-j)*1000000);
                ls_sweep_hunt(address,0,0);
            }
            ls_sweep_observe(address,0,0,rssi,&match,seen);
        }
        return;
    }
    uint8_t axis[6]={0,0x40,0x8c,1,2,3};ls_sweep_wifi(axis,"",-61,now);
    uint8_t mac[6]={1,2,3,4,5,6};uint8_t apple[31]={30,0xff,0x4c,0,0x12,0x19};
    ls_sweep_ble(mac,1,-48,apple,sizeof(apple),now);
    mac[0]=2;uint8_t tile[]={3,3,0xed,0xfe};ls_sweep_ble(mac,0,-74,tile,sizeof(tile),now);
    mac[0]=3;uint8_t name[]={13,9,'B','o','d','y','c','a','m',' ','H','i','n','t'};ls_sweep_ble(mac,0,-56,name,sizeof(name),now);
    mac[0]=4;uint8_t ad[31]={30,0x16,0xfa,0xff,0x0d,0,2,0x12};memcpy(ad+8,"SWEEP-DEMO-RID",14);ls_sweep_ble(mac,0,-39,ad,31,now);
    memset(ad+6,0,25);ad[6]=0x12;ad[7]=0x20;ad[8]=90;ad[9]=48;
    u32(ad+11,434500000);u32(ad+15,-716500000);ad[21]=0xc0;ad[22]=8;
    for(int i=0;i<24;i++) ls_sweep_ble(mac,0,-62+i,ad,31,now);
    memset(ad+6,0,25);ad[6]=0x42;ad[7]=1;u32(ad+8,434490000);u32(ad+12,-716490000);ls_sweep_ble(mac,0,-39,ad,31,now);
    if(getenv("LSSIM_SWEEP_FLOOD")) for(unsigned i=0;i<40;i++) {mac[0]=i;mac[1]=0xee;ls_sweep_ble(mac,1,-50,apple,31,now);}
}
