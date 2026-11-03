/* Multiple real SDK contexts, foreign handles, bounded tables and deadlines. */
#include "ls_lap_api.h"
#include "ls_lap_sys.h"
#include "ls_lap_net.h"
#include "ls_lap_net_audio.h"
#include "ls_lap_service.h"
#include "ls_lap_system.h"
#include "ls_lap_radio.h"
#include "ls_lap_dsp.h"
#include "ls_lap_ble_peripheral.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint32_t now;
static int64_t us(void) { return (int64_t)now*1000; }
static uint32_t ms(void) { return now; }
extern uint32_t (*ls_lap_api_host_clock)(void);
static unsigned ticks[2];
static void tick_a(void) { ticks[0]++; }
static void tick_b(void) { ticks[1]++; }
static void begin(const char *id,void (*tick)(void)) {
    ls_lap_api_begin(id,1,0); ls_lap_sys_begin();
    lap_app_t a={.api_major=2,.api_minor=6,.flags=LAP_CAP_NETWORK|LAP_CAP_SERVICE|LAP_CAP_RADIO_RX|LAP_CAP_RADIO_TX|LAP_CAP_MIC};
    snprintf(a.id,sizeof(a.id),"%s",id); snprintf(a.name,sizeof(a.name),"%s",id);
    ls_lap_radio_begin(&a); ls_lap_net_begin(true); ls_lap_sys_service(); ls_lap_api_callback(true);
    lap_service_t s={.tick=tick,.period_ms=50}; assert(!lap_service_start(&s));
    char topic[32]="workers.test"; assert(!lap_ipc_subscribe(topic));
}
static void end(void) {
    ls_lap_api_callback(false); ls_lap_sys_end();
    for(int i=0;i<8;i++) ls_lap_sys_service();
    ls_lap_api_cleanup();
}
static int pure(const int16_t *iq,uint32_t n,const lap_iq_meta2_t *m,void *state,void *out,uint32_t cap) {
    assert(!ls_lap_api_inside()); assert(ls_lap_malloc(1)==NULL);
    assert(n==2048&&iq[0]==123&&m->rate_hz==16000&&cap>=4);
    (*(uint32_t *)state)++; memcpy(out,state,4); return 4;
}
static int forever(const int16_t *iq,uint32_t n,const lap_iq_meta2_t *m,void *s,void *o,uint32_t c) {
    volatile unsigned count=0; for(;;) count++; return (int)count;
}
static void dsp_contracts(void) {
    uint32_t *state=ls_lap_calloc(1,sizeof(*state)); assert(state);
    lap_dsp_config_t cfg={sizeof(cfg),1,10000,pure,state,sizeof(*state)};
    assert(lap_rx_dsp_start(12345,&cfg)==LAP_RX_EINVAL);
    int h=ls_lap_dsp_attach(17,&cfg); assert(h>0);
    int16_t iq[4096]={123}; lap_iq_meta2_t meta={.size=sizeof(meta),.version=1,.rate_hz=16000};
    for(int i=0;i<5;i++) ls_lap_dsp_submit(17,iq,2048,&meta);
    lap_dsp_status_t st; assert(!lap_rx_dsp_status(h,&st)&&st.queued==2&&st.dropped==3);
    ls_lap_api_callback(false); ls_lap_dsp_service(); ls_lap_api_callback(true);
    assert(!lap_rx_dsp_status(h,&st)&&st.blocks==1&&st.state==LAP_DSP_RUNNING);
    lap_dsp_result_t r; uint32_t result=0;
    assert(lap_rx_dsp_next(h,&r,&result,1)==LAP_RX_ERANGE);
    assert(lap_rx_dsp_next(h,&r,&result,4)==1&&result==1&&r.bytes==4);
    lap_rx_dsp_close(h); ls_lap_api_callback(false); ls_lap_dsp_service(); ls_lap_api_callback(true);
    assert(!lap_rx_dsp_status(h,&st)&&st.state==LAP_DSP_STOPPED);
    cfg.process=forever; cfg.budget_us=2000;
    h=ls_lap_dsp_attach(17,&cfg); assert(h>0); ls_lap_dsp_submit(17,iq,2048,&meta);
    ls_lap_api_callback(false); ls_lap_dsp_service(); ls_lap_api_callback(true);
    assert(!lap_rx_dsp_status(h,&st)&&st.state==LAP_DSP_OVERRUN&&st.overruns==1);
    assert(ls_lap_dsp_script("dspcost=20000")); cfg.process=pure;
    int h2=ls_lap_dsp_attach(17,&cfg); assert(h2>0&&lap_rx_dsp_status(h,&st)==LAP_RX_EINVAL);
    ls_lap_dsp_submit(17,iq,2048,&meta); ls_lap_api_callback(false); ls_lap_dsp_service(); ls_lap_api_callback(true);
    assert(!lap_rx_dsp_status(h2,&st)&&st.overruns==1&&st.last_us>=20000);
    ls_lap_dsp_script("dspcost=0"); ls_lap_dsp_script("dspfail=1");
    int failed=ls_lap_dsp_attach(17,&cfg); assert(failed>0); ls_lap_dsp_submit(17,iq,2048,&meta);
    ls_lap_api_callback(false); ls_lap_dsp_service(); ls_lap_api_callback(true);
    assert(!lap_rx_dsp_status(failed,&st)&&st.state==LAP_DSP_FAILED&&st.error==LAP_RX_ENOMEM&&st.overruns==0);
    ls_lap_dsp_script("dspfail=0"); ls_lap_free(state);
}
static void peripheral_lease(void) {
    lap_ble_advert_config_t cfg={.size=sizeof(cfg),.version=1,.interval_min_ms=100,.interval_max_ms=120};
    int h=lap_ble_advertise(&cfg); assert(h>0);
    ls_lap_api_callback(false);
    for(int i=0;i<5;i++) ls_lap_av_service();
    assert(ls_lap_resource_owner(LS_LAP_LEASE_BLE)==ls_lap_instance_current());
    ls_lap_api_callback(true);
    lap_ble_peripheral_status_t status;
    assert(!lap_ble_peripheral_status(h,&status)&&status.advertising);
    lap_gatt_server_close(h); ls_lap_api_callback(false);
    for(int i=0;i<8;i++) ls_lap_av_service();
    assert(!ls_lap_peripheral_reserved()&&!ls_lap_resource_owner(LS_LAP_LEASE_BLE));
    ls_lap_api_callback(true);
}
int main(void) {
    ls_lap_api_host_clock=ms; ls_lap_service_host_clock=us;
    ls_lap_sys_host()->hardware=LAP_HW_WIFI|LAP_HW_BLE|LAP_HW_SD|LAP_HW_SPEAKER|LAP_HW_MIC;
    ls_lap_instance_t *a=ls_lap_instance_current(),*b=ls_lap_instance_new(); assert(b);
    begin("logger",tick_a); peripheral_lease(); void *owned=ls_lap_malloc(32); assert(owned);
    lap_caps_t caps; assert(!lap_caps(&caps,sizeof(caps)));
    assert((caps.hardware&(LAP_HW_WORKER_DSP|LAP_HW_MDNS))==(LAP_HW_WORKER_DSP|LAP_HW_MDNS));
    char filename[32]="isolated.dat",mode[2]="a",sample[8]="hello";
    int fd=lap_file_open(filename,mode); assert(fd>=0&&lap_file_write(fd,sample,5)==5);
    int http=lap_http_open(&(lap_http_request_t){.size=sizeof(lap_http_request_t),.version=1,.timeout_ms=5000,.response_limit=1024,.url="https://example.com/"});
    assert(http>0);
    int mic=lap_mic_open(); assert(mic>0);
    int ble=lap_ble_scan_start(); assert(ble>0);
    lap_socket_address_t addr={.size=sizeof(addr),.version=1,.ip="127.0.0.1"};
    int udp=lap_udp_open(&addr); assert(udp>0);
    int16_t audio_a[512]; for(int i=0;i<512;i++) audio_a[i]=300;
    assert(!lap_mixer_config(0,100,100)&&lap_mixer_write(0,audio_a,512)==512);
    lap_rx_config_t rx_cfg={433920000,256000,LAP_GAIN_AUTO,0};
    int radio_a=lap_rx_open(0,&rx_cfg); assert(radio_a>0);
    ls_lap_api_callback(false); ls_lap_radio_service(); ls_lap_api_callback(true);
    assert(lap_rx_state(radio_a)==LAP_RX_RUNNING);
    uint32_t old_capture=ls_lap_rx_token();
    ls_lap_service_set_shown(false); ls_lap_api_callback(false); assert(ls_lap_instance_select(b));
    begin("notifier",tick_b);
    int16_t audio_b[512]; for(int i=0;i<512;i++) audio_b[i]=700;
    assert(!lap_mixer_config(0,100,100)&&lap_mixer_write(0,audio_b,512)==512);
    now+=20; ls_lap_api_callback(false); ls_lap_sys_service(); ls_lap_api_callback(true);
    assert(ls_lap_sound_host()->count==320&&ls_lap_sound_host()->pcm[0]==1000);
    int own_fd=lap_file_open(filename,mode); assert(own_fd>=0&&own_fd!=fd);
    lap_file_close(fd); assert(lap_file_write(fd,sample,5)<0&&lap_file_write(own_fd,sample,5)==5);
    assert(!ls_lap_api_owns(owned,1)); lap_operation_t op; lap_http_status_t hs;
    assert(lap_http_status(http,&hs)<0); assert(lap_udp_status(udp,&op)<0);
    assert(lap_mic_open()==LAP_IO_EBUSY&&lap_ble_scan_start()==LAP_IO_EBUSY);
    int radio_b=lap_rx_open(0,&rx_cfg); assert(radio_b>0&&radio_b!=radio_a);
    assert(!ls_lap_radio_control_allowed());
    ls_lap_api_callback(false); ls_lap_radio_service(); ls_lap_api_callback(true);
    assert(lap_rx_state(radio_b)==LAP_RX_EBUSY);
    lap_rx_close(radio_b); ls_lap_api_callback(false); ls_lap_radio_service();
    assert(ls_lap_instance_select(a)); ls_lap_api_callback(true); lap_rx_close(radio_a);
    ls_lap_api_callback(false); ls_lap_radio_service(); assert(ls_lap_instance_select(b)); ls_lap_api_callback(true);
    radio_b=lap_rx_open(0,&rx_cfg); assert(radio_b>0);
    ls_lap_api_callback(false); ls_lap_radio_service(); ls_lap_api_callback(true);
    assert(lap_rx_state(radio_b)==LAP_RX_RUNNING);
    assert(ls_lap_radio_control_allowed());
    lap_rx_status_t before,after; assert(!lap_rx_status(radio_b,&before));
    int16_t late[4096]={0};
    ls_lap_rx_push_token(old_capture,late,2048); ls_lap_rx_mark_token(old_capture,100000000,48000,LAP_IQ_GAP);
    assert(!lap_rx_status(radio_b,&after));
    assert(before.queued==after.queued&&before.center_hz==after.center_hz&&before.gaps==after.gaps);
    int own_http=lap_http_open(&(lap_http_request_t){.size=sizeof(lap_http_request_t),.version=1,.timeout_ms=5000,.response_limit=1024,.url="https://example.com/"}); assert(own_http>0&&own_http!=http);
    lap_service_info_t info[4]; assert(lap_service_list(info,4)==2&&lap_service_limit()==CONFIG_LS_LAP_SERVICES_MAX);
    char topic[32]="workers.test",body[8]="ok",name[32]="logger";
    assert(lap_service_query(name,info)==1&&!info[0].shown&&info[0].memory_bytes>=32);
    assert(info[0].pool_bytes<=info[0].pool_limit&&info[0].handles<=info[0].handle_limit);
    assert(lap_ipc_publish(topic,1,body,3)==2);
    for(int k=0;k<4;k++) {
        now+=50; ls_lap_api_callback(false); ls_lap_service_poll();
        assert(ls_lap_instance_select(a)); ls_lap_service_poll(); assert(ls_lap_instance_select(b)); ls_lap_api_callback(true);
    }
    assert(ticks[0]==4&&ticks[1]==4);
    dsp_contracts(); end();
    assert(ls_lap_instance_select(a)); ls_lap_api_callback(true);
    assert(ls_lap_api_owns(owned,32)); assert(lap_http_status(http,&hs)==0);
    assert(lap_ipc_publish(topic,1,body,3)==1); /* unloaded subscription gone */
    assert(ls_lap_service_request_stop("logger")&&ls_lap_service_take_stop());
    ls_lap_free(owned); end(); assert(ls_lap_instance_delete(b));
    puts("API 2.6 multiple services / ownership / DSP deadline PASS"); return 0;
}
