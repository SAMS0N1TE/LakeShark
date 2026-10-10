#include "ls_test.h"
#include "ls_sweep_app.h"
#include "ls_sweep_scope.h"
#include "ls_rid.h"
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include "esp_timer.h"
static ls_sweep_device_t rows[LS_SWEEP_CAP];
static const uint8_t mac[6]={1,2,3,4,5,6};
static void reset(void) {
    ls_sweep_clear();ls_sweep_defaults();ls_rid_clear();
    ls_sweep_settings_t s={.version=1,.enabled=15,.receive_only=true};LS_CHECK(ls_sweep_settings_set(&s));ls_sweep_start(true);
}
LS_CASE(crowded_home_keeps_thirteen_find_my_and_tile_visible) {
    reset();uint8_t ad[31]={30,0xff,0x4c,0,0x12,0x19};
    uint8_t tile[]={3,0xff,0x7c,6},a[6]={0};
    for(int pass=0;pass<40;pass++) {
        int64_t now=1000000+(int64_t)pass*2000000;
        for(int i=0;i<14;i++) {
            a[0]=i;ls_sweep_ble(a,0,-60,i==13?tile:ad,i==13?sizeof(tile):sizeof(ad),now+i*1000);
        }
        LS_EQ_UINT(ls_sweep_snapshot(rows,LS_SWEEP_CAP,now+14000),14);
    }
}
LS_CASE(two_hundred_trackers_survive_dense_churn) {
    reset();ls_sweep_match_t m={.category=SW_TRACKER};uint8_t a[6]={0};
    for(int pass=0;pass<3;pass++) {
        for(int i=0;i<200;i++) {a[0]=i;ls_sweep_observe(a,0,0,-60,&m,1000000+pass*2000000+i*1000);}
        LS_EQ_UINT(ls_sweep_snapshot(rows,LS_SWEEP_CAP,1200000+pass*2000000),200);
    }
}
LS_CASE(two_hundred_sparse_contacts_expire_at_sixty_seconds) {
    reset();ls_sweep_stats_t before,after;ls_sweep_stats(&before,1000000);
    ls_sweep_match_t m={.category=SW_CAMERA};uint8_t a[6]={0};
    for(int i=0;i<200;i++) {a[0]=i;ls_sweep_observe(a,0,0,-65,&m,1000000);}
    LS_EQ_UINT(ls_sweep_snapshot(rows,LS_SWEEP_CAP,31000000),200);
    uint32_t ids[200];for(int i=0;i<200;i++) ids[rows[i].mac[0]]=rows[i].serial;
    /* Repeat advertisements after missed scan cycles preserve contact identity. */
    for(int i=0;i<200;i++) {a[0]=i;ls_sweep_observe(a,0,0,-55,&m,31000000);}
    LS_EQ_UINT(ls_sweep_snapshot(rows,LS_SWEEP_CAP,90999999),200);
    for(int i=0;i<200;i++) LS_EQ_UINT(rows[i].serial,ids[rows[i].mac[0]]);
    LS_EQ_UINT(ls_sweep_snapshot(rows,LS_SWEEP_CAP,91000000),0);
    ls_sweep_stats(&after,91000000);
    LS_EQ_UINT(after.expiries-before.expiries,200);
    LS_EQ_UINT(after.evicted_capacity-before.evicted_capacity,0);
    LS_EQ_UINT(after.accepted[SW_CAMERA]-before.accepted[SW_CAMERA],400);
}
LS_CASE(capacity_evicts_oldest_and_expired_slots_are_reused_first) {
    reset();ls_sweep_match_t m={.category=SW_CAMERA};ls_sweep_stats_t before,after;
    ls_sweep_stats(&before,1000000);
    for(unsigned i=0;i<LS_SWEEP_CAP+44;i++) {
        uint8_t a[6]={i&255,i>>8};ls_sweep_observe(a,0,0,-50,&m,1000000+i);
    }
    LS_EQ_UINT(ls_sweep_snapshot(rows,LS_SWEEP_CAP,1001000),LS_SWEEP_CAP);
    for(unsigned i=0;i<LS_SWEEP_CAP;i++) LS_CHECK((rows[i].mac[0]+256u*rows[i].mac[1])>=44);
    ls_sweep_stats(&after,1001000);LS_EQ_UINT(after.evicted_capacity-before.evicted_capacity,44);
    /* Expire on insertion, without a status/snapshot tick first. */
    uint8_t fresh[6]={0xff,0xff};ls_sweep_observe(fresh,0,0,-50,&m,302000000);
    ls_sweep_stats(&after,302000000);LS_EQ_UINT(after.table_size,1);
    LS_EQ_UINT(after.evicted_capacity-before.evicted_capacity,44);
    LS_EQ_UINT(after.expiries-before.expiries,LS_SWEEP_CAP);
}
LS_CASE(stats_count_all_ble_categories_rotation_and_unique_window) {
    reset();ls_sweep_stats_t before,after;const int64_t now=INT64_C(1000000000000);
    ls_sweep_stats(&before,now);
    uint8_t ad[31]={30,0xff,0x4c,0,0x12,0x19},a[6]={0};
    for(int i=0;i<200;i++) {a[0]=i;ls_sweep_ble(a,0,-55,ad,sizeof(ad),now);}
    for(int i=0;i<200;i++) {a[0]=i;ls_sweep_ble(a,0,-55,ad,sizeof(ad),now+2000000);}
    a[0]=201;uint8_t flags[]={2,1,6};ls_sweep_ble(a,0,-60,flags,sizeof(flags),now+2000000);
    ls_sweep_stats(&after,now+2000000);
    LS_EQ_UINT(after.adverts_in-before.adverts_in,401);
    LS_EQ_UINT(after.accepted[SW_TRACKER]-before.accepted[SW_TRACKER],400);
    LS_EQ_UINT(after.table_size,200);LS_EQ_UINT(after.unique_addresses_min,201);
    /* Equal payloads at different addresses stay separate; zero keys and
     * nearby-owner payloads cannot honestly establish physical identity. */
    ls_sweep_match_t m={.category=SW_BODYCAM};a[0]=0;
    ls_sweep_observe(a,0,0,-60,&m,now+3000000);
    ls_sweep_stats(&after,now+3000000);LS_EQ_UINT(after.category_changes-before.category_changes,1);
    ls_sweep_stats(&after,now+62000000);LS_EQ_UINT(after.unique_addresses_min,0);
    ls_shim_time_set(now+3000000);char *args[]={"sweep","stats"};LS_CHECK(ls_sweep_command(2,args)==0);
}
LS_CASE(unique_address_window_reports_saturation_honestly) {
    reset();ls_sweep_stats_t before,after;const int64_t now=INT64_C(2000000000000);
    ls_sweep_stats(&before,now);uint8_t flags[]={2,1,6};
    for(unsigned i=0;i<1100;i++) {
        uint8_t a[6]={i&255,i>>8,0xa5};ls_sweep_ble(a,0,-60,flags,sizeof(flags),now);
    }
    ls_sweep_stats(&after,now);LS_EQ_UINT(after.unique_addresses_min,1024);
    LS_EQ_UINT(after.unique_saturated-before.unique_saturated,76);
    ls_sweep_stats(&after,now+60000000);LS_EQ_UINT(after.unique_addresses_min,0);
}
LS_CASE(find_my_requires_exact_payload_not_apple_company) {
    reset();ls_sweep_match_t m;
    uint8_t ad[31]={30,0xff,0x4c,0,0x12,0x19};
    LS_CHECK(ls_sweep_parse_ble(ad,31,&m) && m.category==SW_TRACKER && !m.immediate_alert);
    for(size_t n=0;n<31;n++) LS_CHECK(!ls_sweep_parse_ble(ad,n,&m));
    ad[4]=2;LS_CHECK(!ls_sweep_parse_ble(ad,31,&m));ad[4]=0x12;ad[5]=0x18;LS_CHECK(!ls_sweep_parse_ble(ad,31,&m));
    uint8_t nearby[]={7,0xff,0x4c,0,0x12,2,0x20,0};LS_CHECK(ls_sweep_parse_ble(nearby,sizeof(nearby),&m));
    uint8_t company[]={3,0xff,0x4c,0};LS_CHECK(!ls_sweep_parse_ble(company,4,&m));
}
LS_CASE(fmdn_supports_both_curves_and_rejects_eddystone) {
    reset();ls_sweep_match_t m;
    for(int curve=0;curve<2;curve++) for(int separated=0;separated<2;separated++) {
        uint8_t ad[38]={0,0x16,0xaa,0xfe,(uint8_t)(0x40+separated)};size_t n=25+curve*12+separated;ad[0]=n-1;
        LS_CHECK(ls_sweep_parse_ble(ad,n,&m) && m.category==SW_TRACKER);
        ad[4]=0x10;LS_CHECK(!ls_sweep_parse_ble(ad,n,&m));ad[4]=0x40+separated;
        LS_CHECK(!ls_sweep_parse_ble(ad,n-1,&m));
    }
    uint8_t fastpair[]={6,0x16,0x2c,0xfe,1,2,3};LS_CHECK(!ls_sweep_parse_ble(fastpair,sizeof(fastpair),&m));
}
LS_CASE(smarttag_requires_company_and_name_tile_uses_registry) {
    reset();ls_sweep_match_t m;
    uint8_t ad[]={3,0xff,0x75,0,9,9,'S','m','a','r','t','T','a','g'};
    LS_CHECK(ls_sweep_parse_ble(ad,sizeof(ad),&m) && m.category==SW_TRACKER && strstr(m.label,"SmartTag"));
    LS_CHECK(!ls_sweep_parse_ble(ad,4,&m));ad[2]=0x76;LS_CHECK(!ls_sweep_parse_ble(ad,sizeof(ad),&m));
    uint8_t tile[]={5,3,0xed,0xfe,2,0x18};LS_CHECK(ls_sweep_parse_ble(tile,sizeof(tile),&m) && m.immediate_alert && strstr(m.label,"Tile"));
    uint8_t tc[]={3,0xff,0x7c,6};LS_CHECK(ls_sweep_parse_ble(tc,4,&m) && m.category==SW_TRACKER);
    tile[0]=4;LS_CHECK(!ls_sweep_parse_ble(tile,5,&m));
}
LS_CASE(rid_uses_existing_validated_receiver) {
    reset();uint8_t ad[31]={30,0x16,0xfa,0xff,0x0d,0,2,0x12};memcpy(ad+8,"CLEANROOM-RID",13);
    ls_sweep_ble(mac,0,-48,ad,31,1000000);LS_CHECK(ls_sweep_snapshot(rows,64,1000000)==1 && rows[0].match.category==SW_DRONE);
    ad[6]=0xe2;ls_sweep_clear();ls_sweep_ble(mac,0,-48,ad,31,2000000);LS_CHECK(ls_sweep_snapshot(rows,64,2000000)==0);
}
LS_CASE(rules_are_atomic_strict_and_match_each_selector) {
    reset();ls_sweep_match_t m;uint8_t wifi[]={0,0x40,0x8c,1,2,3};
    LS_CHECK(ls_sweep_parse_wifi(wifi,"hidden",&m) && m.category==SW_CAMERA);wifi[0]=2;LS_CHECK(!ls_sweep_parse_wifi(wifi,"hidden",&m));
    const char *bad[]={"[{","[{},]","[{\"category\":\"NOPE\",\"label\":\"x\",\"oui\":\"123456\"}]","[{\"category\":\"CAMERA\",\"label\":\"x\",\"oui\":\"12345Z\"}]","[{\"category\":\"CAMERA\",\"label\":\"x\",\"name\":\"a\",\"ssid\":\"b\"}]","[] garbage","[{\"category\":\"CAMERA\",\"category\":\"CAMERA\",\"label\":\"x\",\"name\":\"a\"}]"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++) LS_CHECK(!ls_sweep_rules_parse(bad[i],strlen(bad[i])));
    wifi[0]=0;LS_CHECK(ls_sweep_parse_wifi(wifi,"hidden",&m));
    const char *json="[{\"category\":\"BODYCAM\",\"label\":\"Custom camera\",\"uuid\":\"12345678-1234-5678-9abc-123456789abc\"},{\"category\":\"CAMERA\",\"label\":\"Test SSID\",\"ssid\":\"cam\"},{\"category\":\"BODYCAM\",\"label\":\"Test name\",\"name\":\"glasses\"}]";
    LS_CHECK(ls_sweep_rules_parse(json,strlen(json)));uint8_t uuid[]={17,7,0xbc,0x9a,0x78,0x56,0x34,0x12,0xbc,0x9a,0x78,0x56,0x34,0x12,0x78,0x56,0x34,0x12};
    LS_CHECK(ls_sweep_parse_ble(uuid,sizeof(uuid),&m) && !strcmp(m.label,"Custom camera"));
    uint8_t name[]={8,9,'G','L','A','S','S','E','S'};LS_CHECK(ls_sweep_parse_ble(name,sizeof(name),&m));
    LS_CHECK(ls_sweep_parse_wifi(wifi,"MyCAMera",&m));
    LS_CHECK(ls_sweep_rules_parse("[]",2));LS_CHECK(!ls_sweep_parse_wifi(wifi,"MyCAMera",&m));
}
LS_CASE(flood_is_distinct_quiets_audio_preserves_contacts_and_recovers) {
    reset();ls_sweep_match_t m={.category=SW_TRACKER,.label="Test tracker"};uint8_t a[6]={0};
    for(int i=0;i<40;i++) ls_sweep_observe(mac,0,0,-60,&m,1000000+i*1000);
    ls_sweep_status_t st;ls_sweep_status(&st,1040000);LS_CHECK(!st.flood);
    unsigned sounds=0;
    for(int i=0;i<200;i++) {a[0]=i;a[1]=i>>8;int64_t t=2000000+i*10000;ls_sweep_observe(a,0,0,-65,&m,t);if(ls_sweep_alert(t)>=0) sounds++;}
    LS_CHECK(sounds<=3);LS_CHECK(ls_sweep_snapshot(rows,LS_SWEEP_CAP,4000000)==201);
    LS_CHECK(ls_sweep_alert(4000000)==-1);
    /* A real tag continues transmitting after spoofers stop. Those repeated
     * packets must not perpetually extend the storm's quiet period. */
    for(int i=0;i<24;i++) ls_sweep_observe(mac,0,0,-55,&m,4100000+i*250000);
    ls_sweep_observe(mac,0,0,-55,&m,10000000);ls_sweep_status(&st,10000000);LS_CHECK(!st.flood);
    LS_CHECK(ls_sweep_alert(10000000)==SW_TRACKER);
}
LS_CASE(attack_burst_is_opt_in_collapsed_and_rate_limited) {
    reset();uint8_t ad[]={2,1,6},a[6]={0};
    for(int i=0;i<300;i++) {a[0]=i;ls_sweep_ble(a,0,-60,ad,3,1000000+i);}
    LS_CHECK(ls_sweep_snapshot(rows,64,1100000)==0);
    ls_sweep_settings_t s;ls_sweep_settings_get(&s);s.enabled|=1<<SW_ATTACK;ls_sweep_settings_set(&s);
    for(int i=0;i<300;i++) {a[0]=i;ls_sweep_ble(a,0,-60,ad,3,2000000+i);}
    LS_CHECK(ls_sweep_snapshot(rows,64,2100000)==1 && rows[0].match.category==SW_ATTACK);
    LS_CHECK(ls_sweep_alert(2100000)==SW_ATTACK);LS_CHECK(ls_sweep_alert(2900000)==-1);
}
LS_CASE(mute_attack_opt_in_expiry_capacity_and_hunt) {
    reset();ls_sweep_match_t m={.category=SW_ATTACK,.label="Test attack"};
    ls_sweep_observe(mac,0,0,-60,&m,1000000);LS_CHECK(ls_sweep_snapshot(rows,64,1000000)==0);
    m.category=SW_CAMERA;ls_sweep_settings_t s;ls_sweep_settings_get(&s);s.muted=1<<SW_CAMERA;LS_CHECK(ls_sweep_settings_set(&s));
    ls_sweep_observe(mac,0,0,-60,&m,1000000);LS_CHECK(ls_sweep_alert(1000000)==-1);LS_CHECK(ls_sweep_snapshot(rows,64,1000000)==1);
    s.muted=0;LS_CHECK(ls_sweep_settings_set(&s));LS_CHECK(ls_sweep_hunt(mac,0,0));
    LS_CHECK(ls_sweep_alert(3000000)==-1);LS_CHECK(ls_sweep_alert(3100000)==-1);
    ls_sweep_observe(mac,0,0,-35,&m,3200000);LS_CHECK(ls_sweep_alert(3500000)==SW_CAMERA);
    LS_CHECK(ls_sweep_alert(10000000)==-1);LS_CHECK(ls_sweep_snapshot(rows,64,63199999)==1);
    LS_CHECK(ls_sweep_snapshot(rows,64,303200000)==0);
    for(unsigned i=0;i<90;i++) {uint8_t a[6]={(uint8_t)i};ls_sweep_observe(a,1,0,-50,&m,65000000+i);}
    LS_CHECK(ls_sweep_snapshot(rows,64,65001000)==64);ls_sweep_start(false);LS_CHECK(ls_sweep_snapshot(rows,64,65001000)==0);
}
static void *scan_thread(void *arg) {for(int i=0;i<1000;i++) {uint8_t ad[]={3,3,0xed,0xfe};ls_sweep_ble(mac,0,-50,ad,4,1000000+i);}return NULL;}
LS_CASE(concurrent_scan_snapshot_and_random_lengths) {
    reset();pthread_t t;pthread_create(&t,NULL,scan_thread,NULL);for(int i=0;i<1000;i++) {ls_sweep_snapshot(rows,64,1000000+i);ls_sweep_defaults();}pthread_join(t,NULL);
    uint8_t ad[80];uint32_t seed=1;ls_sweep_match_t m;
    for(int i=0;i<10000;i++) {for(unsigned j=0;j<80;j++) {seed=seed*1664525+1013904223;ad[j]=seed>>24;}ls_sweep_parse_ble(ad,i%80,&m);}
}
LS_CASE(console_hunt_uses_printed_mac_and_mute_is_idempotent) {
    reset();uint8_t ad[]={3,3,0xed,0xfe};ls_sweep_ble(mac,1,-55,ad,4,1000000);
    char *hunt[]={"sweep","hunt","06:05:04:03:02:01"};LS_CHECK(ls_sweep_command(3,hunt)==0);
    ls_sweep_settings_t s;ls_sweep_settings_get(&s);LS_CHECK(s.hunt && s.hunt_radio==0 && s.hunt_type==1 && !memcmp(s.hunt_mac,mac,6));
    char *mute[]={"sweep","mute","tracker"};LS_CHECK(ls_sweep_command(3,mute)==0);LS_CHECK(ls_sweep_command(3,mute)==0);
    ls_sweep_settings_get(&s);LS_CHECK(s.muted&(1<<SW_TRACKER));
    char *unmute[]={"sweep","unmute","TRACKER"};LS_CHECK(ls_sweep_command(3,unmute)==0);ls_sweep_settings_get(&s);LS_CHECK(!(s.muted&(1<<SW_TRACKER)));
    char *unknown[]={"sweep","hunt","00:00:00:00:00:00"};LS_CHECK(ls_sweep_command(3,unknown)==1);
    char *off[]={"sweep","hunt","off"};LS_CHECK(ls_sweep_command(3,off)==0);ls_sweep_settings_get(&s);LS_CHECK(!s.hunt);
    char *stop[]={"sweep","off"};LS_CHECK(ls_sweep_command(2,stop)==0);LS_CHECK(!ls_sweep_snapshot(rows,64,1000000));
}


LS_CASE(scope_persistence_boundaries_and_real_refresh) {
    LS_CHECK(ls_sweep_scope_level(-1)==0);
    LS_CHECK(ls_sweep_scope_level(11249999)==0);
    LS_CHECK(ls_sweep_scope_level(11250000)==1);
    LS_CHECK(ls_sweep_scope_level(22500000)==2);
    LS_CHECK(ls_sweep_scope_level(33750000)==3);
    LS_CHECK(ls_sweep_scope_level(44999999)==3);
    LS_CHECK(ls_sweep_scope_level(45000000)==4);
    reset();ls_sweep_match_t m={.category=SW_CAMERA,.label="Camera"};
    ls_sweep_observe(mac,0,0,-70,&m,1000000);
    LS_CHECK(ls_sweep_snapshot(rows,64,45000000)==1);
    uint32_t serial=rows[0].serial;
    LS_CHECK(ls_sweep_scope_level(45000000-rows[0].seen_us)==3);
    ls_sweep_observe(mac,0,0,-65,&m,45000000);
    LS_CHECK(ls_sweep_snapshot(rows,64,45000000)==1 && rows[0].serial==serial);
    LS_CHECK(ls_sweep_scope_level(45000000-rows[0].seen_us)==0);
}
LS_CASE(scope_geometry_uses_identity_and_rssi_not_rank_or_clock) {
    ls_sweep_device_t a={.mac={1,2,3,4,5,6},.match={.category=SW_TRACKER},.rssi=-70,.serial=3};
    double angle=ls_sweep_scope_degrees(&a);
    LS_CHECK(angle>=8 && angle<=52);
    a.serial=99;a.seen_us=123456;a.samples=24;
    LS_CHECK(ls_sweep_scope_degrees(&a)==angle);
    static const int sectors[]={1,2,3,0,4};
    for(unsigned c=0;c<SW_CATS;c++) {
        a.match.category=c;double t=ls_sweep_scope_degrees(&a);
        LS_CHECK(t>=sectors[c]*60+8 && t<=sectors[c]*60+52);
    }
    LS_CHECK(ls_sweep_scope_radius(-50)>0.333 && ls_sweep_scope_radius(-50)<0.334);
    LS_CHECK(ls_sweep_scope_radius(-50)<ls_sweep_scope_radius(-70));
    LS_CHECK(ls_sweep_scope_radius(0)==.10 && ls_sweep_scope_radius(-127)==.96);
    a.match.category=SW_TRACKER;a.serial=3;char id[16];ls_sweep_track_id(&a,id,sizeof(id));
    LS_CHECK(!strcmp(id,"T03"));
}

LS_CASE(global_mute_quiets_all_categories_and_hunt_without_losing_contacts) {
    reset();ls_sweep_settings_t s;ls_sweep_settings_get(&s);s.enabled=31;s.muted=2;s.logging=true;
    LS_CHECK(ls_sweep_settings_set(&s));
    char *on[]={"sweep","mute","on"},*off[]={"sweep","mute","off"},*bad[]={"sweep","mute","maybe"};
    LS_EQ_INT(ls_sweep_command(3,on),0);
    ls_sweep_settings_get(&s);LS_CHECK(s.alerts_muted && s.enabled==31 && s.muted==2 && s.logging);
    for(unsigned c=0;c<SW_CATS;c++) {
        uint8_t a[6]={c+1,2,3,4,5,6};ls_sweep_match_t m={.category=c};
        ls_sweep_observe(a,0,0,-45,&m,10000000+c*100000);
        LS_EQ_INT(ls_sweep_alert(11000000+c*100000),-1);
        LS_CHECK(ls_sweep_hunt(a,0,0));LS_EQ_INT(ls_sweep_alert(12000000+c*100000),-1);
    }
    LS_EQ_UINT(ls_sweep_snapshot(rows,LS_SWEEP_CAP,13000000),5);
    ls_sweep_device_t logged;LS_CHECK(ls_sweep_log_event(&logged));
    LS_EQ_INT(ls_sweep_command(3,bad),1);LS_EQ_INT(ls_sweep_command(3,off),0);
    ls_sweep_settings_get(&s);LS_CHECK(!s.alerts_muted && s.muted==2 && s.hunt && s.logging);
    LS_EQ_INT(ls_sweep_alert(13000000),-1);
    LS_CHECK(ls_sweep_hunt(NULL,0,0));LS_EQ_INT(ls_sweep_alert(14000000),-1);
    uint8_t fresh[6]={99};ls_sweep_match_t m={.category=SW_CAMERA};
    ls_sweep_observe(fresh,0,0,-45,&m,15000000);LS_EQ_INT(ls_sweep_alert(15000000),SW_CAMERA);
}

LS_CASE(owner_states_and_owner_nearby_never_feed_flood) {
    reset();ls_sweep_match_t m;ls_sweep_status_t st;
    uint8_t apple[31]={30,0xff,0x4c,0,0x12,0x19};
    LS_CHECK(ls_sweep_parse_ble(apple,31,&m) && m.state==SW_STATE_SEPARATED);
    apple[6]=4;LS_CHECK(ls_sweep_parse_ble(apple,31,&m) && m.state==SW_STATE_NEAR);
    uint8_t dult[]={5,0x16,0xb2,0xfc,1,1};
    LS_CHECK(ls_sweep_parse_ble(dult,sizeof(dult),&m) && m.state==SW_STATE_NEAR);
    dult[5]=0;LS_CHECK(ls_sweep_parse_ble(dult,sizeof(dult),&m) && m.state==SW_STATE_SEPARATED);
    uint8_t google[26]={25,0x16,0xaa,0xfe,0x40};
    LS_CHECK(ls_sweep_parse_ble(google,sizeof(google),&m) && m.state==SW_STATE_NEAR);
    google[4]=0x41;LS_CHECK(ls_sweep_parse_ble(google,sizeof(google),&m) && m.state==SW_STATE_SEPARATED);
    m.state=SW_STATE_NEAR;
    for(int i=0;i<40;i++) {uint8_t a[6]={i};ls_sweep_observe(a,0,1,-55,&m,1000000+i);}
    ls_sweep_status(&st,1100000);LS_CHECK(!st.flood && st.owner_nearby==40);
    LS_EQ_UINT(ls_sweep_snapshot(rows,LS_SWEEP_CAP,1100000),0);
    ls_sweep_status(&st,62000000);LS_EQ_UINT(st.owner_nearby,0);
}
LS_CASE(with_you_time_gps_movement_and_retention) {
    reset();ls_sweep_match_t m={.category=SW_TRACKER,.state=SW_STATE_SEPARATED};
    ls_sweep_motion(true,0,0,1000000);
    for(int i=0;i<=20;i++) ls_sweep_observe(mac,0,1,-60,&m,1000000+i*30000000LL);
    LS_EQ_UINT(ls_sweep_snapshot(rows,LS_SWEEP_CAP,601000000),1);
    LS_CHECK(ls_sweep_section(&rows[0],601000000,true)==SW_PASSING);
    ls_sweep_motion(true,499,0,601000000);ls_sweep_snapshot(rows,LS_SWEEP_CAP,601000000);LS_CHECK(!rows[0].with_you);
    ls_sweep_motion(true,1,0,601000000);ls_sweep_snapshot(rows,LS_SWEEP_CAP,601000000);LS_CHECK(rows[0].with_you && ls_sweep_alarm(&rows[0],true));
    LS_EQ_UINT(ls_sweep_snapshot(rows,LS_SWEEP_CAP,1500999999),1);
    LS_EQ_UINT(ls_sweep_snapshot(rows,LS_SWEEP_CAP,1501000000),0);
    reset();ls_sweep_motion(false,0,0,1000000);
    for(int i=0;i<=20;i++) ls_sweep_observe(mac,0,1,-60,&m,1000000+i*30000000LL);
    ls_sweep_snapshot(rows,LS_SWEEP_CAP,601000000);LS_CHECK(rows[0].with_you && !ls_sweep_alarm(&rows[0],false));
    reset();ls_sweep_motion(true,0,0,1000000);
    for(int i=0;i<=20;i++) ls_sweep_observe(mac,0,1,-60,&m,1000000+i*30000000LL);
    ls_sweep_motion(true,0,3,601000000);ls_sweep_snapshot(rows,LS_SWEEP_CAP,601000000);LS_CHECK(rows[0].with_you);
}
LS_CASE(history_buckets_gaps_median_ema_trend_and_advert_clicks) {
    reset();ls_sweep_match_t m={.category=SW_TRACKER,.state=SW_STATE_SEPARATED};
    for(int i=0;i<5;i++) ls_sweep_observe(mac,0,1,-80,&m,10000000+i*1000);
    ls_sweep_observe(mac,0,1,-20,&m,20000000);
    ls_sweep_snapshot(rows,LS_SWEEP_CAP,20000000);
    LS_EQ_INT(ls_sweep_bucket(&rows[0],20000000,0),-20);LS_EQ_INT(ls_sweep_bucket(&rows[0],20000000,1),-128);
    LS_CHECK(rows[0].fast==-80 && rows[0].slow==-80 && ls_sweep_trend(&rows[0],20000000)==60);
    LS_CHECK(ls_sweep_hunt_trend(&rows[0],20000000,false)==0);
    for(int i=0;i<3;i++) ls_sweep_observe(mac,0,1,-60,&m,21000000+i*1000000);
    ls_sweep_snapshot(rows,LS_SWEEP_CAP,23000000);LS_CHECK(rows[0].fast>rows[0].slow && rows[0].slow>=-80);
    LS_CHECK(ls_sweep_hunt(mac,0,1));LS_EQ_INT(ls_sweep_alert(23000000),-1);
    ls_sweep_observe(mac,0,1,-60,&m,24000000);LS_EQ_INT(ls_sweep_alert(24000000),SW_TRACKER);LS_EQ_INT(ls_sweep_alert(25000000),-1);
    ls_sweep_settings_t s;ls_sweep_settings_get(&s);s.alerts_muted=true;ls_sweep_settings_set(&s);
    ls_sweep_observe(mac,0,1,-55,&m,26000000);LS_EQ_INT(ls_sweep_alert(26000000),-1);
    s.alerts_muted=false;ls_sweep_settings_set(&s);LS_EQ_INT(ls_sweep_alert(27000000),-1);
    LS_EQ_INT(ls_sweep_bucket(&rows[0],83000000,0),-128);
}

LS_CASE(owner_nearby_selection_warns_without_tracking_or_timer_clicks) {
    reset();ls_sweep_match_t m={.category=SW_TRACKER,.state=SW_STATE_NEAR,.label="DULT tag"};
    ls_sweep_observe(mac,0,1,-60,&m,1000000);
    ls_sweep_device_t target;LS_CHECK(ls_sweep_nearby_target(&target,1000000));LS_CHECK(target.match.state==SW_STATE_NEAR);
    LS_CHECK(ls_sweep_hunt(target.mac,target.radio,target.address_type));LS_EQ_INT(ls_sweep_alert(2000000),-1);
    uint8_t other[6]={99};ls_sweep_observe(other,0,1,-60,&m,2100000);
    ls_sweep_observe(mac,0,1,-50,&m,2200000);LS_EQ_INT(ls_sweep_alert(2200000),SW_TRACKER);LS_EQ_INT(ls_sweep_alert(3000000),-1);
    LS_EQ_UINT(ls_sweep_snapshot(rows,LS_SWEEP_CAP,3000000),0);
}

LS_CASE(delayed_adverts_do_not_move_quiet_clock_backwards) {
    reset();const uint8_t flags[]={2,1,6};
    ls_sweep_ble(mac,0,-80,flags,sizeof(flags),10000000);
    ls_sweep_ble(mac,0,-80,flags,sizeof(flags),2000000);
    ls_sweep_status_t st;ls_sweep_status(&st,10000000);LS_CHECK(st.last_advert_us>=10000000);
}

LS_CASE(link_summary_matches_filtered_snapshot_and_expires_without_heap) {
    reset();ls_sweep_runtime_status(true,"running");
    for(unsigned i=0;i<5;i++) {
        uint8_t a[6]={20+i};ls_sweep_match_t m={.category=i};
        ls_sweep_observe(a,0,0,-40-(int)i,&m,1000000);
    }
    ls_sweep_settings_t s;ls_sweep_settings_get(&s);s.enabled=31;LS_CHECK(ls_sweep_settings_set(&s));
    /* Attack was disabled during intake, so four entries are retained. */
    ls_sweep_summary_t summary;ls_sweep_summary(&summary,2000000);
    size_t n=ls_sweep_snapshot(rows,LS_SWEEP_CAP,2000000);unsigned counts[SW_CATS]={0};
    for(size_t i=0;i<n;i++) counts[rows[i].match.category]++;
    LS_CHECK(!memcmp(counts,summary.counts,sizeof(counts)) && summary.running);
    LS_CHECK(summary.serial[0]==rows[0].serial && summary.rssi[0]==-40 && summary.rssi[1]==-41);
    s.filter=4;s.alerts_muted=true;LS_CHECK(ls_sweep_settings_set(&s));ls_sweep_summary(&summary,2000000);
    LS_CHECK(summary.counts[SW_TRACKER]==1 && !summary.counts[SW_CAMERA] && summary.muted);
    LS_CHECK(summary.category[0]==SW_TRACKER && !summary.serial[1]);
    ls_sweep_summary(&summary,61000000);LS_CHECK(!summary.serial[0] && !summary.counts[SW_TRACKER]);
}
