/* LS_TEST_SOURCES: experiments/lorafam_decode.c; synthetic frames encoded
   from Meshtastic RadioInterface.h and MeshCore Packet::writeTo. */
#include "ls_test.h"
#include <string.h>
#include "lorafam_decode.h"
LS_CASE(meshtastic_header_is_little_endian)
{
    const uint8_t p[] = {255,255,255,255,0x78,0x56,0x34,0x12,0xef,0xcd,0xab,0x90,0x63,0x2b,0,0,1};
    lorafam_mesh_t m;
    LS_CHECK(lorafam_meshtastic(p,sizeof(p),&m)); LS_EQ_UINT(m.source,0x12345678); LS_EQ_UINT(m.dest,0xffffffff);
    LS_EQ_UINT(m.id,0x90abcdef); LS_EQ_UINT(m.flags,0x63); LS_EQ_UINT(m.channel,0x2b);
    for (size_t n=0;n<16;n++) LS_CHECK(!lorafam_meshtastic(p,n,&m));
    LS_CHECK(!lorafam_meshtastic(NULL,20,&m));
}
LS_CASE(meshcore_header_path_and_hash_are_bounded)
{
    const uint8_t p[] = {0x09,2,0xaa,0xbb,0x43,0x21,0x12,0x34,0x88};
    lorafam_mesh_t m;
    LS_CHECK(lorafam_meshcore(p,sizeof(p),&m)); LS_EQ_UINT(m.type,2); LS_EQ_UINT(m.route,1);
    LS_CHECK(m.source_hash); LS_EQ_UINT(m.source,0x21);
    uint8_t bad[sizeof(p)]; memcpy(bad,p,sizeof(p)); bad[1]=60; LS_CHECK(!lorafam_meshcore(bad,sizeof(bad),&m));
    bad[1]=0xc0; LS_CHECK(!lorafam_meshcore(bad,sizeof(bad),&m));
    for(size_t n=0;n<5;n++) LS_CHECK(!lorafam_meshcore(p,n,&m));
}
LS_CASE(meshcore_advert_and_transport_headers)
{
    uint8_t p[34]={0x11,0,0x78,0x56,0x34,0x12}; lorafam_mesh_t m;
    LS_CHECK(lorafam_meshcore(p,sizeof(p),&m)); LS_EQ_UINT(m.source,0x12345678); LS_CHECK(!m.source_hash);
    const uint8_t transport[]={0x08,1,0,2,0,0,9,8,0,0};
    LS_CHECK(lorafam_meshcore(transport,sizeof(transport),&m)); LS_EQ_UINT(m.source,8);
}

#include "ls_experiments.h"
#include "lorafam_module_fake.h"
extern const ls_experiment_t exp_loramon;
LS_CASE(loramon_presets_and_sender_table_reach_readout)
{
    char why[80]; char *args[]={"meshtastic"};
    LS_CHECK(exp_loramon.configure(1,args,why,sizeof(why))); LS_CHECK(exp_loramon.start(why,sizeof(why)));
    LS_EQ_INT(lf_engine,LR20XX_ENGINE_LORAMON); LS_EQ_UINT(lf_preset,1);
    const uint8_t p[]={255,255,255,255,0x78,0x56,0x34,0x12,1,2,3,4,0x63,0x2b,0,0,1};
    memcpy(lf_data,p,sizeof(p)); lf_result=sizeof(p); lf_packet=(lr20xx_engine_packet_t){.length=sizeof(p),.rssi_dbm=-80.5f,.snr_db=3.5f,.fei_hz=-1234,.detector=8,.cr=1};
    exp_loramon.poll(); lf_result=sizeof(p); exp_loramon.poll();
    char out[20][LS_EXP_LINE]; int n=exp_loramon.lines(out,20); bool found=false;
    for(int i=0;i<n;i++) { LS_CHECK(strlen(out[i])<LS_EXP_LINE); if(strstr(out[i],"node 12345678 2")) found=true; }
    LS_CHECK(found); LS_CHECK(strstr(out[3],"SF11 CR4/5")); LS_CHECK(strstr(out[4],"-1234 Hz"));
    exp_loramon.stop(); args[0]="bogus"; LS_CHECK(!exp_loramon.configure(1,args,why,sizeof(why)));
}

LS_CASE(loramon_is_marked_lr2021_only)
{
    LS_CHECK(exp_loramon.lr2021_only);
}
