/* DETECTOR aircraft rules: known aircraft states and replayed ADS-B frames go
   through the real parser, matcher, cooldowns and rate limits. No receiver is
   started; the "source" is the shared aircraft table the decoder fills. */
#include "ls_test.h"
#include "ls_detector.h"
#include "ls_detector_air.h"
#include "ls_sweep_app.h"
#include "ls_wifi.h"
#include "ls_gps.h"
#include "ble_link.h"
#include "esp_timer.h"
#include "apps/adsb/adsb_app.h"
#include "apps/adsb/adsb_state.h"
#include "apps/adsb/adsb_decode.h"
#include "apps/adsb/mode-s.h"
#include "audio_events.h"
#include "event_bus.h"
#include "perf.h"
#include "plane_audio.h"
#include <string.h>
#include <stdio.h>

/* ---- stand-ins for the platform around the worker -------------------------- */
void ls_wireless_wake(void) {}
void ls_sweep_wake(void) {}
esp_err_t ble_link_sweep_observer(bool on) {(void)on;return ESP_OK;}
void ls_sweep_transport_stats(ls_sweep_transport_stats_t *out) {memset(out,0,sizeof(*out));}
int ls_wifi_scan_cached(ls_wifi_scan_ap_t *out,int cap,int64_t *at) {(void)out;(void)cap;(void)at;return 0;}
bool snd_test_busy(void) {return false;}
bool audio_is_muted(void) {return false;}
int audio_volume_get(void) {return 50;}
bool snd_sweep_click(void) {return true;}
bool snd_sweep_start(int category) {(void)category;return true;}
static unsigned deliveries;
static ls_detector_event_t last;
uint8_t ls_detector_deliver(const ls_detector_event_t *e,uint8_t mask) {deliveries++;last=*e;return mask;}
void ls_detector_log_begin(void) {}
bool ls_detector_log_end(void) {return true;}
bool ls_detector_write(const ls_detector_event_t *e,uint64_t boot) {(void)e;(void)boot;return true;}
void event_bus_publish_contact(evt_kind_t kind,const char *app,const evt_contact_t *c) {(void)kind;(void)app;(void)c;}
void audio_events_publish(audio_evt_kind_t kind,uint32_t icao,const char *callsign,bool crc_shaky) {(void)kind;(void)icao;(void)callsign;(void)crc_shaky;}
plane_category_t plane_classify(uint32_t icao,const char *callsign) {(void)icao;(void)callsign;return PLANE_UNKNOWN;}
void perf_count_msg_good(void) {}
void perf_count_msg_bad(void) {}
void perf_count_burst(void) {}
void perf_mark_good_msg(int64_t now_us) {(void)now_us;}
void perf_mark_position(int64_t now_us) {(void)now_us;}
void perf_set_mag(int avg,int peak) {(void)avg;(void)peak;}
void perf_set_active_count(int n) {(void)n;}
int perf_get_crc_good(void) {return 0;}
int perf_get_crc_err(void) {return 0;}

/* The receiver another app would run, and the GPS: both under test control. */
static adsb_source_t source=ADSB_SRC_IQ;
adsb_source_t adsb_active_source(void) {return source;}
static ls_gps_state_t gps;
void ls_gps_get(ls_gps_state_t *out) {*out=gps;}

uint32_t mode_s_checksum(unsigned char *msg,int bits);

#define SEC 1000000LL
static int64_t clock_us=100*SEC;
static void at(int64_t us) {clock_us=us;ls_shim_time_set(us);}
static ls_detector_status_t status(void) {ls_detector_status_t s;ls_detector_status(&s);return s;}

/* Squawk and emergency rules, each on its own so a case can name what fired. */
#define DEFAULTS "\"defaults\":{\"enabled\":true,\"cooldown_s\":120,\"rule_cooldown_s\":0}"
static const char *emergency_rules="{\"version\":1," DEFAULTS ",\"rules\":["
    "{\"id\":\"sq\",\"sources\":[\"adsb\"],\"match\":{\"squawk_in\":[\"7500\",\"7600\",\"7700\"]},\"sinks\":[\"banner\"]},"
    "{\"id\":\"emg\",\"sources\":[\"adsb\"],\"match\":{\"emergency_status\":true},\"sinks\":[\"banner\"]}]}";

static void setup(const char *json) {
    ls_detector_start(false);
    at(clock_us+1000*SEC);
    adsb_state_init();
    source=ADSB_SRC_IQ;
    memset(&gps,0,sizeof(gps));
    ls_detector_policy(31,DET_BANNER|DET_VIBRATE);
    ls_detector_start(true);ls_detector_tick();
    LS_CHECK(ls_detector_parse(json,strlen(json)));
    deliveries=0;
}
/* An aircraft heard `age_us` ago. */
static adsb_aircraft_t *plane(uint32_t icao,const char *call,int64_t age_us) {
    adsb_aircraft_t *a=adsb_state_find_or_create(icao);
    LS_CHECK(a!=NULL);if(!a)return NULL;
    if(call)snprintf(a->callsign,sizeof(a->callsign),"%s",call);
    a->last_seen_us=clock_us-age_us;a->sources=1;a->msg_count=a->good_msg_count=5;
    return a;
}
/* Two agreeing reports at the current clock. */
static void squawk(adsb_aircraft_t *a,int code) {adsb_state_set_squawk(a,code);adsb_state_set_squawk(a,code);}
static void emergency(adsb_aircraft_t *a,int state) {adsb_state_set_emergency(a,state);adsb_state_set_emergency(a,state);}
static void altitude(adsb_aircraft_t *a,int ft,int64_t age_us) {a->altitude=ft;a->altitude_valid=true;a->altitude_ts_us=clock_us-age_us;}
static void position(adsb_aircraft_t *a,float lat,float lon,int64_t age_us) {a->lat=lat;a->lon=lon;a->pos_valid=true;a->pos_ts_us=clock_us-age_us;}
static void fix(double lat,double lon) {
    gps.fix=true;gps.lat_deg=lat;gps.lon_deg=lon;gps.last_fix_us=clock_us;gps.alive=true;
}
static void heard(adsb_aircraft_t *a) {a->last_seen_us=clock_us;}

LS_CASE(aircraft_rule_grammar_is_strict) {
    setup(emergency_rules);unsigned revision=status().revision;
    const char *good[]={
        "{\"id\":\"x\",\"sources\":[\"adsb\"],\"match\":{\"callsign_prefix_in\":[]},\"sinks\":[]}",
        "{\"id\":\"x\",\"sources\":[\"adsb\"],\"match\":{\"icao_in\":[\"ABC123\",\"abc124\"],\"icao_ranges\":[[\"AE0000\",\"AEFFFF\"]]},\"sinks\":[]}",
        "{\"id\":\"x\",\"sources\":[\"adsb\"],\"match\":{\"adsb_category_in\":[\"rotorcraft\",\"heavy\",\"uav\"],\"distance_km_within\":10,\"altitude_baro_ft_below\":3000},\"sinks\":[]}",
    };
    for(unsigned i=0;i<3;i++) {
        char json[600];snprintf(json,sizeof(json),"{\"version\":1,\"rules\":[%s]}",good[i]);
        LS_CHECK(ls_detector_parse(json,strlen(json)));
    }
    const char *bad[]={
        /* Aircraft rules watch only the aircraft table and say what for. */
        "\"sources\":[\"adsb\"],\"match\":{},\"sinks\":[]",
        "\"sources\":[\"adsb\",\"sweep.ble\"],\"match\":{\"squawk_in\":[\"7700\"]},\"sinks\":[]",
        "\"sources\":[\"adsb\"],\"match\":{\"category_in\":[\"CAMERA\"]},\"sinks\":[]",
        "\"sources\":[\"adsb\"],\"match\":{\"squawk_in\":[\"7700\"],\"min_rssi_dbm\":-50},\"sinks\":[]",
        "\"sources\":[\"sweep.ble\"],\"match\":{\"squawk_in\":[\"7700\"]},\"sinks\":[]",
        "\"sources\":[\"rid.ble\"],\"match\":{\"validated_message\":true,\"emergency_status\":true},\"sinks\":[]",
        /* Values. */
        "\"sources\":[\"adsb\"],\"match\":{\"squawk_in\":[\"7800\"]},\"sinks\":[]",
        "\"sources\":[\"adsb\"],\"match\":{\"squawk_in\":[\"770\"]},\"sinks\":[]",
        "\"sources\":[\"adsb\"],\"match\":{\"squawk_in\":[\"7700\",\"7700\"]},\"sinks\":[]",
        "\"sources\":[\"adsb\"],\"match\":{\"squawk_in\":[7700]},\"sinks\":[]",
        "\"sources\":[\"adsb\"],\"match\":{\"callsign_prefix_in\":[\"ual\"]},\"sinks\":[]",
        "\"sources\":[\"adsb\"],\"match\":{\"callsign_prefix_in\":[\"\"]},\"sinks\":[]",
        "\"sources\":[\"adsb\"],\"match\":{\"callsign_prefix_in\":[\"TOOLONGCS\"]},\"sinks\":[]",
        "\"sources\":[\"adsb\"],\"match\":{\"icao_in\":[\"ABC12\"]},\"sinks\":[]",
        "\"sources\":[\"adsb\"],\"match\":{\"icao_in\":[\"ABC12G\"]},\"sinks\":[]",
        "\"sources\":[\"adsb\"],\"match\":{\"icao_ranges\":[[\"AE0000\"]]},\"sinks\":[]",
        "\"sources\":[\"adsb\"],\"match\":{\"icao_ranges\":[[\"AEFFFF\",\"AE0000\"]]},\"sinks\":[]",
        "\"sources\":[\"adsb\"],\"match\":{\"adsb_category_in\":[\"jumbo\"]},\"sinks\":[]",
        "\"sources\":[\"adsb\"],\"match\":{\"adsb_category_in\":[\"heavy\",\"heavy\"]},\"sinks\":[]",
        "\"sources\":[\"adsb\"],\"match\":{\"emergency_status\":false},\"sinks\":[]",
        "\"sources\":[\"adsb\"],\"match\":{\"distance_km_within\":0},\"sinks\":[]",
        "\"sources\":[\"adsb\"],\"match\":{\"distance_km_within\":1001},\"sinks\":[]",
        "\"sources\":[\"adsb\"],\"match\":{\"altitude_baro_ft_below\":60001},\"sinks\":[]",
        "\"sources\":[\"adsb\"],\"match\":{\"altitude_baro_ft_below\":-5},\"sinks\":[]",
        "\"sources\":[\"adsb\"],\"match\":{\"altitude_baro_ft_below\":3000,\"altitude_baro_ft_below\":3000},\"sinks\":[]",
        "\"sources\":[\"adsb\"],\"match\":{\"squawk_in\":[\"1000\",\"1001\",\"1002\",\"1003\",\"1004\",\"1005\",\"1006\",\"1007\",\"1010\"]},\"sinks\":[]",
    };
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++) {
        char json[700];snprintf(json,sizeof(json),"{\"version\":1,\"rules\":[{\"id\":\"x\",%s}]}",bad[i]);
        revision=status().revision;
        LS_CHECK_MSG(!ls_detector_parse(json,strlen(json)),"accepted: %s",bad[i]);
        LS_CHECK(status().revision==revision && status().error[0]);
    }
}

LS_CASE(emergency_squawk_alerts_once_then_on_a_new_code) {
    setup(emergency_rules);
    adsb_aircraft_t *a=plane(0xABC123,"UAL123",0);squawk(a,7700);
    ls_detector_tick();LS_CHECK(deliveries==1);
    LS_CHECK(last.source==DET_ADSB && last.squawk==7700 && last.icao==0xABC123 && !strcmp(last.callsign,"UAL123"));
    LS_CHECK(!strcmp(last.rule_ids,"\"sq\"") && last.entity && ls_detector_air_alarm(last.squawk,last.emergency));
    /* Same aircraft, same code, still heard: held by the per-aircraft cooldown. */
    at(clock_us+3*SEC);heard(a);unsigned held=status().suppressed;
    ls_detector_tick();LS_CHECK(deliveries==1 && status().suppressed>held);
    /* A different emergency code is a new alert. */
    at(clock_us+3*SEC);heard(a);squawk(a,7600);
    ls_detector_tick();LS_CHECK(deliveries==2 && last.squawk==7600);
    /* After the cooldown the same code alerts again while the aircraft is heard. */
    at(clock_us+121*SEC);heard(a);squawk(a,7600);
    ls_detector_tick();LS_CHECK(deliveries==3);
    /* An ordinary code does not. */
    at(clock_us+200*SEC);heard(a);squawk(a,1200);
    ls_detector_tick();LS_CHECK(deliveries==3);
}

LS_CASE(unknown_unconfirmed_and_stale_fields_never_match) {
    const char *json="{\"version\":1," DEFAULTS ",\"rules\":["
        "{\"id\":\"sq\",\"sources\":[\"adsb\"],\"match\":{\"squawk_in\":[\"7700\"]},\"sinks\":[\"banner\"]},"
        "{\"id\":\"emg\",\"sources\":[\"adsb\"],\"match\":{\"emergency_status\":true},\"sinks\":[\"banner\"]},"
        "{\"id\":\"low\",\"sources\":[\"adsb\"],\"match\":{\"altitude_baro_ft_below\":3000},\"sinks\":[\"banner\"]},"
        "{\"id\":\"near\",\"sources\":[\"adsb\"],\"match\":{\"distance_km_within\":50},\"sinks\":[\"banner\"]},"
        "{\"id\":\"cat\",\"sources\":[\"adsb\"],\"match\":{\"adsb_category_in\":[\"heavy\",\"rotorcraft\"]},\"sinks\":[\"banner\"]},"
        "{\"id\":\"call\",\"sources\":[\"adsb\"],\"match\":{\"callsign_prefix_in\":[\"UAL\"]},\"sinks\":[\"banner\"]}]}";
    setup(json);unsigned matches0=status().matches;
    /* Nothing known beyond an address: unknown altitude, squawk, category, callsign, position. */
    plane(0x100001,NULL,0);
    /* One report of a squawk or an emergency is not enough. */
    adsb_aircraft_t *b=plane(0x100002,NULL,0);adsb_state_set_squawk(b,7700);adsb_state_set_emergency(b,1);
    /* An altitude older than the field limit, a position older than it, no receiver fix. */
    adsb_aircraft_t *c=plane(0x100003,NULL,0);altitude(c,500,16*SEC);position(c,0.01f,0.01f,16*SEC);fix(0,0);
    /* An emitter category the standard leaves reserved or unset. */
    adsb_aircraft_t *d=plane(0x100004,NULL,0);d->emitter_tc=4;d->emitter_ca=0;
    adsb_aircraft_t *e=plane(0x100005,NULL,0);e->emitter_tc=3;e->emitter_ca=5;
    ls_detector_tick();LS_CHECK(deliveries==0 && status().matches==matches0);
    /* The same facts, fresh, do match: the cases above were refused for being unknown, not by accident. */
    at(clock_us+2*SEC);d->emitter_ca=5;heard(d);
    adsb_aircraft_t *f=plane(0x100006,"UAL9",0);altitude(f,500,1*SEC);position(f,0.01f,0.01f,1*SEC);
    gps.last_fix_us=clock_us;
    ls_detector_tick();LS_CHECK(status().matches==matches0+2 && deliveries==2);
}

LS_CASE(stale_aircraft_never_alert_and_return_when_heard) {
    setup(emergency_rules);
    unsigned obs0=status().observations;
    adsb_aircraft_t *a=plane(0xABC123,"UAL123",61*SEC);squawk(a,7700);a->last_seen_us=clock_us-61*SEC;
    ls_detector_tick();LS_CHECK(deliveries==0 && status().observations==obs0);
    /* Heard again: now it is a live emergency. */
    at(clock_us+SEC);heard(a);
    ls_detector_tick();LS_CHECK(deliveries==1);
    /* An aircraft still heard whose squawk was last confirmed over a minute ago is not an alert either. */
    at(clock_us+1000*SEC);
    adsb_aircraft_t *b=plane(0xABC124,"UAL124",1*SEC);
    at(clock_us-70*SEC);squawk(b,7600);at(clock_us+70*SEC);heard(b);
    ls_detector_tick();LS_CHECK(deliveries==1);
    squawk(b,7600);at(clock_us+SEC);heard(b);ls_detector_tick();LS_CHECK(deliveries==2);
}

LS_CASE(squawk_and_emergency_rules_join_one_event) {
    setup(emergency_rules);
    adsb_aircraft_t *a=plane(0xABC123,"AAL1",0);squawk(a,7700);emergency(a,1);
    ls_detector_tick();LS_CHECK(deliveries==1);
    LS_CHECK(!strcmp(last.rule_ids,"\"sq\",\"emg\"") && last.emergency==1 && last.squawk==7700);
}

LS_CASE(callsign_and_icao_watch_lists) {
    const char *json="{\"version\":1," DEFAULTS ",\"rules\":["
        "{\"id\":\"call\",\"sources\":[\"adsb\"],\"match\":{\"callsign_prefix_in\":[\"RESCUE\",\"MED\"]},\"sinks\":[\"banner\"]},"
        "{\"id\":\"addr\",\"sources\":[\"adsb\"],\"match\":{\"icao_in\":[\"ABC123\"],\"icao_ranges\":[[\"AE0000\",\"AEFFFF\"]]},\"sinks\":[\"banner\"]},"
        "{\"id\":\"none\",\"sources\":[\"adsb\"],\"match\":{\"callsign_prefix_in\":[]},\"sinks\":[\"banner\"]}]}";
    setup(json);
    unsigned matches0=status().matches;
    plane(0x400001,"RESCUE12",0);                  /* prefix */
    plane(0x400002,"MED1",0);                      /* prefix */
    plane(0x400003,"RESCU",0);                     /* shorter than the prefix */
    plane(0x400004,"UAL1",0);                      /* nothing */
    plane(0x400005,NULL,0);                        /* callsign not heard */
    ls_detector_tick();LS_CHECK(deliveries==2);
    LS_CHECK(status().matches==matches0+2);
    unsigned n=deliveries;
    plane(0xABC123,"X1",0);ls_detector_tick();LS_CHECK(deliveries==n+1 && !strcmp(last.rule_ids,"\"addr\""));
    plane(0xAE1234,"X2",0);ls_detector_tick();LS_CHECK(deliveries==n+2);
    plane(0xAF0000,"X3",0);ls_detector_tick();LS_CHECK(deliveries==n+2);
    /* An address in another namespace (UAT self-assigned) is not an ICAO address. */
    adsb_aircraft_t *u=adsb_state_find_address(0xABC123,1);LS_CHECK(u!=NULL);
    if(u) {u->last_seen_us=clock_us;u->sources=2;ls_detector_tick();LS_CHECK(deliveries==n+2);}
}

LS_CASE(altitude_threshold_is_barometric_as_reported_and_fresh) {
    const char *json="{\"version\":1," DEFAULTS ",\"rules\":["
        "{\"id\":\"low\",\"sources\":[\"adsb\"],\"match\":{\"altitude_baro_ft_below\":1500},\"sinks\":[\"banner\"]}]}";
    setup(json);
    adsb_aircraft_t *a=plane(0x500001,"LOW1",0);altitude(a,1499,5*SEC);
    adsb_aircraft_t *b=plane(0x500002,"HIGH1",0);altitude(b,1500,1*SEC);
    adsb_aircraft_t *c=plane(0x500003,"OLD1",0);altitude(c,200,16*SEC);
    plane(0x500004,"UNK1",0);
    ls_detector_tick();LS_CHECK(deliveries==1 && last.icao==0x500001 && last.altitude_ft==1499);
    /* Negative barometric altitudes (below the pressure datum) are still lower. */
    adsb_aircraft_t *d=plane(0x500005,"NEG1",0);altitude(d,-100,SEC);ls_detector_tick();LS_CHECK(deliveries==2);
}

LS_CASE(distance_needs_a_fresh_receiver_fix_and_a_fresh_target_position) {
    const char *json="{\"version\":1," DEFAULTS ",\"rules\":["
        "{\"id\":\"near\",\"sources\":[\"adsb\"],\"match\":{\"distance_km_within\":10},\"sinks\":[\"banner\"]}]}";
    setup(json);
    /* 0.05 degrees of latitude is 5.56 km; 0.15 is 16.7 km. */
    adsb_aircraft_t *a=plane(0x600001,"NEAR1",0);position(a,0.05f,0.0f,2*SEC);
    adsb_aircraft_t *b=plane(0x600002,"FAR1",0);position(b,0.15f,0.0f,2*SEC);
    adsb_aircraft_t *c=plane(0x600003,"OLDPOS",0);position(c,0.05f,0.0f,16*SEC);
    plane(0x600004,"NOPOS",0);
    /* No receiver fix: nothing is near. */
    ls_detector_tick();LS_CHECK(deliveries==0);
    /* A fix older than three seconds is not a fix. */
    fix(0,0);gps.last_fix_us=clock_us-4*SEC;at(clock_us+SEC);heard(a);heard(b);heard(c);
    ls_detector_tick();LS_CHECK(deliveries==0);
    /* A fresh fix: only the aircraft with a fresh position inside the range. */
    at(clock_us+2*SEC);fix(0,0);heard(a);heard(b);heard(c);
    ls_detector_tick();LS_CHECK(deliveries==1 && last.icao==0x600001);
    LS_CHECK(last.distance_x10_km>=55 && last.distance_x10_km<=57);
}

LS_CASE(category_filter_uses_the_decoded_emitter_category) {
    const char *json="{\"version\":1," DEFAULTS ",\"rules\":["
        "{\"id\":\"cat\",\"sources\":[\"adsb\"],\"match\":{\"adsb_category_in\":[\"rotorcraft\",\"heavy\",\"uav\"]},\"sinks\":[\"banner\"]}]}";
    setup(json);
    adsb_aircraft_t *h=plane(0x700001,"HELI",0);h->emitter_tc=4;h->emitter_ca=7;
    adsb_aircraft_t *k=plane(0x700002,"HEAVY",0);k->emitter_tc=4;k->emitter_ca=5;
    adsb_aircraft_t *u=plane(0x700003,"DRONE",0);u->emitter_tc=3;u->emitter_ca=6;
    adsb_aircraft_t *s=plane(0x700004,"SMALL",0);s->emitter_tc=4;s->emitter_ca=2;
    adsb_aircraft_t *n=plane(0x700005,"NOCAT",0);n->emitter_tc=0;
    ls_detector_tick();LS_CHECK(deliveries==3);
    LS_CHECK(ls_detector_air_category_bit(4,7)==6 && ls_detector_air_category_bit(3,6)==11);
    LS_CHECK(ls_detector_air_category_bit(0,0)<0 && ls_detector_air_category_bit(4,0)<0 && ls_detector_air_category_bit(3,5)<0);
}

LS_CASE(no_source_means_source_off_and_nothing_is_started) {
    setup(emergency_rules);
    adsb_aircraft_t *a=plane(0xABC123,"UAL123",0);squawk(a,7700);
    source=ADSB_SRC_NONE;unsigned obs0=status().observations;
    ls_detector_tick();LS_CHECK(deliveries==0 && !strcmp(status().adsb,"OFF") && status().observations==obs0);
    /* The detector never starts one: with the receiver off the answer stays off. */
    ls_detector_tick();LS_CHECK(source==ADSB_SRC_NONE && !strcmp(status().adsb,"OFF"));
    /* Another app starts a receiver: the table is read from then on. */
    source=ADSB_SRC_LORA;heard(a);ls_detector_tick();
    LS_CHECK(deliveries==1 && !strcmp(status().adsb,"LR chip"));
    /* The policy toggle and the run switch both gate it. */
    at(clock_us+1000*SEC);heard(a);squawk(a,7600);
    ls_detector_policy(15,DET_BANNER);ls_detector_tick();LS_CHECK(deliveries==1);
    ls_detector_policy(31,DET_BANNER);ls_detector_start(false);ls_detector_tick();LS_CHECK(deliveries==1);
}

LS_CASE(an_emergency_flood_is_rate_limited_and_counted) {
    setup(emergency_rules);
    unsigned matches0=status().matches,supp0=status().suppressed;
    for(uint32_t i=0;i<16;i++) {adsb_aircraft_t *a=plane(0x800000+i,"FLOOD",0);squawk(a,7700);}
    ls_detector_tick();
    LS_CHECK(deliveries>=1 && deliveries<=4);
    LS_CHECK(status().matches==matches0+16 && status().suppressed>=supp0+12);
    unsigned first=deliveries;
    /* Held matches stay counted; the bucket refills with time, not with polling. */
    ls_detector_tick();LS_CHECK(deliveries==first);
    /* Matches the bucket refused are held by their cooldown and counted, not delivered late. */
    at(clock_us+2*SEC);
    for(int i=0;i<16;i++)heard(adsb_state_find_or_create(0x800000+(uint32_t)i));
    ls_detector_tick();LS_CHECK(deliveries==first);
    /* Once the cooldown ends they alert again, still bounded by the bucket. */
    at(clock_us+125*SEC);
    for(int i=0;i<16;i++) {adsb_aircraft_t *a=adsb_state_find_or_create(0x800000+(uint32_t)i);heard(a);squawk(a,7700);}
    ls_detector_tick();LS_CHECK(deliveries>first && deliveries<=first+4);
}

/* ---- replayed frames: the decoder fills the table, DETECTOR reads it ------- */
static void put_identity(uint8_t *hi,uint8_t *lo,int code) {
    const int a=code/1000%10,b=code/100%10,c=code/10%10,d=code%10;
    *hi=(uint8_t)(((c&1)<<4)|((a&1)<<3)|((c&2)<<1)|(a&2)|((c&4)>>2));
    *lo=(uint8_t)(((a&4)<<5)|((b&1)<<5)|((d&1)<<4)|((b&2)<<2)|((d&2)<<1)|((b&4)>>1)|((d&4)>>2));
}
static void seal(uint8_t *m) {
    m[11]=m[12]=m[13]=0;
    uint32_t crc=mode_s_checksum(m,112);
    m[11]=(uint8_t)(crc>>16);m[12]=(uint8_t)(crc>>8);m[13]=(uint8_t)crc;
}
static void status_frame(uint8_t m[14],uint32_t icao,int state,int code) {
    memset(m,0,14);m[0]=(17<<3)|5;m[1]=(uint8_t)(icao>>16);m[2]=(uint8_t)(icao>>8);m[3]=(uint8_t)icao;
    m[4]=(28<<3)|1;uint8_t hi,lo;put_identity(&hi,&lo,code);m[5]=(uint8_t)((state<<5)|hi);m[6]=lo;seal(m);
}
static int hexval(char c) {return c<='9'?c-'0':(c|0x20)-'a'+10;}
static void from_hex(uint8_t m[14],const char *hex) {for(int i=0;i<14;i++)m[i]=(uint8_t)((hexval(hex[2*i])<<4)|hexval(hex[2*i+1]));}
static uint8_t iq[2048];
static void on_air(const uint8_t *frame) {
    static const int preamble[]={0,2,7,9};
    memset(iq,127,sizeof(iq));
    for(int i=0;i<4;i++)iq[2*(100+preamble[i])]=227;
    for(int b=0;b<112;b++) {bool one=(frame[b/8]&(0x80>>(b%8)))!=0;iq[2*(100+16+2*b+(one?0:1))]=227;}
    adsb_on_sample(iq,(int)sizeof(iq));
}
LS_CASE(replayed_frames_reach_the_rules_through_the_shared_table) {
    const char *json="{\"version\":1," DEFAULTS ",\"rules\":["
        "{\"id\":\"sq\",\"sources\":[\"adsb\"],\"match\":{\"squawk_in\":[\"7700\"]},\"sinks\":[\"banner\"]},"
        "{\"id\":\"emg\",\"sources\":[\"adsb\"],\"match\":{\"emergency_status\":true},\"sinks\":[\"banner\"]},"
        "{\"id\":\"klm\",\"sources\":[\"adsb\"],\"match\":{\"callsign_prefix_in\":[\"KLM\"]},\"sinks\":[\"banner\"]},"
        "{\"id\":\"high\",\"sources\":[\"adsb\"],\"match\":{\"altitude_baro_ft_below\":30000},\"sinks\":[\"banner\"]}]}";
    setup(json);adsb_decode_init();
    uint8_t m[14];
    /* KLM1023 from the published identification vector. */
    from_hex(m,"8D4840D6202CC371C32CE0576098");on_air(m);
    ls_detector_tick();LS_CHECK(deliveries==1 && !strcmp(last.callsign,"KLM1023") && !strcmp(last.rule_ids,"\"klm\""));
    /* An airborne position at 38000 ft: above the threshold, and it is the same table. */
    at(clock_us+SEC);from_hex(m,"8D40621D58C382D690C8AC2863A7");on_air(m);
    ls_detector_tick();LS_CHECK(deliveries==1);
    /* The 7700 aircraft-status message: one report is not enough, two are. */
    at(clock_us+SEC);status_frame(m,0x40621D,1,7700);on_air(m);
    ls_detector_tick();LS_CHECK(deliveries==1);
    at(clock_us+SEC);on_air(m);
    ls_detector_tick();LS_CHECK(deliveries==2);
    LS_CHECK(last.icao==0x40621D && last.squawk==7700 && last.emergency==1 && last.altitude_ft==38000);
    LS_CHECK(!strcmp(last.rule_ids,"\"sq\",\"emg\""));
    /* It stops sending: after a minute and a bit the table still holds the aircraft, DETECTOR does not alert. */
    at(clock_us+130*SEC);ls_detector_tick();LS_CHECK(deliveries==2);
}

LS_CASE(screen_view_lists_fresh_aircraft_and_is_empty_when_the_source_is_off) {
    setup(emergency_rules);
    adsb_aircraft_t *a=plane(0xABC123,"UAL123",0);squawk(a,7700);
    adsb_aircraft_t *b=plane(0xABC124,"DAL9",5*SEC);
    plane(0xABC125,"OLD1",61*SEC);
    ls_detector_air_row_t rows[LS_DETECTOR_AIR_SLOTS];size_t n=99;
    LS_CHECK(ls_detector_air_view(rows,LS_DETECTOR_AIR_SLOTS,&n) && n==2);
    /* The alerting aircraft sorts first, whatever slot it holds. */
    LS_CHECK(rows[0].a.icao==0xABC123 && rows[0].rules==1 && rows[1].a.icao==0xABC124 && rows[1].rules==0);
    LS_CHECK(b!=NULL && !strcmp(rows[0].a.callsign,"UAL123") && rows[0].a.squawk==7700);
    source=ADSB_SRC_NONE;n=99;
    LS_CHECK(ls_detector_air_view(rows,LS_DETECTOR_AIR_SLOTS,&n) && n==0);
}
