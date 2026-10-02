/* LS_TEST_SOURCES: experiments/lorafam_decode.c; IEEE 802.15.4 SUN FSK
   PHR vectors constructed from the standard's MS/FCS/DW/length fields. */
#include "ls_test.h"
#include <string.h>
#include "lorafam_decode.h"
LS_CASE(wisun_normal_phr_fields)
{
    lorafam_phr_t p; LS_CHECK(lorafam_wisun_phr(0x0864,&p)); LS_EQ_UINT(p.length,100);
    LS_CHECK(p.whitening); LS_CHECK(!p.crc16); LS_CHECK(!p.mode_switch);
    LS_CHECK(lorafam_wisun_phr(0x17ff,&p)); LS_EQ_UINT(p.length,2047); LS_CHECK(p.crc16);
    LS_CHECK(!lorafam_wisun_phr(0x2004,&p)); LS_CHECK(!lorafam_wisun_phr(0x0803,&p));
    LS_CHECK(lorafam_wisun_phr(0x8000,&p)); LS_CHECK(p.mode_switch); LS_EQ_UINT(p.length,0);
}
LS_CASE(wisun_mac_frame_type_is_a_header_guess)
{
    uint8_t type; const uint8_t data[]={0x41,0x88,0x13,0x34,0x12,0x01,0,0x02,0};
    LS_CHECK(lorafam_wisun_mac(data,sizeof(data),&type)); LS_EQ_UINT(type,1);
    const uint8_t ack[]={2,0,1}; LS_CHECK(lorafam_wisun_mac(ack,3,&type)); LS_EQ_UINT(type,2);
    LS_CHECK(!lorafam_wisun_mac(data,1,&type)); LS_CHECK(!lorafam_wisun_mac(ack,2,&type));
    const uint8_t reserved[]={7,0,1}; LS_CHECK(!lorafam_wisun_mac(reserved,3,&type));
}

#include "ls_experiments.h"
#include "lorafam_module_fake.h"
extern const ls_experiment_t exp_wisun;
LS_CASE(wisun_options_plan_and_oversized_readout)
{
    char why[80]; char *args[]={"3","41"}; LS_CHECK(exp_wisun.configure(2,args,why,sizeof(why)));
    LS_CHECK(exp_wisun.start(why,sizeof(why))); LS_EQ_UINT(lf_preset,2); LS_EQ_UINT(lf_channel,41); LS_EQ_UINT(lf_fec,1);
    lf_packet=(lr20xx_engine_packet_t){.length=300,.phr=0x092c,.rssi_dbm=-100}; lf_result=-2; exp_wisun.poll();
    char out[20][LS_EXP_LINE]; int n=exp_wisun.lines(out,20); LS_CHECK(n>=7);
    LS_CHECK(strstr(out[0],"927.200 MHz")); LS_CHECK(strstr(out[2],"PHR 1 frames 0")); LS_CHECK(strstr(out[3],"oversized dropped 1"));
    for(int i=0;i<n;i++) LS_CHECK(strlen(out[i])<LS_EXP_LINE);
    exp_wisun.stop(); args[1]="42"; LS_CHECK(!exp_wisun.configure(2,args,why,sizeof(why)));
    lf_begin_error=ESP_ERR_INVALID_STATE; LS_CHECK(!exp_wisun.start(why,sizeof(why))); lf_begin_error=ESP_OK;
}

LS_CASE(wisun_is_marked_lr2021_only)
{
    LS_CHECK(exp_wisun.lr2021_only);
}
