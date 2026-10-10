#include "ls_test.h"
#include "ls_rid.h"
#include "esp_timer.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
/* Constructed from the standard's field offsets; no captured/project vectors. */
static uint8_t mac[6]={1,2,3,4,5,6};
static ls_rid_drone_t d[32];
static void le16(uint8_t *p,unsigned x) {p[0]=x;p[1]=x>>8;}
static void le32(uint8_t *p,int32_t x) {uint32_t u=(uint32_t)x;for(int i=0;i<4;i++)p[i]=u>>(8*i);}
static bool send(const uint8_t *msg,size_t n,int64_t now) {
    uint8_t ad[255]={0,0x16,0xfa,0xff,0x0d,42}; LS_CHECK(n+5<255);
    ad[0]=(uint8_t)(n+5); memcpy(ad+6,msg,n);
    return ls_rid_advert(mac,0,-52,ad,n+6,now);
}
static void one(int64_t now) {LS_CHECK(ls_rid_snapshot(d,32,now)==1);}
LS_CASE(documented_vectors_and_bounds) {
    uint8_t basic[25]={0x02,0x12}; memcpy(basic+2,"LAKESHARK-TEST",14);
    LS_CHECK(send(basic,25,1000000)); one(1000000); LS_CHECK(!strcmp(d[0].uas_id,"LAKESHARK-TEST"));
    LS_CHECK(d[0].rssi==-52 && d[0].counter==42 && isnan(d[0].geo_m));
    uint32_t serial=d[0].serial;
    uint8_t loc[25]={0x12,0x23,10,20,0xfe};
    le32(loc+5,377749000); le32(loc+9,-1224194000);
    le16(loc+13,2200); le16(loc+15,2301); le16(loc+17,2100);
    LS_CHECK(send(loc,25,2000000)); one(2000000);
    LS_CHECK(fabs(d[0].lat-37.7749)<1e-6 && fabs(d[0].lon+122.4194)<1e-6);
    LS_CHECK(d[0].serial==serial && d[0].position_valid && d[0].status==2);
    LS_CHECK(d[0].track_deg==190 && d[0].speed_mps==78.75 && d[0].vertical_mps==-1);
    LS_CHECK(d[0].geo_m==150.5 && d[0].baro_m==100 && d[0].height_m==50 && !d[0].height_agl);
    uint8_t sys[25]={0x42,5}; le32(sys+2,377740000); le32(sys+6,-1224180000);
    le16(sys+10,3); sys[12]=12; le16(sys+13,2600); le16(sys+15,2000);sys[17]=0x23;le16(sys+18,2080);
    LS_CHECK(send(sys,25,3000000));one(3000000);
    LS_CHECK(d[0].operator_valid && d[0].area_count==3 && d[0].area_radius_m==120);
    LS_CHECK(d[0].area_ceiling_m==300 && d[0].area_floor_m==0 && d[0].operator_geo_m==40);
    LS_CHECK(d[0].classification_type==1 && d[0].category==2 && d[0].class_id==3 && d[0].operator_location_type==1);
    uint8_t auth[25]={0x22,0x31}; LS_CHECK(send(auth,25,4000000));one(4000000);
    LS_CHECK(d[0].auth_type==3 && d[0].auth_page==1);
    uint8_t self[25]={0x32}; memcpy(self+2,"SURVEY",6); LS_CHECK(send(self,25,5000000));one(5000000); LS_CHECK(!strcmp(d[0].self_id,"SURVEY"));
    uint8_t op[25]={0x52};memcpy(op+2,"OPERATOR-TEST",13);LS_CHECK(send(op,25,6000000));one(6000000); LS_CHECK(!strcmp(d[0].operator_id,"OPERATOR-TEST"));
    LS_CHECK(d[0].messages==63);
    uint8_t pack[153]={0xf2,25,6};
    memcpy(pack+3,basic,25);memcpy(pack+28,loc,25);memcpy(pack+53,auth,25);
    memcpy(pack+78,self,25);memcpy(pack+103,sys,25);memcpy(pack+128,op,25);
    ls_rid_clear(); LS_CHECK(send(pack,sizeof(pack),7000000));one(7000000);LS_CHECK(d[0].messages==63);
    for(size_t n=0;n<sizeof(pack);n++) LS_CHECK(!send(pack,n,8000000));
    pack[1]=24;LS_CHECK(!send(pack,sizeof(pack),8000000));pack[1]=25;
    pack[2]=10;LS_CHECK(!send(pack,sizeof(pack),8000000));pack[2]=6;
    pack[28]=0xf2;LS_CHECK(!send(pack,sizeof(pack),8000000));pack[28]=0x12;
    pack[28]=0x19;LS_CHECK(!send(pack,sizeof(pack),8000000));
    one(8000000);LS_CHECK(d[0].last_us==7000000); /* malformed packs are atomic */
    for(size_t n=0;n<25;n++) LS_CHECK(!send(basic,n,8000000));
    uint8_t malformed[]={31,0x16,0xfa,0xff,0x0d};
    LS_CHECK(!ls_rid_advert(mac,0,-50,malformed,sizeof(malformed),8000000));
    uint8_t wrong[31]={30,0x16,0xfb,0xff,0x0d};memcpy(wrong+6,basic,25);
    LS_CHECK(!ls_rid_advert(mac,0,-50,wrong,31,8000000));
    wrong[2]=0xfa;wrong[4]=0x0e;LS_CHECK(!ls_rid_advert(mac,0,-50,wrong,31,8000000));
    memset(loc+5,0,14);loc[2]=255;loc[3]=255;loc[4]=128;
    LS_CHECK(send(loc,25,9000000));one(9000000);
    LS_CHECK(!d[0].position_valid && isnan(d[0].geo_m) && isnan(d[0].speed_mps) && isnan(d[0].vertical_mps) && isnan(d[0].track_deg));
    loc[4]=126;loc[1]=0x20;
    LS_CHECK(send(loc,25,9500000));one(9500000);
    LS_CHECK(isnan(d[0].vertical_mps) && d[0].speed_mps==63.75f);
    loc[1]=0x21;
    le32(loc+5,910000000);LS_CHECK(send(loc,25,10000000));one(10000000);LS_CHECK(!d[0].position_valid);
    LS_CHECK(ls_rid_snapshot(d,32,69999999)==1);LS_CHECK(ls_rid_snapshot(d,32,70000000)==0);
    /* Promote location-before-ID without discarding telemetry. */
    LS_CHECK(send(loc,25,80000000));one(80000000);serial=d[0].serial;
    LS_CHECK(send(basic,25,81000000));one(81000000);LS_CHECK(d[0].serial==serial && (d[0].messages&2));
    /* Same MAC, distinct UAS identities remain distinct. */
    basic[2]='X';LS_CHECK(send(basic,25,82000000));LS_CHECK(ls_rid_snapshot(d,32,82000000)==2);
    ls_rid_clear();
    for(int i=0;i<40;i++) {mac[0]=i;LS_CHECK(send(basic,25,90000000+i));}
    LS_CHECK(ls_rid_snapshot(d,32,91000000)==32);LS_CHECK(ls_rid_snapshot(d,1,91000000)==1);
    /* All 256 bytes in text fields are rendered without control characters. */
    for(int i=1;i<256;i++) {memset(self+2,i,23);LS_CHECK(send(self,25,92000000));}
    ls_rid_clear(); ls_shim_time_set(95000000);char *list[]={"rid"};LS_CHECK(ls_rid_command(1,list)==0);
    char *bad[]={"rid","detail","1x"};LS_CHECK(ls_rid_command(3,bad)==1);
    puts("Remote ID vectors, bounds, identity, expiry, capacity: PASS");
}

LS_CASE(random_ad_lengths_do_not_escape_the_table)
{
    uint32_t rng=0x51347;
    uint8_t bytes[255], addr[6]={0};
    ls_rid_clear();
    for(int pass=0;pass<20000;pass++) {
        for(unsigned i=0;i<sizeof(bytes);i++) {rng=rng*1664525u+1013904223u;bytes[i]=(uint8_t)(rng>>24);}
        size_t len=(rng>>16)%256;
        ls_rid_is_advert(bytes,len);
        ls_rid_advert(addr,0,-60,bytes,len,1000000);
        LS_CHECK(ls_rid_snapshot(d,32,1000000)<=32);
    }
    LS_CHECK(!ls_rid_advert(NULL,0,-60,bytes,25,0));
    LS_CHECK(!ls_rid_advert(addr,0,-60,NULL,25,0));
    LS_CHECK(!ls_rid_advert(addr,0,-60,bytes,25,-1));
    uint8_t bad[]={30,0x16,0xfa,0xff,0x0d,0,0xff};
    LS_CHECK(ls_rid_is_advert(bad,sizeof(bad)));
    LS_CHECK(!ls_rid_advert(addr,0,-60,bad,sizeof(bad),0));
}

static void *producer(void *arg)
{
    uint8_t ad[31]={30,0x16,0xfa,0xff,0x0d,0,2,0x12},addr[6]={0};
    addr[0]=(uint8_t)(uintptr_t)arg;memcpy(ad+8,"CONCURRENT",10);
    for(int i=0;i<1000;i++) ls_rid_advert(addr,0,-50,ad,sizeof(ad),100000000);
    return NULL;
}
LS_CASE(scan_and_snapshot_are_synchronized)
{
    ls_rid_clear();pthread_t threads[4];
    for(int i=0;i<4;i++) LS_CHECK(!pthread_create(&threads[i],NULL,producer,(void *)(uintptr_t)(i+1)));
    for(int i=0;i<1000;i++) LS_CHECK(ls_rid_snapshot(d,32,100000000)<=4);
    for(int i=0;i<4;i++) LS_CHECK(!pthread_join(threads[i],NULL));
    LS_CHECK(ls_rid_snapshot(d,32,100000000)==4);
}

LS_CASE(link_summary_nearest_requires_fresh_position_and_valid_fix) {
    ls_rid_clear();uint8_t basic[25]={0x02,0x12};memcpy(basic+2,"NEAREST-TEST",12);
    LS_CHECK(send(basic,25,1000000));
    uint8_t loc[25]={0x12};le32(loc+5,400000000);le32(loc+9,-740000000);
    LS_CHECK(send(loc,25,2000000));
    ls_rid_summary_t s;ls_rid_summary(&s,true,40.001,-74,2000000);
    LS_CHECK(s.count==1 && s.nearest_m>=110 && s.nearest_m<=112 && !strcmp(s.id,"NEAREST-TEST"));
    ls_rid_summary(&s,false,40.001,-74,2000000);LS_CHECK(s.count==1 && s.nearest_m==-1 && !s.id[0]);
    ls_rid_summary(&s,true,NAN,-74,2000000);LS_CHECK(s.nearest_m==-1);
    LS_CHECK(send(basic,25,61000000));
    ls_rid_summary(&s,true,40.001,-74,62000000);LS_CHECK(s.count==1 && s.nearest_m==-1 && !s.id[0]);
    ls_rid_summary(&s,true,40.001,-74,121000000);LS_CHECK(s.count==0 && s.nearest_m==-1);
}
