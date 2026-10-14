/* Phase 1: bounded policy on the shared wireless worker. */
#include "ls_detector.h"
#include "ls_dronelink.h"
#ifndef ESP_PLATFORM
#pragma weak ls_dronelink_tick
#pragma weak ls_dronelink_pending
#pragma weak ls_dronelink_status
#define HAS_DRONELINK (ls_dronelink_tick != NULL)
#else
#define HAS_DRONELINK true
#endif
#include "ls_sweep_app.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#ifdef ESP_PLATFORM
#include "esp_random.h"
#endif
#ifndef LS_DETECTOR_DIR
#define LS_DETECTOR_DIR "/sdcard/detector"
#endif
static EXT_RAM_BSS_ATTR ls_detector_rule_t rules[64], staging[64];
static EXT_RAM_BSS_ATTR char input[LS_DETECTOR_BYTES+1];
typedef struct { uint64_t entity; int64_t at; uint8_t source,category,section; int8_t rssi; bool validated,flood; uint16_t band,rate_guess; uint32_t hits_per_min; } observation_t;
static EXT_RAM_BSS_ATTR observation_t queue[64];
static EXT_RAM_BSS_ATTR ls_detector_event_t history[LS_DETECTOR_HISTORY];
typedef struct { uint64_t entity; int64_t until; uint8_t rule; } cooldown_t;
static EXT_RAM_BSS_ATTR cooldown_t cooldown[512];
static EXT_RAM_BSS_ATTR ls_detector_status_t state;
static StaticSemaphore_t lock_mem;
static SemaphoreHandle_t mutex;
static unsigned once, head, count, history_n, sequence;
static uint32_t dropped;
static bool reload_requested,test_requested;
static uint64_t secret[2],boot_id;
typedef struct { uint64_t entity; int64_t at; uint8_t source,category,section; bool validated; } duplicate_t;
static EXT_RAM_BSS_ATTR duplicate_t duplicates[64];
static unsigned duplicate_at;
static int64_t bucket_at;
static unsigned tokens=4;
static EXT_RAM_BSS_ATTR struct { int64_t until; uint32_t pending; } rule_window[LS_DETECTOR_RULES];
static EXT_RAM_BSS_ATTR ls_detector_event_t work_event;
static void lock(void) { xSemaphoreTake(mutex,portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(mutex); }
static const char defaults[]=
"{\"version\":1,\"sweep_rules\":\"/sdcard/sweep/rules.json\",\"defaults\":{\"enabled\":false,\"cooldown_s\":60,\"dedupe\":\"rule_entity\",\"rule_cooldown_s\":120},\"rules\":[{\"id\":\"remote-id\",\"enabled\":true,\"sources\":[\"rid.ble\"],\"match\":{\"validated_message\":true},\"sinks\":[\"vibrate\",\"banner\",\"sd\"]},{\"id\":\"with-you-trackers\",\"enabled\":true,\"sources\":[\"sweep.ble\",\"sweep.wifi\"],\"match\":{\"category_in\":[\"TRACKER\"],\"section_in\":[\"with_you\"]},\"sinks\":[\"vibrate\",\"banner\"]},{\"id\":\"cameras\",\"enabled\":true,\"sources\":[\"sweep.ble\",\"sweep.wifi\"],\"match\":{\"category_in\":[\"CAMERA\"]},\"sinks\":[\"banner\"]},{\"id\":\"bodycam-example\",\"sources\":[\"sweep.ble\"],\"match\":{\"category_in\":[\"BODYCAM\"]},\"sinks\":[\"banner\"]},{\"id\":\"wifi-example\",\"sources\":[\"sweep.wifi\"],\"match\":{\"category_in\":[\"CAMERA\"]},\"sinks\":[\"sd\"]},{\"id\":\"drone-link-example\",\"enabled\":false,\"sources\":[\"lora.dronelink\"],\"match\":{\"band_in\":[\"868\",\"915\"],\"min_rssi_dbm\":-100,\"min_hits_per_min\":3},\"sinks\":[\"banner\",\"sd\"]}]}";
/* Fixed-depth grammar. No recursive allocation, ignored keys, duplicate keys,
 * coercion, partial numbers, or executable future-phase operators. */
typedef struct { const char *p,*end; } json_t;
static void ws(json_t *j) {while(j->p<j->end && strchr(" \r\n\t",*j->p) && *j->p) j->p++;}
static bool take(json_t *j,char c) {ws(j);if(j->p==j->end || *j->p!=c)return false;j->p++;return true;}
static bool str(json_t *j,char *out,size_t cap) {
    if(!take(j,'"'))return false;
    size_t n=0;
    while(j->p<j->end && *j->p!='"') {
        unsigned char c=*j->p++;
        if(c=='\\') {if(j->p==j->end)return false;c=*j->p++;if(c!='"' && c!='\\' && c!='/')return false;}
        if(c<32 || c>126 || n+1>=cap)return false;
        out[n++]=c;
    }
    out[n]=0;return take(j,'"');
}
static bool boolean(json_t *j,bool *out) {
    ws(j);size_t n=(size_t)(j->end-j->p);
    if(n>=4 && !memcmp(j->p,"true",4)) {*out=true;j->p+=4;return true;}
    if(n>=5 && !memcmp(j->p,"false",5)) {*out=false;j->p+=5;return true;}
    return false;
}
static bool number(json_t *j,uint32_t *out) {
    ws(j);if(j->p==j->end || !isdigit((unsigned char)*j->p))return false;
    uint32_t v=0;bool zero=*j->p=='0';unsigned n=0;
    while(j->p<j->end && isdigit((unsigned char)*j->p)) {if(++n>6 || (zero && n>1))return false;v=v*10+(*j->p++-'0');}
    *out=v;return true;
}
const char *ls_detector_source(unsigned s) {return s==DET_RID?"rid.ble":s==DET_BLE?"sweep.ble":s==DET_WIFI?"sweep.wifi":s==DET_LORA?"lora.dronelink":"detector.summary";}
static bool list(json_t *j,uint8_t *out,int kind) {
    if(!take(j,'['))return false;
    *out=0;
    if(kind==2 && take(j,']'))return true;
    do {
        char s[32];unsigned bit=0;if(!str(j,s,sizeof(s)))return false;
        if(kind==0) {for(unsigned b=1;b<=8;b<<=1)if(!strcmp(s,ls_detector_source(b)))bit=b;}
        if(kind==1) {for(unsigned c=0;c<SW_CATS;c++)if(!strcmp(s,ls_sweep_category(c)))bit=1u<<c;}
        if(kind==2) {if(!strcmp(s,"banner"))bit=1;if(!strcmp(s,"vibrate"))bit=2;if(!strcmp(s,"sd"))bit=4;}
        if(kind==3) {if(!strcmp(s,"with_you"))bit=1;if(!strcmp(s,"new"))bit=2;if(!strcmp(s,"passing"))bit=4;}
        if(kind==4) {if(!strcmp(s,"868"))bit=1;if(!strcmp(s,"915"))bit=2;}
        if(!bit || (*out&bit))return false;
        *out|=bit;
    } while(take(j,','));
    return take(j,']');
}
static bool predicate(json_t *j,ls_detector_rule_t *r) {
    if(!take(j,'{'))return false;
    unsigned seen=0;
    do {
        char k[32];unsigned b=0;if(!str(j,k,sizeof(k)) || !take(j,':'))return false;
        if(!strcmp(k,"category_in")) {b=1;if(!list(j,&r->categories,1))return false;}
        else if(!strcmp(k,"band_in")) {b=8;if(!list(j,&r->bands,4))return false;}
        else if(!strcmp(k,"min_hits_per_min")) {b=16;if(!number(j,&r->min_hits_per_min))return false;}
        else if(!strcmp(k,"min_rssi_dbm")) {
            b=32;bool negative=take(j,'-');uint32_t v;
            if(!number(j,&v) || v>127 || (!negative && v))return false;
            r->min_rssi_dbm=-(int)v;r->has_min_rssi=true;
        }
        else if(!strcmp(k,"section_in")) {b=4;if(!list(j,&r->sections,3))return false;}
        else if(!strcmp(k,"validated_message")) {b=2;r->has_validated=true;if(!boolean(j,&r->validated))return false;}
        else return false;
        if(seen&b)return false;
        seen|=b;
    }while(take(j,','));
    return take(j,'}');
}
static bool object(json_t *j,ls_detector_rule_t *r,bool def) {
    if(!take(j,'{'))return false;
    unsigned seen=0;
    do {
        char k[32],s[32];unsigned b=0;
        if(!str(j,k,sizeof(k)) || !take(j,':'))return false;
        if(!strcmp(k,"enabled")) {b=1;if(!boolean(j,&r->enabled))return false;}
        else if(!strcmp(k,"cooldown_s")) {b=2;if(!number(j,&r->cooldown_s) || r->cooldown_s>86400)return false;}
        else if(!strcmp(k,"rule_cooldown_s")) {b=128;if(!number(j,&r->rule_cooldown_s) || r->rule_cooldown_s>86400)return false;}
        else if(!strcmp(k,"dedupe")) {b=4;if(!str(j,s,sizeof(s)) || strcmp(s,"rule_entity"))return false;}
        else if(!def && !strcmp(k,"id")) {
            b=8;if(!str(j,r->id,sizeof(r->id)) || !r->id[0])return false;
            for(const char *p=r->id;*p;p++)if(!isalnum((unsigned char)*p) && *p!='-' && *p!='_')return false;
        }
        else if(!def && !strcmp(k,"sources")) {b=16;if(!list(j,&r->sources,0))return false;}
        else if(!def && !strcmp(k,"match")) {b=32;if(!predicate(j,r))return false;}
        else if(!def && !strcmp(k,"sinks")) {b=64;if(!list(j,&r->sinks,2))return false;}
        else return false;
        if(seen&b)return false;
        seen|=b;
    }while(take(j,','));
    return take(j,'}') && (def || (seen&120)==120);
}
static bool parse(const char *text,size_t len,unsigned *n) {
    json_t j={text,text+len};unsigned seen=0;ls_detector_rule_t def={.cooldown_s=60,.rule_cooldown_s=120};
    /* Defaults may appear after rules: retain presence masks by parsing the
     * root twice, once for defaults and once for rules. Both passes validate. */
    for(int pass=0;pass<2;pass++) {
        j.p=text;seen=0;*n=0;
        if(!take(&j,'{'))return false;
        do {
            char k[32],s[64];uint32_t v;unsigned b=0;
            if(!str(&j,k,sizeof(k)) || !take(&j,':'))return false;
            if(!strcmp(k,"version")) {b=1;if(!number(&j,&v) || v!=1)return false;}
            else if(!strcmp(k,"sweep_rules")) {b=2;if(!str(&j,s,sizeof(s)) || strcmp(s,"/sdcard/sweep/rules.json"))return false;}
            else if(!strcmp(k,"defaults")) {b=4;ls_detector_rule_t d={.cooldown_s=60,.rule_cooldown_s=120};if(!object(&j,&d,true))return false;def=d;}
            else if(!strcmp(k,"rules")) {
                b=8;if(!take(&j,'['))return false;
                if(!take(&j,']')) {
                    do {
                        if(*n>=64)return false;
                        staging[*n]=def;
                        if(!object(&j,&staging[*n],false))return false;
                        for(unsigned i=0;i<*n;i++)if(!strcmp(staging[i].id,staging[*n].id))return false;
                        (*n)++;
                    }while(take(&j,','));
                    if(!take(&j,']'))return false;
                }
            } else return false;
            if(seen&b)return false;
        seen|=b;
        }while(take(&j,','));
        if(!take(&j,'}'))return false;
        ws(&j);if(j.p!=j.end || (seen&9)!=9)return false;
    }
    return true;
}
bool ls_detector_parse(const char *text,size_t len) {
    ls_detector_init();lock();unsigned n=0;
    bool ok=text && len<=LS_DETECTOR_BYTES && parse(text,len,&n);
    if(ok) {memcpy(rules,staging,n*sizeof(*rules));state.rules=n;state.revision++;memset(cooldown,0,sizeof(cooldown));memset(rule_window,0,sizeof(rule_window));state.error[0]=0;}
    else snprintf(state.error,sizeof(state.error),"Rules rejected: invalid/unsupported v1 schema; previous rules retained");
    unlock();return ok;
}
void ls_detector_init(void) {
    unsigned e=0;
    if(__atomic_compare_exchange_n(&once,&e,1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) {
        mutex=xSemaphoreCreateMutexStatic(&lock_mem);
#ifdef ESP_PLATFORM
        esp_fill_random(secret,sizeof(secret));esp_fill_random(&boot_id,sizeof(boot_id));
#else
        secret[0]=UINT64_C(0x123456789abcdef);secret[1]=UINT64_C(0xfedcba9876543210);boot_id=UINT64_C(0x1020304050607080);
#endif
        state.sources=15;state.sinks=DET_BANNER|DET_VIBRATE;reload_requested=true;
        unsigned n=0;if(parse(defaults,strlen(defaults),&n)) {memcpy(rules,staging,n*sizeof(*rules));state.rules=n;state.revision=1;}
        __atomic_store_n(&once,2,__ATOMIC_RELEASE);
    } else while(__atomic_load_n(&once,__ATOMIC_ACQUIRE)!=2)vTaskDelay(1);
}
/* SipHash-2-4, one fixed eight-byte endpoint/address input. Secret never logs. */
static uint64_t rot(uint64_t x,unsigned n) {return (x<<n)|(x>>(64-n));}
#define ROUND() do {v0+=v1;v1=rot(v1,13);v1^=v0;v0=rot(v0,32);v2+=v3;v3=rot(v3,16);v3^=v2;v0+=v3;v3=rot(v3,21);v3^=v0;v2+=v1;v1=rot(v1,17);v1^=v2;v2=rot(v2,32);}while(0)
static uint64_t pseudonym(const uint8_t *mac,uint8_t type,uint8_t source) {
    uint64_t m=type|((uint64_t)(source==DET_WIFI)<<8);
    for(unsigned i=0;i<6;i++)m|=(uint64_t)mac[i]<<(16+i*8);
    uint64_t v0=secret[0]^UINT64_C(0x736f6d6570736575),v1=secret[1]^UINT64_C(0x646f72616e646f6d),v2=secret[0]^UINT64_C(0x6c7967656e657261),v3=secret[1]^UINT64_C(0x7465646279746573);
    v3^=m;ROUND();ROUND();v0^=m;m=UINT64_C(8)<<56;v3^=m;ROUND();ROUND();v0^=m;v2^=255;
    ROUND();ROUND();ROUND();ROUND();return v0^v1^v2^v3;
}
#undef ROUND
bool ls_detector_running(void) {return __atomic_load_n(&state.running,__ATOMIC_ACQUIRE);}
bool ls_detector_pending(void) {ls_detector_init();lock();bool b=state.running || reload_requested || test_requested || (HAS_DRONELINK && ls_dronelink_pending());unlock();return b;}
void ls_detector_start(bool on) {
    ls_detector_init();ls_sweep_init();lock();
    __atomic_store_n(&state.running,on,__ATOMIC_RELEASE);head=count=0;memset(duplicates,0,sizeof(duplicates));
    unlock();ls_sweep_wake();
}
void ls_detector_request(bool test) {ls_detector_init();lock();if(test)test_requested=true;else reload_requested=true;unlock();ls_sweep_wake();}
void ls_detector_policy(uint8_t sources,uint8_t sinks) {ls_detector_init();lock();state.sources=sources&15;state.sinks=sinks&7;unlock();ls_sweep_wake();}
static void status_locked(ls_detector_status_t *out) {*out=state;snprintf(out->lora,sizeof(out->lora),"%s",HAS_DRONELINK?ls_dronelink_status():"UNAVAILABLE");out->queued=count;out->recent=history_n;out->dropped=__atomic_load_n(&dropped,__ATOMIC_RELAXED);}
void ls_detector_status(ls_detector_status_t *out) {ls_detector_init();lock();status_locked(out);unlock();}
bool ls_detector_snapshot(ls_detector_status_t *out,ls_detector_event_t *events,size_t cap,size_t *n) {
    if(!mutex || !xSemaphoreTake(mutex,0))return false;
    status_locked(out);
    *n=history_n<cap?history_n:cap;
    if(events && *n)memcpy(events,history,*n*sizeof(*events));
    unlock();return true;
}
size_t ls_detector_history(ls_detector_event_t *out,size_t cap) {ls_detector_init();lock();size_t n=history_n<cap?history_n:cap;memcpy(out,history,n*sizeof(*out));unlock();return n;}
bool ls_detector_rule(unsigned i,ls_detector_rule_t *out) {ls_detector_init();lock();bool ok=i<state.rules;if(ok)*out=rules[i];unlock();return ok;}
bool ls_detector_rule_snapshot(unsigned i,ls_detector_rule_t *out) {
    if(!mutex || !xSemaphoreTake(mutex,0))return false;
    bool ok=i<state.rules;if(ok)*out=rules[i];else memset(out,0,sizeof(*out));unlock();return ok;
}
bool ls_detector_rule_set(unsigned i,const ls_detector_rule_t *r) {
    ls_detector_init();lock();bool ok=i<state.rules && !strcmp(rules[i].id,r->id) && r->cooldown_s<=86400 && r->rule_cooldown_s<=86400 && !(r->sinks&~7);
    if(ok) {rules[i].enabled=r->enabled;rules[i].cooldown_s=r->cooldown_s;rules[i].rule_cooldown_s=r->rule_cooldown_s;rules[i].sinks=r->sinks;state.revision++;}
    unlock();return ok;
}
void ls_detector_observe(const uint8_t mac[6],uint8_t type,uint8_t source,uint8_t category,int rssi,bool validated,bool flood,uint8_t section,int64_t now) {
    if(!ls_detector_running() || !mac || now<0 || (source!=1 && source!=2 && source!=4))return;
    if(!xSemaphoreTake(mutex,0)) {__atomic_fetch_add(&dropped,1,__ATOMIC_RELAXED);return;}
    state.observations++;
    if(state.sources&source) {
        uint64_t entity=pseudonym(mac,type,source);
        for(unsigned i=0;i<64;i++)if(duplicates[i].source==source && duplicates[i].entity==entity && duplicates[i].at==now && duplicates[i].category==category && duplicates[i].validated==validated && duplicates[i].section==section) {state.suppressed++;unlock();return;}
        duplicates[duplicate_at++%64]=(duplicate_t){entity,now,source,category,section,validated};
        if(count==64)__atomic_fetch_add(&dropped,1,__ATOMIC_RELAXED);
        else {queue[(head+count++)%64]=(observation_t){.entity=entity,.at=now,.source=source,.category=category,.section=section,.rssi=rssi<-127?-127:rssi>0?0:rssi,.validated=validated,.flood=flood};}
    }
    unlock();
}
void ls_detector_drone(unsigned band,unsigned channel,unsigned rate,int rssi,unsigned hits,int64_t now) {
    if(!ls_detector_running() || (band!=868 && band!=915) || now<0)return;
    if(!xSemaphoreTake(mutex,0)) {__atomic_fetch_add(&dropped,1,__ATOMIC_RELAXED);return;}
    state.observations++;
    if(state.sources&DET_LORA) {
        if(count==64)__atomic_fetch_add(&dropped,1,__ATOMIC_RELAXED);
        else queue[(head+count++)%64]=(observation_t){.entity=((uint64_t)band<<32)|channel,
            .at=now,.source=DET_LORA,.category=SW_DRONE,.section=255,
            .rssi=rssi<-127?-127:rssi>0?0:rssi,.band=band,.rate_guess=rate,.hits_per_min=hits};
    }
    unlock();
}
static bool gate(unsigned rule,uint64_t entity,int64_t now,uint32_t seconds) {
    cooldown_t *slot=NULL;
    for(unsigned i=0;i<512;i++) {
        cooldown_t *c=&cooldown[i];
        if(c->until && c->entity==entity && c->rule==rule) {if(now<c->until)return false;slot=c;break;}
        if(!slot && c->until<=now)slot=c;
    }
    /* Saturation fails closed: never evict an unexpired cooldown. */
    if(!slot)return false;
    *slot=(cooldown_t){entity,now+(int64_t)seconds*1000000,rule};return true;
}
static bool token(int64_t now) {
    if(now>bucket_at) {int64_t add=(now-bucket_at)/2000000;if(add) {tokens=add>=4?4:tokens+(unsigned)add;if(tokens>4)tokens=4;bucket_at=now;}}
    if(!tokens)return false;
    tokens--;return true;
}
static void finish(ls_detector_event_t *e) {
    uint8_t effective;
    lock();effective=e->selected&state.sinks;e->allowed=effective;
    if(e->selected&~effective)state.suppressed++;
    e->suppressed=state.suppressed;unlock();
    e->delivered=ls_detector_deliver(e,effective&~DET_SD);
    e->failed=(effective&~DET_SD)&~e->delivered;
    if(effective&DET_SD) {
        e->delivered|=DET_SD;
        if(!ls_detector_write(e,boot_id)) {e->delivered&=~DET_SD;e->failed|=DET_SD;lock();snprintf(state.error,sizeof(state.error),"SD log unavailable / reserve or write failure");unlock();}
    }
    lock();if(history_n<LS_DETECTOR_HISTORY)history_n++;
    memmove(history+1,history,(history_n-1)*sizeof(*history));history[0]=*e;unlock();
}
void ls_detector_tick(void) {
    ls_detector_init();ls_detector_log_begin();unsigned batch_sequence=sequence;
    lock();bool reload=reload_requested,test=test_requested;reload_requested=test_requested=false;unlock();
    if(reload) {
        FILE *f=fopen(LS_DETECTOR_DIR "/rules.json","rb");
        if(f) {size_t n=fread(input,1,sizeof(input),f);bool ok=!ferror(f);fclose(f);ls_detector_parse(ok?input:NULL,n);}
        else {lock();snprintf(state.error,sizeof(state.error),"No SD rules file: using built-in rules");unlock();}
    }
    int64_t now=esp_timer_get_time();
    lock();bool lora=false;
    if(state.running && (state.sources&DET_LORA))for(unsigned i=0;i<state.rules;i++)
        if(rules[i].enabled && (rules[i].sources&DET_LORA))lora=true;
    unlock();if(HAS_DRONELINK)ls_dronelink_tick(lora,now);
    if(test) {lock();bool allow=token(now);uint8_t sinks=state.sinks;unlock();if(allow) {memset(&work_event,0,sizeof(work_event));work_event.at=now;work_event.sequence=++sequence;work_event.revision=state.revision;work_event.test=true;work_event.selected=sinks;work_event.category=255;snprintf(work_event.reason,sizeof(work_event.reason),"Operator sink test");finish(&work_event);}}
    for(unsigned budget=0;budget<64;budget++) {
        lock();if(!state.running || !count) {unlock();break;}
        observation_t o=queue[head];head=(head+1)%64;count--;
        ls_detector_event_t *e=&work_event;
        memset(e,0,sizeof(*e));e->entity=o.entity;e->at=o.at;e->source=o.source;e->category=o.category;e->rssi=o.rssi;e->band=o.band;e->rate_guess=o.rate_guess;e->hits_per_min=o.hits_per_min;e->validated=o.validated;e->sequence=++sequence;e->revision=state.revision;
        for(unsigned i=0;i<state.rules;i++) {
            const ls_detector_rule_t *r=&rules[i];
            if(!r->enabled || !(r->sources&o.source) || !(state.sources&o.source) ||
               (r->categories && (o.category>=SW_CATS || !(r->categories&(1u<<o.category)))) ||
               (r->bands && (o.source!=DET_LORA || !(r->bands&(o.band==868?1:2)))) ||
               (r->has_min_rssi && o.rssi<r->min_rssi_dbm) ||
               (r->min_hits_per_min && (o.source!=DET_LORA || o.hits_per_min<r->min_hits_per_min)) ||
               (r->sections && (o.section>2 || !(r->sections&(1u<<o.section)))) ||
               (r->has_validated && (o.source!=DET_RID || r->validated!=o.validated)))continue;
            if(now<rule_window[i].until || !gate(i,o.entity,now,r->cooldown_s)) {
                if(rule_window[i].pending<UINT32_MAX)rule_window[i].pending++;
                state.suppressed++;continue;
            }
            rule_window[i].until=now+(int64_t)r->rule_cooldown_s*1000000;
            e->coalesced=UINT32_MAX-e->coalesced<rule_window[i].pending?UINT32_MAX:e->coalesced+rule_window[i].pending;
            e->rules|=UINT64_C(1)<<i;e->selected|=r->sinks;
            size_t ids=strlen(e->rule_ids);snprintf(e->rule_ids+ids,sizeof(e->rule_ids)-ids,"%s\"%.31s\"",ids?",":"",r->id);
            size_t n=strlen(e->reason);snprintf(e->reason+n,sizeof(e->reason)-n,"%s%s",n?",":"",r->id);
        }
        if(!e->rules) {unlock();continue;}
        state.matches++;
        /* Floods and rotating addresses use the same rule window. No second
         * timer may flush alerts around that cap. Retain rate-limited matches. */
        if(!token(now)) {
            for(unsigned i=0;i<state.rules;i++)if(e->rules&(UINT64_C(1)<<i))
                if(rule_window[i].pending<UINT32_MAX)rule_window[i].pending++;
            state.suppressed++;unlock();continue;
        }
        for(unsigned i=0;i<state.rules;i++)if(e->rules&(UINT64_C(1)<<i))rule_window[i].pending=0;
        e->suppressed=state.suppressed;e->dropped=__atomic_load_n(&dropped,__ATOMIC_RELAXED);unlock();finish(e);
    }
    if(!ls_detector_log_end()) {
        lock();snprintf(state.error,sizeof(state.error),"SD batch flush failed; queued records may be incomplete");
        for(unsigned i=0;i<history_n;i++)if(history[i].sequence>batch_sequence && (history[i].delivered&DET_SD)) {history[i].delivered&=~DET_SD;history[i].failed|=DET_SD;}
        unlock();
    }
}
int ls_detector_command(int argc,char **argv) {
    if(argc!=2)return 1;
    if(!strcmp(argv[1],"start"))ls_detector_start(true);
    else if(!strcmp(argv[1],"stop"))ls_detector_start(false);
    else if(!strcmp(argv[1],"reload"))ls_detector_request(false);
    else if(!strcmp(argv[1],"test"))ls_detector_request(true);
    else if(!strcmp(argv[1],"status")) {
        ls_detector_status_t s;ls_detector_status(&s);ls_sweep_status_t rx;ls_sweep_status(&rx,esp_timer_get_time());
        ls_sweep_transport_stats_t transport;ls_sweep_transport_stats(&transport);
        printf("LoRa %s\n",s.lora);
        printf("BLE scan=%s\n",!transport.scanning?"off":transport.coded_phy?"extended 1M+Coded":"legacy 1M");
        printf("DETECTOR %s BLE %s Wi-Fi cached only; rules %u rev %u, matches %u, dropped %u, suppressed %u; %s\n",s.running?"RUNNING":"STOPPED",rx.running?"RUNNING":"UNAVAILABLE",s.rules,(unsigned)s.revision,(unsigned)s.matches,(unsigned)s.dropped,(unsigned)s.suppressed,s.error);
    } else return 1;
    return 0;
}
