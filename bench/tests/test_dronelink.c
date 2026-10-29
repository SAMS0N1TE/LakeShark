#include "ls_test.h"
#include "ls_dronelink.h"
#include "board/ls_lora.h"
#include <string.h>
static bool mesh,fg,bg,present=true,lease,rx;
static unsigned configs,releases,polls,observed,band,rate,hits;
static bool bad_config;static int flags;static float level=-65;
static ls_lora_cfg_t cfg={.freq_hz=910525000,.sf=7,.bw_hz=62500};
bool ls_mesh_running(void) {return mesh;}
bool ls_mesh_radio_wanted(void) {return fg;}
bool ls_mesh_radio_bg(void) {return bg;}
bool ls_mesh_radio_hold_bg(bool on) {if(on){lease=!fg && !bg;return lease;}lease=false;releases++;return true;}
bool ls_lora_present(void) {return present;}
uint32_t ls_lora_caps(void) {return LS_LORA_CAP_LORA;}
const ls_lora_cfg_t *ls_lora_cfg(void) {return &cfg;}
void ls_lora_cfg_default(ls_lora_cfg_t *c) {*c=(ls_lora_cfg_t){.freq_hz=910525000,.sf=7,.bw_hz=62500};}
bool ls_lora_is_receiving(void) {return rx;}
esp_err_t ls_lora_configure(const ls_lora_cfg_t *c) {
    LS_CHECK(lease);configs++;cfg=*c;rx=false;return bad_config?ESP_FAIL:ESP_OK;
}
esp_err_t ls_lora_receive(void) {LS_CHECK(lease);rx=true;return ESP_OK;}
int ls_lora_activity(float *r) {LS_CHECK(lease);polls++;*r=level;return flags;}
void ls_seen_drone(unsigned b,unsigned channel,unsigned r,int rssi,unsigned h,int64_t now) {
    (void)channel;(void)now;LS_CHECK(rssi==-65);observed++;band=b;rate=r;hits=h;
}
LS_CASE(dronelink_lease_hopping_hits_and_failures) {
    ls_dronelink_tick(false,0);configs=observed=polls=0;
    mesh=true;ls_dronelink_tick(true,10000000);LS_CHECK(configs==0 && !strcmp(ls_dronelink_status(),"CONFLICT"));mesh=false;
    bg=true;ls_dronelink_tick(true,11000000);LS_CHECK(configs==0);bg=false;
    fg=true;ls_dronelink_tick(true,12000000);LS_CHECK(configs==0);fg=false;
    present=false;ls_dronelink_tick(true,20000000);LS_CHECK(!strcmp(ls_dronelink_status(),"UNAVAILABLE"));present=true;
    ls_dronelink_tick(true,30000000);LS_CHECK(lease && cfg.freq_hz==903500000 && cfg.sf==6 && cfg.sync_word==0x12 && cfg.bw_hz==500000);
    flags=LS_LORA_PREAMBLE|LS_LORA_HEADER;ls_dronelink_tick(true,30125000);
    LS_CHECK(observed==1 && band==915 && rate==200 && hits==1);
    flags=0;
    for(unsigned i=1;i<=40;i++)ls_dronelink_tick(true,30000000+i*250000);
    LS_CHECK(cfg.freq_hz==865275000);
    flags=LS_LORA_PREAMBLE;ls_dronelink_tick(true,40125000);LS_CHECK(band==868 && hits==1);
    flags=0;for(unsigned i=41;i<=53;i++)ls_dronelink_tick(true,30000000+i*250000);
    LS_CHECK(cfg.freq_hz==903500000 && cfg.sf==6 && cfg.cr==8 && cfg.implicit_rx_bytes==13);
    fg=true;unsigned before=polls;ls_dronelink_tick(true,44000000);
    LS_CHECK(!lease && polls==before && cfg.freq_hz==910525000 && !ls_dronelink_pending());fg=false;
    ls_dronelink_tick(true,50000000);flags=-1;ls_dronelink_tick(true,50125000);
    LS_CHECK(!lease && !strcmp(ls_dronelink_status(),"UNAVAILABLE"));
    flags=0;ls_dronelink_tick(true,60000000);ls_dronelink_tick(false,60125000);LS_CHECK(!lease && !ls_dronelink_pending());
}
