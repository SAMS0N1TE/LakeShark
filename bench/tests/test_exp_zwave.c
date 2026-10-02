/* LS_TEST_SOURCES: experiments/lorafam_decode.c; US classic MPDUs encoded
   using G.9959 header layout and independent XOR/CRC test vectors. */
#include "ls_test.h"
#include <string.h>
#include "lorafam_decode.h"
LS_CASE(us_r1_and_r2_mac_checksum)
{
    const uint8_t p[]={0xde,0xad,0xbe,0xef,1,0x41,2,10,3,0x96};
    lorafam_zwave_t z;
    for(uint8_t rate=1;rate<=2;rate++) {
        LS_CHECK(lorafam_zwave(p,sizeof(p),rate,&z)); LS_CHECK(z.checksum_ok);
        LS_EQ_UINT(z.homeid,0xdeadbeef); LS_EQ_UINT(z.source,1); LS_EQ_UINT(z.dest,3); LS_EQ_UINT(z.control,0x4102);
    }
    uint8_t bad[10]; memcpy(bad,p,10); bad[9]^=1; LS_CHECK(lorafam_zwave(bad,10,1,&z)); LS_CHECK(!z.checksum_ok);
    bad[7]=9; LS_CHECK(!lorafam_zwave(bad,10,1,&z));
    for(size_t n=0;n<10;n++) LS_CHECK(!lorafam_zwave(p,n,1,&z));
    LS_CHECK(!lorafam_zwave(p,10,0,&z));
}
LS_CASE(us_r3_crc16_and_multicast)
{
    const uint8_t p[]={0xde,0xad,0xbe,0xef,1,0x41,2,11,3,0x7a,0x27}; lorafam_zwave_t z;
    LS_CHECK(lorafam_zwave(p,sizeof(p),3,&z)); LS_CHECK(z.checksum_ok);
    uint8_t bad[11]; memcpy(bad,p,11); bad[0]^=1;
    LS_CHECK(lorafam_zwave(bad,11,3,&z)); LS_CHECK(!z.checksum_ok);
    bad[5]=2; LS_CHECK(lorafam_zwave(bad,11,3,&z)); LS_CHECK(!z.dest_known);
}

#include "ls_experiments.h"
#include "lorafam_module_fake.h"
extern const ls_experiment_t exp_zwave;
LS_CASE(zwave_table_uses_only_frames_with_good_fcs)
{
    char why[80]; LS_CHECK(exp_zwave.start(why,sizeof(why))); LS_EQ_INT(lf_engine,LR20XX_ENGINE_ZWAVE);
    const uint8_t p[]={0xde,0xad,0xbe,0xef,1,0x41,2,10,3,0x96}; memcpy(lf_data,p,10);
    lf_packet=(lr20xx_engine_packet_t){.length=10,.rate=2,.rssi_dbm=-80}; lf_result=10; exp_zwave.poll();
    lf_data[9]^=1; lf_result=10; exp_zwave.poll();
    char out[20][LS_EXP_LINE]; int n=exp_zwave.lines(out,20); bool found=false;
    for(int i=0;i<n;i++) { LS_CHECK(strlen(out[i])<LS_EXP_LINE); if(strstr(out[i],"DEADBEEF   1     1")) found=true; }
    LS_CHECK(found); LS_CHECK(strstr(out[2],"bad FCS/header 1")); LS_CHECK(strstr(out[5],"FCS BAD"));
    exp_zwave.stop();
}

LS_CASE(zwave_is_marked_lr2021_only)
{
    LS_CHECK(exp_zwave.lr2021_only);
}
