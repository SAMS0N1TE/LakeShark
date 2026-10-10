#include "ls_test.h"
#include "ble_link_observer.h"
#include "ls_rid.h"
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
static ble_link_observer_t ring;
static ble_link_advert_t received;
static unsigned calls;
static const uint8_t mac[6] = {1,2,3,4,5,6};
LS_CASE(raw_ad_walker_handles_all_field_types_and_boundaries)
{
    uint8_t ad[128]={0}; size_t len=0;
    const uint8_t types[]={1,8,9,2,3,4,5,6,7,0x16,0x20,0x21,0xff,0xee};
    const uint8_t sizes[]={1,3,4,2,2,4,4,16,16,2,4,16,2,0};
    for(unsigned i=0;i<sizeof(types);i++) {
        ad[len++]=sizes[i]+1;ad[len++]=types[i];
        memset(ad+len,'A'+i,sizes[i]);len+=sizes[i];
    }
    LS_CHECK(ls_ble_ad_valid(ad,len));
    size_t off=0,name_len=0;ls_ble_ad_field_t f={0};
    for(unsigned i=0;i<sizeof(types);i++) {
        LS_CHECK(ls_ble_ad_next(ad,len,&off,&f)==1);
        LS_CHECK(f.type==types[i] && f.len==sizes[i]);
    }
    LS_CHECK(ls_ble_ad_next(ad,len,&off,&f)==0);
    const uint8_t *name=ls_ble_ad_name(ad,len,&name_len);
    LS_CHECK(name && name_len==4 && !memcmp(name,"CCCC",4));
    uint8_t names[]={2,9,'C',2,8,'S',0,255};
    name=ls_ble_ad_name(names,sizeof(names),&name_len);
    LS_CHECK(name && name_len==1 && *name=='C');
    LS_CHECK(ls_ble_ad_valid(names,sizeof(names))); /* zero terminates padding */
    names[6]=255;
    LS_CHECK(!ls_ble_ad_name(names,sizeof(names),&name_len) && name_len==0);
    LS_CHECK(!ls_ble_ad_valid(names,sizeof(names)));
    LS_CHECK(ls_ble_ad_valid(NULL,0) && !ls_ble_ad_valid(NULL,1));
    uint8_t empty[]={1,9};
    LS_CHECK(ls_ble_ad_name(empty,sizeof(empty),&name_len)==empty+2 && name_len==0);
    LS_CHECK(!ls_ble_ad_valid(ad,LS_BLE_AD_MAX+1));
    /* Exact heap allocations make boundary overreads visible to sanitizers. */
    const size_t lengths[]={31,255,256,LS_BLE_AD_MAX};
    for(unsigned i=0;i<sizeof(lengths)/sizeof(lengths[0]);i++) {
        size_t n=lengths[i];uint8_t *bytes=calloc(n,1);LS_CHECK(bytes);
        if(!bytes) return;
        bytes[0]=n>256?255:n-1;bytes[1]=9;
        memset(bytes+2,'X',bytes[0]-1);
        name=ls_ble_ad_name(bytes,n,&name_len);
        LS_CHECK(name==bytes+2 && name_len==(size_t)bytes[0]-1);
        for(size_t cut=1;cut<=(size_t)bytes[0];cut++)
            LS_CHECK(!ls_ble_ad_valid(bytes,cut));
        free(bytes);
    }
}

LS_CASE(raw_ad_walker_fuzz_matches_framing_reference)
{
    ls_rng_t rng;ls_rng_seed(&rng,0x514d);
    for(unsigned pass=0;pass<20000;pass++) {
        size_t len=pass%257;uint8_t *bytes=malloc(len?len:1);LS_CHECK(bytes);
        if(!bytes) return;
        for(size_t i=0;i<len;i++) bytes[i]=ls_rng_u32(&rng);
        size_t ref=0;bool valid=true;
        while(ref<len && bytes[ref]) {
            size_t next=ref+1+bytes[ref];
            if(next>len) {valid=false;break;}
            ref=next;
        }
        LS_CHECK(ls_ble_ad_valid(bytes,len)==valid);
        size_t off=0;ls_ble_ad_field_t f;unsigned count=0;int rc;
        while((rc=ls_ble_ad_next(bytes,len,&off,&f))>0) {
            LS_CHECK(f.data>=bytes && f.data<=bytes+len);
            LS_CHECK(f.len<=(size_t)(bytes+len-f.data));
            LS_CHECK(++count<=len/2);
        }
        LS_CHECK((rc==0)==valid);
        size_t name_len;const uint8_t *name=ls_ble_ad_name(bytes,len,&name_len);
        if(!valid) LS_CHECK(!name && name_len==0);
        if(name) LS_CHECK(name>=bytes && name<=bytes+len && name_len<=(size_t)(bytes+len-name));
        free(bytes);
    }
}
static void capture(const ble_link_advert_t *a, void *ctx)
{
    (*(unsigned *)ctx)++;
    received = *a;
}
LS_CASE(copies_advert_and_scan_response_and_dispatches_only_on_consumer)
{
    memset(&ring,0,sizeof(ring)); calls=0;
    unsigned other=0;
    LS_CHECK(ble_link_observer_subscribe(&ring,capture,&calls));
    LS_CHECK(ble_link_observer_subscribe(&ring,capture,&calls));
    LS_CHECK(ble_link_observer_subscribe(&ring,capture,&other));
    uint8_t data[]={4,9,'P','4',0};
    LS_CHECK(ble_link_observer_push(&ring,mac,1,-88,false,data,sizeof(data),123));
    memset(data,0,sizeof(data));
    LS_CHECK(calls==0 && other==0);
    LS_CHECK(ble_link_observer_dispatch(&ring,1)==1);
    LS_CHECK(calls==1 && other==1 && !received.scan_response);
    LS_CHECK(received.rssi==-88 && received.addr_type==1 && received.seen_us==123);
    LS_CHECK(!memcmp(received.addr,mac,6) && received.data[2]=='P');
    LS_CHECK(ble_link_observer_push(&ring,mac,1,-60,true,data,sizeof(data),456));
    LS_CHECK(ble_link_observer_dispatch(&ring,1)==1 && received.scan_response && received.seen_us==456);
}
LS_CASE(full_ring_drops_new_reports_without_overwriting_and_wraps_counters)
{
    memset(&ring,0,sizeof(ring));calls=0;
    ring.read=ring.write=UINT32_MAX-15;
    ble_link_observer_subscribe(&ring,capture,&calls);
    uint8_t data[BLE_LINK_AD_MAX]; memset(data,0xa5,sizeof(data));
    for(unsigned i=0;i<BLE_LINK_AD_DEPTH;i++)
        LS_CHECK(ble_link_observer_push(&ring,mac,0,-30,false,data,sizeof(data),i));
    LS_CHECK(!ble_link_observer_push(&ring,mac,0,-20,false,data,1,999));
    LS_CHECK(ring.dropped==1);
    for(unsigned i=0;i<BLE_LINK_AD_DEPTH;i++) {
        LS_CHECK(ble_link_observer_dispatch(&ring,1)==1);
        LS_CHECK(received.seen_us==i && received.len==sizeof(data) && received.data[1649]==0xa5);
    }
    LS_CHECK(calls==BLE_LINK_AD_DEPTH && ble_link_observer_dispatch(&ring,1)==0);
    LS_CHECK(ble_link_observer_push(&ring,mac,0,-20,false,data,1,1000));
    LS_CHECK(ble_link_observer_dispatch(&ring,1)==1 && received.seen_us==1000);
    LS_CHECK(!ble_link_observer_push(&ring,mac,0,0,false,data,sizeof(data)+1,0));
    LS_CHECK(!ble_link_observer_push(&ring,mac,0,0,false,NULL,1,0));
    LS_CHECK(ring.dropped==3);
    LS_EQ_UINT(ring.overflow,1);LS_EQ_UINT(ring.invalid,2);
    LS_EQ_UINT(ring.dispatched,BLE_LINK_AD_DEPTH+1);
    LS_EQ_UINT(ring.received,BLE_LINK_AD_DEPTH+4);
    LS_EQ_UINT(ring.high_water,BLE_LINK_AD_DEPTH);
}
LS_CASE(two_hundred_device_burst_fits_observer_ring_without_loss)
{
    memset(&ring,0,sizeof(ring));calls=0;ble_link_observer_subscribe(&ring,capture,&calls);
    uint8_t a[6]={0},data[]={2,1,6};
    for(int i=0;i<200;i++) {a[0]=i;LS_CHECK(ble_link_observer_push(&ring,a,0,-60,false,data,sizeof(data),1000000+i));}
    LS_EQ_UINT(ring.dropped,0);LS_EQ_UINT(ring.high_water,200);
    LS_EQ_UINT(ble_link_observer_dispatch(&ring,BLE_LINK_AD_DEPTH),200);
    LS_EQ_UINT(calls,200);LS_EQ_UINT(ring.received,200);LS_EQ_UINT(ring.dispatched,200);
}

static void decode_rid(const ble_link_advert_t *a, void *ctx)
{
    (void)ctx;
    ls_rid_advert(a->addr,a->addr_type,a->rssi,a->data,a->len,a->seen_us);
}
LS_CASE(remote_id_reaches_decoder_from_copied_scan_response)
{
    memset(&ring,0,sizeof(ring)); ls_rid_clear();
    ble_link_observer_subscribe(&ring,decode_rid,NULL);
    uint8_t ad[31]={30,0x16,0xfa,0xff,0x0d,42,0x02,0x12};
    memcpy(ad+8,"P4-OBSERVER",11);
    LS_CHECK(ble_link_observer_push(&ring,mac,1,-88,true,ad,sizeof(ad),1000000));
    memset(ad,0,sizeof(ad));
    ls_rid_drone_t drone;
    LS_CHECK(ls_rid_snapshot(&drone,1,1000000)==0);
    LS_CHECK(ble_link_observer_dispatch(&ring,1)==1);
    LS_CHECK(ls_rid_snapshot(&drone,1,1000000)==1);
    LS_CHECK(!strcmp(drone.uas_id,"P4-OBSERVER") && drone.rssi==-88);
    LS_CHECK(drone.address_type==1 && !memcmp(drone.mac,mac,6));
}

static unsigned stress_seen;
static bool stress_bad;
static void check_sequence(const ble_link_advert_t *a, void *ctx)
{
    (void)ctx;
    unsigned seq;
    memcpy(&seq,a->data,sizeof(seq));
    if(seq!=stress_seen || a->seen_us!=seq || a->len!=sizeof(seq) ||
        memcmp(a->addr,mac,6) || a->scan_response!=(bool)(seq&1)) stress_bad=true;
    stress_seen++;
}
static void *produce(void *arg)
{
    (void)arg;
    for(unsigned i=0;i<50000;i++)
        while(!ble_link_observer_push(&ring,mac,1,-70,i&1,(const uint8_t *)&i,sizeof(i),i)) {}
    return NULL;
}
LS_CASE(concurrent_producer_consumer_preserve_order_and_payload_publication)
{
    memset(&ring,0,sizeof(ring)); stress_seen=0;stress_bad=false;
    ble_link_observer_subscribe(&ring,check_sequence,NULL);
    pthread_t producer;
    int rc=pthread_create(&producer,NULL,produce,NULL);
    LS_CHECK(rc==0);if(rc) return;
    while(stress_seen<50000) ble_link_observer_dispatch(&ring,7);
    LS_CHECK(pthread_join(producer,NULL)==0 && !stress_bad);
    LS_CHECK(ring.read==ring.write && stress_seen==50000);
}

LS_CASE(listener_registration_is_bounded_and_idempotent)
{
    memset(&ring,0,sizeof(ring));unsigned counts[BLE_LINK_AD_LISTENERS+1]={0};
    LS_CHECK(!ble_link_observer_subscribe(&ring,NULL,NULL));
    for(unsigned i=0;i<BLE_LINK_AD_LISTENERS;i++)
        LS_CHECK(ble_link_observer_subscribe(&ring,capture,&counts[i]));
    LS_CHECK(!ble_link_observer_subscribe(&ring,capture,&counts[BLE_LINK_AD_LISTENERS]));
    LS_CHECK(ble_link_observer_subscribe(&ring,capture,&counts[0]));
    LS_CHECK(ble_link_observer_push(&ring,mac,0,0,false,NULL,0,0));
    LS_CHECK(ble_link_observer_dispatch(&ring,0)==0);
    LS_CHECK(ble_link_observer_dispatch(&ring,1)==1);
    for(unsigned i=0;i<BLE_LINK_AD_LISTENERS;i++) LS_CHECK(counts[i]==1);
}
