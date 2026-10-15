#include "ls_test.h"
#include "ls_detector.h"
#include "ls_sweep_app.h"
#include "ls_rid.h"
#include "ls_notify.h"
#include "ls_options.h"
#include "ls_detector_ui.h"
#include "ls_radio_panel.h"
#include "ls_haptic.h"
#include "esp_timer.h"
#include "ls_gps.h"
#include "apps/adsb/adsb_app.h"
#include "apps/adsb/adsb_state.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
static const ls_opt_ctx_t *opened_options;
/* Geometry assertions here; production raster helpers are exercised in lssim. */
void ls_freq_key_draw(tui_surface *sf,tui_rect r,const char *label,char badge,uint8_t hue,bool strong) {
    (void)badge;(void)strong;LS_CHECK(r.w>=8 && r.h>=3);
    tui_fill(sf,r,' ',TUI_ATTR(hue,TUI_BLACK));tui_put_str(sf,r,r.x,r.y+1,label,TUI_ATTR(hue,TUI_BLACK));
}
void ls_radio_bezel(tui_surface *sf,tui_rect r,const char *title) {tui_box(sf,r,title,TUI_ATTR(TUI_CYAN,TUI_BLACK));}
int ls_radio_chip(tui_surface *sf,tui_rect r,int x,int y,const char *t,bool on,uint8_t hue) {(void)on;tui_put_str(sf,r,x,y,t,TUI_ATTR(hue,TUI_BLACK));return x+(int)strlen(t)+3;}
void ls_radio_digits_draw(tui_surface *sf,tui_rect r,int row,const char *t,uint8_t hue) {tui_put_str(sf,r,r.x,r.y+row,t,hue);}
void ls_radio_bargraph(tui_surface *sf,tui_rect r,int x,int y,int w,int rows,float f,float peak) {(void)sf;(void)r;(void)x;(void)y;(void)w;(void)rows;(void)f;(void)peak;}

void ls_sweep_wake(void) {}
/* The UI and rule tests here run with no ADS-B receiver and no GPS. */
static adsb_source_t air_source=ADSB_SRC_NONE;
adsb_source_t adsb_active_source(void) {return air_source;}
void ls_gps_get(ls_gps_state_t *out) {memset(out,0,sizeof(*out));}
void ls_sweep_transport_stats(ls_sweep_transport_stats_t *s) {memset(s,0,sizeof(*s));}
bool ls_tui_cutout(tui_rect *r) {*r=tui_rect_make(0,0,0,0);return false;}
bool ls_tui_keyboard_mode(void) {return false;}
static int corner_pad;
int ls_tui_corner_pad(int row) {(void)row;return corner_pad;}
int ls_tui_screen_count(void) {return 1;}
const char *ls_tui_screen_name(int index) {(void)index;return "DETECTOR";}
void ls_tui_screen_show(int index) {(void)index;}
ls_btn_t ls_opt_button(const ls_opt_ctx_t *ctx) {(void)ctx;return (ls_btn_t){.label="OPTIONS",.key='o'};}
bool ls_opt_key(const ls_opt_ctx_t *ctx,char ch) {if(ch!='o' && ch!='O')return false;opened_options=ctx;return true;}
static unsigned rings,vibes,invalidations;
static bool motor=true;
void ls_tui_invalidate(void) {invalidations++;}
void ls_notify_alert_hw(bool ring,bool vibe) {rings+=ring;vibes+=vibe;}
bool ls_haptic_present(void) {return motor;}
bool ls_haptic_play(ls_haptic_effect_t e) {LS_CHECK(e==LS_HAPTIC_ALERT);vibes++;return motor;}
extern void ls_detector_banner_poll(void);
static const char *one="{\"version\":1,\"rules\":[{\"id\":\"tag\",\"enabled\":true,\"sources\":[\"sweep.ble\"],\"match\":{\"category_in\":[\"TRACKER\"]},\"sinks\":[\"banner\",\"vibrate\",\"sd\"],\"rule_cooldown_s\":0,\"cooldown_s\":60}]}";
static int64_t clock_us=100000000;
static void begin(const char *json) {
    ls_detector_start(false);ls_detector_policy(7,DET_BANNER|DET_VIBRATE);
    for(int i=0;i<4;i++)ls_detector_banner_poll();
    clock_us+=1000000000;ls_shim_time_set(clock_us);
    ls_detector_start(true);ls_detector_tick();LS_CHECK(ls_detector_parse(json,strlen(json)));
    rings=vibes=invalidations=0;
}
static void observe(unsigned entity,bool flood,uint8_t source,unsigned cat,bool valid) {
    uint8_t mac[6]={0,0x40,0x8c,(uint8_t)(entity>>16),(uint8_t)(entity>>8),(uint8_t)entity};
    ls_detector_observe(mac,0,source,cat,-47,valid,flood,SW_WITH_YOU,clock_us);
}
static ls_detector_status_t status(void) {ls_detector_status_t s;ls_detector_status(&s);return s;}
LS_CASE(detector_strict_parse_and_atomic_swap) {
    begin(one);unsigned revision=status().revision;
    const char *bad[]={"{}","{\"version\":2,\"rules\":[]}","{\"version\":1,\"version\":1,\"rules\":[]}",
        "{\"version\":1,\"rules\":[],\"unknown\":false}","{\"version\":01,\"rules\":[]}",
        "{\"version\":1,\"rules\":[{\"id\":\"x\",\"sources\":[\"adsb\"],\"match\":{\"validated_message\":true},\"sinks\":[\"banner\"]}]}"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++) {LS_CHECK(!ls_detector_parse(bad[i],strlen(bad[i])));LS_CHECK(status().revision==revision && status().rules==1 && status().error[0]);}
    const char *rejected[]={"\"dedupe\":\"none\"","\"cooldown_s\":-1","\"cooldown_s\":86401","\"cooldown_s\":1.2","\"enabled\":1","\"enabled\":true,\"enabled\":false","\"any\":[]"};
    for(unsigned i=0;i<sizeof(rejected)/sizeof(*rejected);i++) {
        char json[700];snprintf(json,sizeof(json),"{\"version\":1,\"rules\":[{\"id\":\"x\",\"sources\":[\"rid.ble\"],\"match\":{\"validated_message\":true},\"sinks\":[\"banner\"],%s}]}",rejected[i]);
        LS_CHECK(!ls_detector_parse(json,strlen(json)));LS_CHECK(status().revision==revision);
    }
    char dup[1200];snprintf(dup,sizeof(dup),"{\"version\":1,\"rules\":[%s,%s]}","{\"id\":\"x\",\"sources\":[\"rid.ble\"],\"match\":{\"validated_message\":true},\"sinks\":[\"banner\"]}","{\"id\":\"x\",\"sources\":[\"rid.ble\"],\"match\":{\"validated_message\":true},\"sinks\":[\"sd\"]}");
    LS_CHECK(!ls_detector_parse(dup,strlen(dup)));
    LS_CHECK(!ls_detector_parse(one,LS_DETECTOR_BYTES+1));
    observe(1,false,DET_BLE,SW_TRACKER,false);ls_detector_tick();LS_CHECK(vibes==1);
    const char *after="{\"rules\":[{\"id\":\"after\",\"sources\":[\"rid.ble\"],\"match\":{\"validated_message\":true},\"sinks\":[\"banner\"]}],\"defaults\":{\"enabled\":true,\"cooldown_s\":123},\"version\":1}";
    LS_CHECK(ls_detector_parse(after,strlen(after)));ls_detector_rule_t r;LS_CHECK(ls_detector_rule(0,&r) && r.enabled && r.cooldown_s==123);
    /* Every truncation is rejected, preserving that revision. */
    revision=status().revision;for(size_t n=0;n<strlen(one);n++)LS_CHECK(!ls_detector_parse(one,n));LS_CHECK(status().revision==revision);
}
LS_CASE(detector_matching_cooldown_dedupe_and_source_policy) {
    begin(one);unsigned matched=status().matches;
    observe(2,false,DET_WIFI,SW_TRACKER,false);observe(2,false,DET_BLE,SW_CAMERA,false);ls_detector_tick();LS_CHECK(status().matches==matched);
    observe(2,false,DET_BLE,SW_TRACKER,false);observe(2,false,DET_BLE,SW_TRACKER,false);ls_detector_tick();LS_CHECK(status().matches==matched+1 && vibes==1);
    clock_us+=59000000;ls_shim_time_set(clock_us);observe(2,false,DET_BLE,SW_TRACKER,false);ls_detector_tick();LS_CHECK(vibes==1);
    clock_us+=1000000;ls_shim_time_set(clock_us);observe(2,false,DET_BLE,SW_TRACKER,false);ls_detector_tick();LS_CHECK(vibes==2);
    ls_detector_policy(DET_RID,7);observe(3,false,DET_BLE,SW_TRACKER,false);ls_detector_tick();LS_CHECK(vibes==2);
    ls_detector_event_t e;LS_CHECK(ls_detector_history(&e,1)==1 && e.entity!=0 && e.selected==7 && !(e.delivered&DET_SD));
    LS_CHECK(!strcmp(e.rule_ids,"\"tag\""));
}
LS_CASE(detector_overlapping_rules_merge_and_banner_never_rings) {
    const char *json="{\"version\":1,\"defaults\":{\"enabled\":true,\"rule_cooldown_s\":0},\"rules\":[{\"id\":\"a\",\"sources\":[\"rid.ble\"],\"match\":{\"validated_message\":true},\"sinks\":[\"banner\"]},{\"id\":\"b\",\"sources\":[\"rid.ble\"],\"match\":{\"validated_message\":true},\"sinks\":[\"banner\",\"vibrate\"]}]}";
    begin(json);ls_notify_set_alerts(true,true);
    observe(4,false,DET_RID,SW_DRONE,false);ls_detector_tick();LS_CHECK(vibes==0);
    observe(4,false,DET_RID,SW_DRONE,true);ls_detector_tick();LS_CHECK(vibes==1 && rings==0);
    ls_detector_banner_poll();LS_CHECK(rings==0 && vibes==1 && invalidations==1);
    ls_detector_event_t e;ls_detector_history(&e,1);LS_CHECK(e.rules==3 && !strcmp(e.rule_ids,"\"a\",\"b\""));
    ls_detector_policy(7,DET_BANNER);observe(5,false,DET_RID,SW_DRONE,true);ls_detector_tick();ls_detector_banner_poll();LS_CHECK(vibes==1 && rings==0);
    motor=false;ls_detector_policy(7,DET_VIBRATE);observe(6,false,DET_RID,SW_DRONE,true);ls_detector_tick();ls_detector_history(&e,1);LS_CHECK(e.failed==DET_VIBRATE && !e.delivered);motor=true;
}
LS_CASE(detector_flood_coalescing_and_queue_bounds) {
    begin(one);unsigned drops=status().dropped,matched=status().matches;
    for(unsigned i=0;i<100;i++)observe(100+i,true,DET_BLE,SW_TRACKER,false);
    LS_CHECK(status().queued==64 && status().dropped==drops+36);ls_detector_tick();LS_CHECK(vibes==4 && status().matches==matched+64);
    clock_us+=5000000;ls_shim_time_set(clock_us);ls_detector_tick();LS_CHECK(vibes==4);
    observe(900,true,DET_BLE,SW_TRACKER,false);ls_detector_tick();
    ls_detector_event_t e;ls_detector_history(&e,1);LS_CHECK(e.coalesced==60);
    ls_detector_tick();LS_CHECK(vibes==5);
    ls_detector_start(false);observe(777,false,DET_BLE,SW_TRACKER,false);LS_CHECK(!status().queued);
}
LS_CASE(detector_shared_sweep_classifier_and_legacy_sound_ownership) {
    begin(one);ls_sweep_clear();ls_sweep_defaults();ls_sweep_start(true);
    ls_sweep_settings_t s;ls_sweep_settings_get(&s);s.alerts_muted=false;s.enabled=31;LS_CHECK(ls_sweep_settings_set(&s));
    uint8_t mac[6]={1,2,3,4,5,6};ls_sweep_match_t m={.category=SW_TRACKER};
    ls_sweep_observe(mac,0,0,-45,&m,clock_us);LS_CHECK(status().queued>0);LS_CHECK(ls_sweep_alert(clock_us)==-1);
    ls_detector_tick();LS_CHECK(vibes==1);ls_detector_start(false);
    clock_us+=31000000;ls_sweep_observe(mac,0,0,-45,&m,clock_us);LS_CHECK(ls_sweep_alert(clock_us)==SW_TRACKER);
    ls_sweep_start(false);
}
LS_CASE(detector_writer_rotation_and_partial_tail) {
    ls_detector_event_t e={.at=123,.entity=0xabcdef,.source=DET_RID,.category=SW_DRONE,.selected=DET_SD,.delivered=DET_SD};strcpy(e.rule_ids,"\"remote-id\"");
    for(int i=0;i<8;i++) {char name[256];snprintf(name,sizeof(name),LS_DETECTOR_DIR "/events/detector-%d.jsonl",i);remove(name);}
    LS_CHECK(ls_detector_write(&e,0x1234));
    FILE *f=fopen(LS_DETECTOR_DIR "/events/keep-me.txt","wb");LS_CHECK(f!=NULL);if(f){fputs("unrelated",f);fclose(f);}
    f=fopen(LS_DETECTOR_DIR "/events/detector-0.jsonl","ab");LS_CHECK(f!=NULL);if(f){fputs("{partial",f);fclose(f);}
    LS_CHECK(ls_detector_write(&e,0x1234));
    f=fopen(LS_DETECTOR_DIR "/events/detector-0.jsonl","rb");char buf[4097];size_t n=f?fread(buf,1,sizeof(buf)-1,f):0;if(f)fclose(f);buf[n]=0;LS_CHECK(n && buf[0]=='{' && !strstr(buf,"partial"));
    for(unsigned i=0;i<120;i++) {e.sequence=i;LS_CHECK(ls_detector_write(&e,0x1234));}
    for(int i=0;i<8;i++) {char name[256];struct stat st;snprintf(name,sizeof(name),LS_DETECTOR_DIR "/events/detector-%d.jsonl",i);LS_CHECK(!stat(name,&st) && st.st_size<=4096 && st.st_size>0);}
    struct stat st;LS_CHECK(!stat(LS_DETECTOR_DIR "/events/keep-me.txt",&st));
}

LS_CASE(detector_options_tree_and_session_controls) {
    begin(one);LS_CHECK(ls_scr_detector.key(LS_TK_CHAR,'o'));
    LS_CHECK(opened_options && opened_options->n==4 && !strcmp(opened_options->name,"DETECTOR"));
    for(int i=0;i<4;i++)LS_CHECK(opened_options->opt[i].kind==LS_OPT_MENU && opened_options->opt[i].sub);
    const ls_opt_ctx_t *sources=opened_options->opt[0].sub;
    sources->opt[1].set(&sources->opt[1],0);LS_CHECK(!(status().sources&DET_BLE));
    sources->opt[1].set(&sources->opt[1],1);LS_CHECK(status().sources&DET_BLE);
    const ls_opt_ctx_t *r=opened_options->opt[1].sub;
    r->opt[2].set_num(&r->opt[2],120);ls_detector_rule_t rule;LS_CHECK(ls_detector_rule(0,&rule) && rule.cooldown_s==120);
    r->opt[1].set(&r->opt[1],0);LS_CHECK(ls_detector_rule(0,&rule) && !rule.enabled);
    const ls_opt_ctx_t *log=opened_options->opt[3].sub;
    LS_CHECK(!log->opt[0].get(&log->opt[0]));log->opt[0].set(&log->opt[0],1);LS_CHECK(status().sinks&DET_SD);
    LS_CHECK(ls_scr_detector.key(LS_TK_CHAR,'s') && !ls_detector_running());
}
LS_CASE(detector_duplicate_callback_and_rate_bucket) {
    begin(one);ls_detector_rule_t r;LS_CHECK(ls_detector_rule(0,&r));r.cooldown_s=0;LS_CHECK(ls_detector_rule_set(0,&r));
    unsigned matches=status().matches;
    observe(42,false,DET_BLE,SW_TRACKER,false);ls_detector_tick();
    observe(42,false,DET_BLE,SW_TRACKER,false);ls_detector_tick();LS_CHECK(vibes==1 && status().matches==matches+1);
    for(unsigned i=0;i<8;i++)observe(1000+i,false,DET_BLE,SW_TRACKER,false);
    ls_detector_tick();LS_CHECK(vibes==4); /* Global burst four, then coalesce. */
    clock_us+=5000000;ls_shim_time_set(clock_us);ls_detector_tick();LS_CHECK(vibes==4);
    observe(2000,false,DET_BLE,SW_TRACKER,false);ls_detector_tick();LS_CHECK(vibes==5);
}

LS_CASE(detector_real_rid_adapter_validates_before_publish) {
    const char *json="{\"version\":1,\"rules\":[{\"id\":\"rid\",\"enabled\":true,\"sources\":[\"rid.ble\"],\"match\":{\"validated_message\":true},\"sinks\":[\"banner\"],\"cooldown_s\":0}]}";
    begin(json);ls_detector_policy(DET_RID,DET_BANNER);uint8_t mac[6]={1,2,3,4,5,6};
    uint8_t ad[31]={30,0x16,0xfa,0xff,0x0d,42,0x02,0x12};
    memcpy(ad+8,"DETECTOR-TEST",13);
    LS_CHECK(!ls_rid_advert(mac,0,-52,ad,30,clock_us));LS_CHECK(status().queued==0);
    LS_CHECK(ls_rid_advert(mac,0,-52,ad,31,clock_us));LS_CHECK(status().queued==1);
    ls_sweep_ble(mac,0,-52,ad,31,clock_us);LS_CHECK(status().queued==1);
    LS_CHECK(invalidations==0 && vibes==0); /* Decoder callbacks cannot call sinks. */
    ls_detector_tick();ls_detector_banner_poll();LS_CHECK(invalidations==1 && vibes==0 && rings==0);
}
LS_CASE(detector_rule_count_bounds_and_cooldown_capacity) {
    char json[16000];size_t n=(size_t)snprintf(json,sizeof(json),"{\"version\":1,\"rules\":[");
    for(unsigned i=0;i<64;i++)n+=(size_t)snprintf(json+n,sizeof(json)-n,"%s{\"id\":\"r%u\",\"sources\":[\"rid.ble\"],\"match\":{\"validated_message\":true},\"sinks\":[]}",i?",":"",i);
    strcpy(json+n,"]}");begin(json);LS_CHECK(status().rules==64);unsigned rev=status().revision;
    strcpy(json+n,",{\"id\":\"r64\",\"sources\":[\"rid.ble\"],\"match\":{\"validated_message\":true},\"sinks\":[]}]}");
    LS_CHECK(!ls_detector_parse(json,strlen(json)) && status().revision==rev);
    begin(one);ls_detector_rule_t r;LS_CHECK(ls_detector_rule(0,&r));r.cooldown_s=86400;r.sinks=0;LS_CHECK(ls_detector_rule_set(0,&r));
    unsigned matches=status().matches;
    for(unsigned batch=0;batch<8;batch++) {
        for(unsigned i=0;i<64;i++)observe(10000+batch*64+i,false,DET_BLE,SW_TRACKER,false);
        ls_detector_tick();clock_us+=1000000;ls_shim_time_set(clock_us);
    }
    LS_CHECK(status().matches==matches+512);
    observe(99999,false,DET_BLE,SW_TRACKER,false);ls_detector_tick();LS_CHECK(status().matches==matches+512);
    observe(10000,false,DET_BLE,SW_TRACKER,false);ls_detector_tick();LS_CHECK(status().matches==matches+512);
}
LS_CASE(detector_file_reload_retains_previous_rules_on_failure) {
    begin(one); /* Writer makes only the task-owned test directory. */
    ls_detector_event_t e={0};LS_CHECK(ls_detector_write(&e,1));
    FILE *f=fopen(LS_DETECTOR_DIR "/rules.json","wb");LS_CHECK(f!=NULL);if(!f)return;
    fputs(one,f);fclose(f);ls_detector_request(false);ls_detector_tick();unsigned revision=status().revision;
    f=fopen(LS_DETECTOR_DIR "/rules.json","wb");LS_CHECK(f!=NULL);if(!f)return;fputs("{broken",f);fclose(f);
    ls_detector_request(false);ls_detector_tick();LS_CHECK(status().revision==revision && status().rules==1 && status().error[0]);
    remove(LS_DETECTOR_DIR "/rules.json");
}

LS_CASE(detector_does_not_enable_legacy_raw_logging) {
    begin(one);ls_sweep_start(false);ls_sweep_clear();
    ls_sweep_settings_t settings;ls_sweep_settings_get(&settings);settings.logging=true;LS_CHECK(ls_sweep_settings_set(&settings));
    uint8_t mac[6]={1,2,3,4,5,6};ls_sweep_match_t m={.category=SW_TRACKER};
    ls_sweep_observe(mac,0,0,-50,&m,clock_us);ls_sweep_device_t event;
    LS_CHECK(!ls_sweep_log_event(&event));
    settings.logging=false;LS_CHECK(ls_sweep_settings_set(&settings));
}
LS_CASE(detector_sustained_rate_flood_cannot_starve_summary) {
    begin(one);
    for(unsigned second=0;second<20;second++) {
        for(unsigned i=0;i<8;i++)observe(100000+second*8+i,false,DET_BLE,SW_TRACKER,false);
        ls_detector_tick();clock_us+=1000000;ls_shim_time_set(clock_us);
    }
    ls_detector_event_t events[LS_DETECTOR_HISTORY];size_t n=ls_detector_history(events,LS_DETECTOR_HISTORY);unsigned summaries=0;
    for(size_t i=0;i<n;i++)if(events[i].coalesced && events[i].at>=clock_us-20000000)summaries++;
    LS_CHECK(summaries>=3);
}

LS_CASE(detector_default_rotation_and_section_caps) {
    FILE *f=fopen("data/detector/rules.json","rb");LS_CHECK(f);if(!f)return;
    char json[8192];size_t n=fread(json,1,sizeof(json)-1,f);fclose(f);json[n]=0;
    begin(json);unsigned initial=status().matches;
    for(unsigned i=0;i<200;i++) {
        uint8_t mac[6]={1,2,3,4,(uint8_t)(i>>8),(uint8_t)i};
        ls_detector_observe(mac,1,DET_BLE,SW_TRACKER,-40,false,true,i%2?SW_NEW:SW_PASSING,clock_us);
        ls_detector_tick();clock_us+=1500000;ls_shim_time_set(clock_us);
    }
    LS_CHECK(status().matches==initial && vibes==0);
    observe(900,false,DET_BLE,SW_TRACKER,false);ls_detector_tick();LS_CHECK(vibes==1);
    for(unsigned i=0;i<119;i++) {
        clock_us+=1000000;ls_shim_time_set(clock_us);
        observe(900,false,DET_BLE,SW_TRACKER,false);ls_detector_tick();
    }
    LS_CHECK(vibes==1);
    clock_us+=1000000;ls_shim_time_set(clock_us);observe(901,true,DET_BLE,SW_TRACKER,false);ls_detector_tick();
    ls_detector_event_t e;ls_detector_history(&e,1);LS_CHECK(vibes==2 && e.coalesced==119);
    begin(json);initial=status().matches;
    for(unsigned i=0;i<30;i++) {
        observe(100+i,true,DET_BLE,SW_CAMERA,false);ls_detector_tick();ls_detector_banner_poll();
        clock_us+=10000000;ls_shim_time_set(clock_us);
    }
    LS_CHECK(status().matches==initial+3 && vibes==0);
}

LS_CASE(detector_lora_match_thresholds_and_metadata) {
    const char *json="{\"version\":1,\"rules\":[{\"id\":\"drone-link\",\"enabled\":true,\"sources\":[\"lora.dronelink\"],\"match\":{\"band_in\":[\"915\"],\"min_rssi_dbm\":-90,\"min_hits_per_min\":3},\"sinks\":[\"banner\"]}]}";
    begin(json);ls_detector_policy(15,DET_BANNER);unsigned initial=status().matches;
    ls_detector_drone(868,0,200,-40,10,clock_us);
    ls_detector_drone(915,0,200,-91,10,clock_us);
    ls_detector_drone(915,0,200,-40,2,clock_us);ls_detector_tick();LS_CHECK(status().matches==initial);
    ls_detector_drone(915,0,200,-40,3,clock_us);ls_detector_tick();LS_CHECK(status().matches==initial+1);
    ls_detector_event_t e;ls_detector_history(&e,1);LS_CHECK(e.source==DET_LORA && e.band==915 && e.hits_per_min==3 && e.rate_guess==200 && !e.validated);
}

LS_CASE(detector_sweep_section_adapter_requires_tracking_time_and_gps_movement) {
    const char *json="{\"version\":1,\"rules\":[{\"id\":\"with-you\",\"enabled\":true,\"sources\":[\"sweep.ble\"],\"match\":{\"category_in\":[\"TRACKER\"],\"section_in\":[\"with_you\"]},\"sinks\":[\"vibrate\"]}]}";
    begin(json);ls_sweep_clear();ls_sweep_defaults();
    uint8_t mac[6]={1,2,3,4,5,6};ls_sweep_match_t m={.category=SW_TRACKER,.state=SW_STATE_SEPARATED};
    ls_sweep_motion(true,0,0,clock_us);
    for(unsigned second=0;second<=600;second+=30) {
        ls_sweep_observe(mac,0,0,-50,&m,clock_us);ls_detector_tick();
        clock_us+=30000000;ls_shim_time_set(clock_us);
    }
    LS_CHECK(vibes==0); /* Ten minutes stationary, GPS present. */
    ls_sweep_motion(true,501,0,clock_us);
    ls_sweep_observe(mac,0,0,-50,&m,clock_us);ls_detector_tick();LS_CHECK(vibes==1);
    clock_us+=1000000;ls_shim_time_set(clock_us);
    ls_sweep_observe(mac,0,0,-50,&m,clock_us);ls_detector_tick();LS_CHECK(vibes==1);
}

/* Exercise the real draw and touch callbacks, not keyboard-only substitutes. */
static tui_cell ui_back[120*80],ui_front[120*80],ui_saved[120*80];
static bool ui_has(tui_surface *sf,const char *word) {
    size_t n=strlen(word);
    for(int y=0;y<sf->h;y++)for(int x=0;x+(int)n<=sf->w;x++) {
        size_t i=0;while(i<n && sf->back[y*sf->w+x+i].ch==word[i])i++;
        if(i==n)return true;
    }
    return false;
}
LS_CASE(detector_ui_touch_results_and_idle_stability) {
    begin(one);
    for(int orientation=0;orientation<2;orientation++) {
        int w=orientation?115:52,h=orientation?27:64;
        tui_surface sf;tui_surface_setup(&sf,ui_back,ui_front,w,h);
        tui_rect area=tui_surface_rect(&sf);
        ls_scr_detector.enter();tui_frame_begin(&sf);ls_scr_detector.draw(&sf,area);
        /* Every control is a filled key at least three cells high. */
        int cols=orientation?7:4,cy=h-(orientation?4:8);
        LS_CHECK(ls_scr_detector.touch(w/(2*cols),cy+1));
        LS_CHECK(!ls_detector_running());
        LS_CHECK(ls_scr_detector.touch(w/(2*cols),cy+1));
        LS_CHECK(ls_detector_running());
        LS_CHECK(ls_scr_detector.touch(2*w/cols+w/(2*cols),cy+1));
        LS_CHECK(opened_options && opened_options->radio==LS_RSEL_NONE && opened_options->draw_header);
        int testx=(6%cols)*w/cols+w/(2*cols),testy=cy+(6/cols)*4+1;
        LS_CHECK(ls_scr_detector.touch(testx,testy));
        ls_detector_tick();clock_us+=300000;ls_shim_time_set(clock_us);
        tui_frame_begin(&sf);ls_scr_detector.draw(&sf,area);
        LS_CHECK(ui_has(&sf,"BANNER") && ui_has(&sf,"VIBRATE") && ui_has(&sf,"SD LOG"));
        LS_CHECK(ui_has(&sf,"ACCEPTED") && ui_has(&sf,"OPERATOR TEST"));
        memcpy(ui_saved,ui_back,w*h*sizeof(tui_cell));
        clock_us+=1000000;ls_shim_time_set(clock_us);
        tui_frame_begin(&sf);ls_scr_detector.draw(&sf,area);
        LS_CHECK(!memcmp(ui_saved,ui_back,w*h*sizeof(tui_cell)));
        LS_CHECK(ls_scr_detector.touch(w/cols+w/(2*cols),cy+1));
        tui_frame_begin(&sf);ls_scr_detector.draw(&sf,area);
        LS_CHECK(ui_has(&sf,"RECENT MATCHES"));
        memcpy(ui_saved,ui_back,w*h*sizeof(tui_cell));
        observe(1001+orientation,false,DET_BLE,SW_TRACKER,false);ls_detector_tick();
        clock_us+=300000;ls_shim_time_set(clock_us);
        tui_frame_begin(&sf);ls_scr_detector.draw(&sf,area);
        LS_CHECK(memcmp(ui_saved,ui_back,w*h*sizeof(tui_cell))!=0);
    }
}
LS_CASE(detector_snapshot_and_steady_banner) {
    begin(one);observe(991,false,DET_BLE,SW_TRACKER,false);ls_detector_tick();
    ls_detector_status_t s;ls_detector_event_t e;size_t n=0;
    LS_CHECK(ls_detector_snapshot(&s,&e,1,&n) && n==1 && e.category==SW_TRACKER);
    ls_detector_rule_t r;LS_CHECK(ls_detector_rule_snapshot(0,&r) && !strcmp(r.id,"tag"));
    ls_notice_t notice={.title="DETECTOR",.body="TRACKER / matched",.accent=TUI_CYAN,.steady=true,.quiet=true,.screen=0};
    for(int i=0;i<4;i++)ls_detector_banner_poll();
    ls_notify_post_quiet(&notice);
    tui_surface sf;tui_surface_setup(&sf,ui_back,ui_front,52,64);
    tui_frame_begin(&sf);ls_notify_draw(&sf,tui_surface_rect(&sf));
    memcpy(ui_saved,ui_back,sizeof(ui_back));
    for(int i=0;i<20;i++)ls_notify_poll(0);
    tui_frame_begin(&sf);ls_notify_draw(&sf,tui_surface_rect(&sf));
    LS_CHECK(!memcmp(ui_saved,ui_back,52*64*sizeof(tui_cell)));
    LS_CHECK(ui_has(&sf,"[X]") && !ui_has(&sf,"ESC / X") && !ui_has(&sf,"Tap to inspect"));
    LS_CHECK(ls_notify_touch(47,2) && !ls_notify_showing());
}

static void notice_row(const tui_surface *sf,int y,int x,int n,char *out) {
    for(int i=0;i<n;i++)out[i]=(char)sf->back[y*sf->w+x+i].ch;
    out[n]=0;
}
static void notice_steady(const char *body) {
    ls_notice_t n={.title="DETECTOR",.accent=TUI_BLUE|TUI_BRIGHT,.steady=true,.quiet=true,.screen=-1};
    snprintf(n.body,sizeof(n.body),"%s",body);
    ls_notify_clear();ls_notify_post_quiet(&n);
}

/* A steady notice is the house banner: accent box, title bar with [X], 4 rows. */
LS_CASE(steady_notice_draws_the_standard_title_row) {
    begin(one);notice_steady("TEST / sink test");
    tui_surface sf;tui_surface_setup(&sf,ui_back,ui_front,52,64);
    tui_frame_begin(&sf);tui_fill(&sf,tui_surface_rect(&sf),'.',0);
    ls_notify_draw(&sf,tui_rect_make(0,1,52,60));
    char t[64];
    const uint8_t bar=TUI_ATTR(TUI_WHITE|TUI_BRIGHT,TUI_BLUE|TUI_BRIGHT);
    notice_row(&sf,2,2,8,t);LS_CHECK(!strcmp(t,"DETECTOR"));
    notice_row(&sf,2,47,3,t);LS_CHECK(!strcmp(t,"[X]"));
    for(int x=1;x<51;x++)LS_CHECK(sf.back[2*52+x].attr==bar);
    notice_row(&sf,3,2,16,t);LS_CHECK(!strcmp(t,"TEST / sink test"));
    LS_CHECK(sf.back[1*52+1].attr==TUI_ATTR(TUI_BLUE|TUI_BRIGHT,TUI_BLACK));
    LS_CHECK(sf.back[5*52+10].ch=='.');   /* four rows, not five */
    LS_CHECK(!ui_has(&sf,"ESC / X") && !ui_has(&sf,"Tap to inspect"));
    ls_notify_clear();
}

/* A body wider than the line scrolls, to its end, instead of being clipped. */
LS_CASE(steady_notice_scrolls_a_long_body_to_its_end) {
    begin(one);
    char body[LS_NOTIFY_BODY];
    for(int i=0;i<110;i++)body[i]=(char)('a'+i%26);
    body[110]=0;
    notice_steady(body);
    tui_surface sf;tui_surface_setup(&sf,ui_back,ui_front,52,64);
    tui_rect area=tui_rect_make(0,1,52,60);
    char t[64],w[64];
    tui_frame_begin(&sf);ls_notify_draw(&sf,area);
    notice_row(&sf,3,2,48,t);LS_CHECK(!strncmp(t,body,48));
    for(int i=0;i<100;i++)ls_notify_poll(0);
    tui_frame_begin(&sf);ls_notify_draw(&sf,area);
    notice_row(&sf,3,2,48,t);LS_CHECK(!strncmp(t,body+13,48));
    for(int i=0;i<160;i++)ls_notify_poll(0);
    LS_CHECK(ls_notify_showing());        /* up long enough to be read to the end */
    tui_frame_begin(&sf);ls_notify_draw(&sf,area);
    notice_row(&sf,3,2,48,t);snprintf(w,sizeof(w),"%.48s",body+62);LS_CHECK(!strcmp(t,w));
    ls_notify_clear();
}

/* Nothing is drawn in the rounded corners, whatever area the router hands in. */
LS_CASE(steady_notice_stays_inside_the_corner_safe_span) {
    begin(one);
    notice_steady("AIRCRAFT N123AB B738 FL035 4.2km NE 1850ft squawk 7700 / altitude_baro_ft_below 2000 within 10 km");
    tui_surface sf;tui_surface_setup(&sf,ui_back,ui_front,52,64);
    tui_rect areas[2]={tui_rect_make(0,1,52,60),tui_rect_make(-4,1,60,60)};
    for(int a=0;a<2;a++)for(int pad=0;pad<=5;pad+=5) {
        corner_pad=pad;
        tui_frame_begin(&sf);tui_fill(&sf,tui_surface_rect(&sf),'.',0);
        ls_notify_draw(&sf,areas[a]);
        for(int y=0;y<64;y++)for(int x=0;x<52;x++) {
            const bool inside=x>=pad && x<52-pad;
            if(!inside)LS_CHECK(sf.back[y*52+x].ch=='.');
        }
        LS_CHECK(ui_has(&sf,"[X]"));
        for(int y=0;y<64;y++)for(int x=0;x<52;x++)
            if(y<1 || y>4)LS_CHECK(sf.back[y*52+x].ch=='.');
    }
    corner_pad=0;
    ls_notify_clear();
}

/* The shared label helper must retain the physical inset and row clipping. */
LS_CASE(detector_shared_line_clipping) {
    tui_surface sf;tui_cell back[60],front[60];
    tui_surface_setup(&sf,back,front,12,5);
    tui_rect area=tui_rect_make(-4,1,24,3);
    tui_frame_begin(&sf);tui_fill(&sf,tui_surface_rect(&sf),'.',0);
    corner_pad=2;
    ls_safe_line(&sf,area,2,"ABCDEFGHIJKLM",TUI_ATTR(TUI_CYAN,TUI_BLACK));
    for(int y=0;y<5;y++)for(int x=0;x<12;x++) {
        const tui_cell *cell=&back[y*12+x];
        LS_CHECK(cell->ch==(y==2 && x>=3 && x<9?'A'+x-3:'.'));
    }
    memcpy(front,back,sizeof(back));
    ls_safe_line(NULL,area,2,"ignored",0);
    ls_safe_line(&sf,area,2,NULL,0);
    ls_safe_line(&sf,area,0,"ignored",0);
    ls_safe_line(&sf,area,4,"ignored",0);
    ls_safe_line(&sf,tui_rect_make(0,1,2,3),2,"ignored",0);
    LS_CHECK(!memcmp(front,back,sizeof(back)));
    corner_pad=0;
}

/* Aircraft: OPTIONS thresholds and the AIRCRAFT view, through the real callbacks. */
LS_CASE(detector_aircraft_options_edit_only_thresholds_a_rule_has) {
    const char *json="{\"version\":1,\"defaults\":{\"enabled\":true},\"rules\":["
        "{\"id\":\"tag\",\"sources\":[\"sweep.ble\"],\"match\":{\"category_in\":[\"TRACKER\"]},\"sinks\":[\"banner\"]},"
        "{\"id\":\"near\",\"sources\":[\"adsb\"],\"match\":{\"distance_km_within\":10},\"sinks\":[\"banner\"]},"
        "{\"id\":\"low\",\"sources\":[\"adsb\"],\"match\":{\"altitude_baro_ft_below\":1500},\"sinks\":[\"banner\"]}]}";
    begin(json);LS_CHECK(ls_scr_detector.key(LS_TK_CHAR,'o'));
    const ls_opt_ctx_t *src=opened_options->opt[0].sub,*r=opened_options->opt[1].sub;
    LS_CHECK(src->n==5 && src->opt[4].get && !strcmp(src->opt[4].label,"ADS-B aircraft (table only)"));
    src->opt[4].set(&src->opt[4],0);LS_CHECK(!(status().sources&DET_ADSB));
    src->opt[4].set(&src->opt[4],1);LS_CHECK(status().sources&DET_ADSB);
    LS_CHECK(r->n==11 && r->opt[9].why_not && r->opt[10].why_not);
    LS_CHECK(r->opt[9].why_not(&r->opt[9]) && r->opt[10].why_not(&r->opt[10])); /* rule 1: a SWEEP rule */
    r->opt[0].set_num(&r->opt[0],2);
    LS_CHECK(!r->opt[9].why_not(&r->opt[9]) && r->opt[10].why_not(&r->opt[10]));
    r->opt[9].set_num(&r->opt[9],25);
    ls_detector_rule_t rule;LS_CHECK(ls_detector_rule(1,&rule) && rule.air_km==25 && rule.air_mask==DET_AIR_KM);
    LS_CHECK(r->opt[9].num(&r->opt[9])==25);
    r->opt[0].set_num(&r->opt[0],3);
    r->opt[10].set_num(&r->opt[10],900);
    LS_CHECK(ls_detector_rule(2,&rule) && rule.air_alt_below_ft==900);
    /* A rule cannot gain a predicate it was not loaded with. */
    r->opt[9].set_num(&r->opt[9],5);
    LS_CHECK(ls_detector_rule(2,&rule) && rule.air_km==0 && !(rule.air_mask&DET_AIR_KM));
    r->opt[0].set_num(&r->opt[0],1);
}
LS_CASE(detector_aircraft_view_is_touch_reachable_and_reads_the_table) {
    begin(one);
    for(int orientation=0;orientation<2;orientation++) {
        int w=orientation?115:52,h=orientation?27:64;
        tui_surface sf;tui_surface_setup(&sf,ui_back,ui_front,w,h);
        tui_rect area=tui_surface_rect(&sf);
        air_source=ADSB_SRC_NONE;
        ls_scr_detector.enter();tui_frame_begin(&sf);ls_scr_detector.draw(&sf,area);
        int cols=orientation?7:4,cy=h-(orientation?4:8);
        /* The AIRCRAFT key is a filled key at least 8 x 3 cells. */
        LS_CHECK(ls_scr_detector.touch(3*w/cols+w/(2*cols),cy+1));
        clock_us+=300000;ls_shim_time_set(clock_us);
        tui_frame_begin(&sf);ls_scr_detector.draw(&sf,area);
        LS_CHECK(ui_has(&sf,"AIRCRAFT WATCH") && ui_has(&sf,"SOURCE OFF") && ui_has(&sf,"claims no radio"));
        /* With a receiver running the fresh table shows, with the squawk. */
        adsb_state_init();air_source=ADSB_SRC_IQ;
        adsb_aircraft_t *a=adsb_state_find_or_create(0xABC123);LS_CHECK(a!=NULL);
        if(a) {
            snprintf(a->callsign,sizeof(a->callsign),"UAL123");a->last_seen_us=clock_us;a->sources=1;a->emitter_tc=4;a->emitter_ca=5;
            a->altitude=12000;a->altitude_valid=true;a->altitude_ts_us=clock_us;
            adsb_state_set_squawk(a,7700);adsb_state_set_squawk(a,7700);
        }
        clock_us+=300000;ls_shim_time_set(clock_us);
        tui_frame_begin(&sf);ls_scr_detector.draw(&sf,area);
        LS_CHECK(ui_has(&sf,"UAL123") && ui_has(&sf,"sq7700") && ui_has(&sf,"heavy") && ui_has(&sf,"12000ft"));
        /* A tap on the row opens the inspector; BACK returns. */
        LS_CHECK(ls_scr_detector.key(LS_TK_CHAR,'d'));
        clock_us+=300000;ls_shim_time_set(clock_us);
        tui_frame_begin(&sf);ls_scr_detector.draw(&sf,area);
        LS_CHECK(ui_has(&sf,"TABLE DETAIL") && ui_has(&sf,"EMERGENCY CODE") && ui_has(&sf,"barometric"));
        LS_CHECK(ls_scr_detector.key(LS_TK_ESC,0));
        LS_CHECK(ls_scr_detector.touch(3*w/cols+w/(2*cols),cy+1)); /* back to MATCHES */
        clock_us+=300000;ls_shim_time_set(clock_us);
        tui_frame_begin(&sf);ls_scr_detector.draw(&sf,area);
        LS_CHECK(ui_has(&sf,"RECENT MATCHES"));
        air_source=ADSB_SRC_NONE;
    }
}
LS_CASE(detector_writer_logs_aircraft_broadcast_identifiers_never_position) {
    ls_detector_event_t e={.at=123,.entity=0xabcdef,.source=DET_ADSB,.category=255,.selected=DET_SD,.delivered=DET_SD,
        .icao=0xABC123,.emitter_tc=4,.emitter_ca=7,.squawk=7700,.emergency=1,.altitude_ft=3500,.distance_x10_km=56};
    strcpy(e.callsign,"UAL123");strcpy(e.rule_ids,"\"air-emergency-squawk\"");
    LS_CHECK(ls_detector_write(&e,0x1234));
    FILE *f=fopen(LS_DETECTOR_DIR "/events/detector-0.jsonl","rb");LS_CHECK(f!=NULL);if(!f)return;
    static char buf[8192];size_t n=fread(buf,1,sizeof(buf)-1,f);fclose(f);buf[n]=0;
    const char *line=strrchr(buf,'\n');if(line && line[1]==0) {buf[n-1]=0;line=strrchr(buf,'\n');}
    line=line?line+1:buf;
    LS_CHECK(strstr(line,"\"source\":\"adsb\"") && strstr(line,"\"icao\":\"ABC123\"") && strstr(line,"\"callsign\":\"UAL123\""));
    LS_CHECK(strstr(line,"\"squawk\":7700") && strstr(line,"\"emergency_state\":1") && strstr(line,"\"altitude_baro_ft\":3500"));
    LS_CHECK(strstr(line,"\"adsb_category\":\"rotorcraft\"") && strstr(line,"\"rssi\":null") && strstr(line,"\"gps_valid\":false"));
    /* Where the receiver or the aircraft is never leaves the device. */
    LS_CHECK(!strstr(line,"\"lat") && !strstr(line,"\"lon") && !strstr(line,"distance") && !strstr(line,"position"));
    e.altitude_ft=INT32_MIN;e.emitter_tc=0;
    LS_CHECK(ls_detector_write(&e,0x1234));
}
