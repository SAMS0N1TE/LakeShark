/* Clean-room implementation from public specifications: Bluetooth SIG company IDs, IEEE OUIs, ASTM F3411. */
#include "ls_sweep_app.h"
#include "ls_rid.h"
#include "ls_ble_ad.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define sweep_mkdir(p) _mkdir(p)
#else
#define sweep_mkdir(p) mkdir(p,0777)
#endif
#ifdef ESP_PLATFORM
#include "nvs.h"
#include "ls_nvs_safe.h"
#include "esp_heap_caps.h"
#endif
#ifndef LS_SWEEP_DIR
#define LS_SWEEP_DIR "/sdcard/sweep"
#endif
typedef struct { uint8_t kind, category, bytes[16]; char value[64], label[48], vendor[24]; } rule_t;
enum { R_OUI, R_COMPANY, R_NAME, R_UUID16, R_UUID128, R_SSID };
static EXT_RAM_BSS_ATTR rule_t rules[LS_SWEEP_RULES], staging[LS_SWEEP_RULES];
static EXT_RAM_BSS_ATTR char rule_text[LS_SWEEP_RULE_BYTES+1];
static EXT_RAM_BSS_ATTR ls_sweep_device_t table[LS_SWEEP_CAP];
static EXT_RAM_BSS_ATTR ls_sweep_device_t log_queue[8];
static unsigned log_count;
static bool reload_requested;
typedef struct { uint8_t mac[6], type; int64_t at; } recent_t;
static EXT_RAM_BSS_ATTR recent_t recent[16];
static StaticSemaphore_t lock_mem;
static SemaphoreHandle_t mutex;
static unsigned once, init_once, rule_count;
static uint32_t serial;
_Static_assert(sizeof(ls_sweep_settings_t)==20 && offsetof(ls_sweep_settings_t,on)==19,
               "SWEEP v3 must preserve the stored v1/v2 blob layout");
static ls_sweep_settings_t settings = {.version=3,.enabled=15,.receive_only=true,.alerts_muted=true};
static ls_sweep_status_t status;
static void lock(void);
static void unlock(void);
static ls_sweep_stats_t stats;
static EXT_RAM_BSS_ATTR recent_t nearby[LS_SWEEP_CAP];
static ls_sweep_device_t near_signal;
static uint32_t rate_count[60];
static int64_t rate_second[60];
static void advert(int64_t now) {
    int64_t sec=now/1000000; unsigned i=sec%60;
    if(rate_second[i]!=sec) {rate_second[i]=sec;rate_count[i]=0;}
    rate_count[i]++;if(now>status.last_advert_us) status.last_advert_us=now;
}
ls_sweep_section_t ls_sweep_section(const ls_sweep_device_t *d,int64_t now,bool gps) {
    if(d->match.category==SW_TRACKER && d->match.state==SW_STATE_SEPARATED &&
       ((d->with_you || d->seen_us-d->separated_us>=600000000) && (!gps || d->moved_m>=500 || d->places>=3))) return SW_WITH_YOU;
    return now-d->first_us<120000000?SW_NEW:SW_PASSING;
}
bool ls_sweep_alarm(const ls_sweep_device_t *d,bool gps) {return gps && d->with_you && (d->moved_m>=500 || d->places>=3);}
int ls_sweep_bucket(const ls_sweep_device_t *d,int64_t now,unsigned age) {
    if(age>=24) return -128;
    int64_t b=now/2500000-(int64_t)age;if(b<0) return -128;
    unsigned i=b%24;return d->bucket_adverts[i] && d->bucket[i]==b?d->history[i]:-128;
}
float ls_sweep_rate(const ls_sweep_device_t *d,int64_t now) {
    unsigned n=0;for(unsigned i=0;i<24;i++) if(d->bucket_adverts[i] && d->bucket[i]>=now/2500000-3 && d->bucket[i]<=now/2500000) n+=d->bucket_adverts[i];
    return n/10.0f;
}
float ls_sweep_trend(const ls_sweep_device_t *d,int64_t now) {
    int recent=ls_sweep_bucket(d,now,0),old=ls_sweep_bucket(d,now,4);
    return recent==-128 || old==-128?NAN:(float)(recent-old);
}
float ls_sweep_hunt_trend(const ls_sweep_device_t *d,int64_t now,bool slow) {
    int64_t b=now/2500000;
    if(b<4 || ls_sweep_bucket(d,now,0)==-128 || ls_sweep_bucket(d,now,4)==-128) return NAN;
    const float *v=slow?d->slow_history:d->fast_history;return v[b%24]-v[(b-4)%24];
}
void ls_sweep_motion(bool fix,float moved,unsigned places,int64_t now) {
    lock();status.gps_fix=fix;
    if(fix && isfinite(moved) && moved>=0) for(unsigned i=0;i<LS_SWEEP_CAP;i++) {
        ls_sweep_device_t *d=&table[i];
        if(d->serial && d->match.state==SW_STATE_SEPARATED && now-d->seen_us<60000000) {
            d->moved_m+=moved;d->places+=places;
            if(ls_sweep_section(d,now,true)==SW_WITH_YOU) d->with_you=true;
        }
    }
    unlock();
}
static void lock(void);
static void unlock(void);
#define UNIQUE_CAP 1024
static EXT_RAM_BSS_ATTR recent_t unique[UNIQUE_CAP];
/* Open addressing; expired slots can be reused after checking for a match.
 * This counts address/type pairs, including unclassified BLE traffic. */
static void note_address(const uint8_t mac[6],uint8_t type,int64_t now) {
    uint32_t h=type;for(unsigned i=0;i<6;i++) h=h*33+mac[i];
    recent_t *reuse=NULL;
    for(unsigned i=0;i<UNIQUE_CAP;i++) {
        recent_t *u=&unique[(h+i)%UNIQUE_CAP];
        if(u->at && u->type==type && !memcmp(u->mac,mac,6)) {if(now>u->at) u->at=now;return;}
        if(!reuse && (!u->at || now-u->at>=60000000)) reuse=u;
        if(!u->at) break;
    }
    if(!reuse) {stats.unique_saturated++;return;}
    memcpy(reuse->mac,mac,6);reuse->type=type;reuse->at=now?now:1;
}
static void expire(int64_t now) {
    for(unsigned i=0;i<LS_SWEEP_CAP;i++) if(table[i].serial && now-table[i].seen_us>=(table[i].with_you?INT64_C(900000000):LS_SWEEP_LIST_US)) {
        memset(&table[i],0,sizeof(table[i]));stats.expiries++;
    }
}
void ls_sweep_stats(ls_sweep_stats_t *out,int64_t now) {
    if(!out) return;
    lock();expire(now);*out=stats;out->capacity=LS_SWEEP_CAP;out->table_size=out->unique_addresses_min=0;
    for(unsigned i=0;i<LS_SWEEP_CAP;i++) out->table_size+=table[i].serial!=0;
    for(unsigned i=0;i<UNIQUE_CAP;i++) out->unique_addresses_min+=unique[i].at && now-unique[i].at<60000000;
    unlock();
}
__attribute__((weak)) void ls_sweep_transport_stats(ls_sweep_transport_stats_t *out) {memset(out,0,sizeof(*out));}
static int64_t flood_until, last_sound, hunt_sound, spam_window;
static unsigned spam_count, tracker_beeps;
static uint32_t hunt_consumed,near_adverts;
static int64_t tracker_window;
static bool storm;
static uint8_t queue[3];
static unsigned queued;
static void lock(void) {
    unsigned e=0;
    if(__atomic_compare_exchange_n(&once,&e,1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) {
        mutex=xSemaphoreCreateMutexStatic(&lock_mem); __atomic_store_n(&once,2,__ATOMIC_RELEASE);
    } else while(__atomic_load_n(&once,__ATOMIC_ACQUIRE)!=2) vTaskDelay(1);
    xSemaphoreTake(mutex,portMAX_DELAY);
}
static void unlock(void) { xSemaphoreGive(mutex); }
static unsigned le16(const uint8_t *p) { return p[0]|((unsigned)p[1]<<8); }
const char *ls_sweep_category(unsigned c) {
    static const char *const names[]={"CAMERA","BODYCAM","DRONE","TRACKER","ATTACK"};
    return c<SW_CATS?names[c]:"UNKNOWN";
}
static int category(const char *s) { for(int i=0;i<SW_CATS;i++) if(!strcmp(s,ls_sweep_category(i))) return i; return -1; }
static bool contains(const char *s,const char *v) {
    for(;*s;s++) { const char *a=s,*b=v; while(*a && *b && tolower((unsigned char)*a)==tolower((unsigned char)*b)) {a++;b++;} if(!*b) return true; }
    return !*v;
}
static void match(ls_sweep_match_t *o,unsigned c,const char *label,const char *vendor) {
    o->category=c; snprintf(o->label,sizeof(o->label),"%s",label); snprintf(o->vendor,sizeof(o->vendor),"%s",vendor);
}
static bool hexbytes(const char *s,uint8_t *out,size_t n) {
    size_t k=0; int high=-1;
    for(;*s;s++) { if(*s==':' || *s=='-') continue;
        int v=*s>='0' && *s<='9'?*s-'0':tolower((unsigned char)*s)>='a' && tolower((unsigned char)*s)<='f'?tolower((unsigned char)*s)-'a'+10:-1;
        if(v<0) return false;
        if(high<0) high=v; else { if(k>=n) return false; out[k++]=(high<<4)|v; high=-1; }
    } return k==n && high<0;
}
/* A deliberately small JSON dialect: strings only, no nesting except objects
 * in the root array. Reject unsupported escapes, duplicate keys and overflow. */
typedef struct { const char *p,*end; } json_t;
static void ws(json_t *j) { while(j->p<j->end && (*j->p==' ' || *j->p=='\n' || *j->p=='\r' || *j->p=='\t')) j->p++; }
static bool take(json_t *j,char c) { ws(j); if(j->p==j->end || *j->p!=c) return false; j->p++; return true; }
static bool string(json_t *j,char *s,size_t cap) {
    if(!take(j,'"')) return false;
    size_t n=0;
    while(j->p<j->end && *j->p!='"') {
        unsigned char c=*j->p++;
        if(c=='\\') { if(j->p==j->end) return false; c=*j->p++; if(c!='"' && c!='\\' && c!='/') return false; }
        if(c<32 || c>126 || n+1>=cap) return false;
        s[n++]=c;
    }
    s[n]=0; return take(j,'"');
}
static bool parse_rules(const char *text,size_t len,unsigned *count) {
    json_t j={text,text+len}; *count=0;
    if(!take(&j,'[')) return false;
    ws(&j); if(j.p<j.end && *j.p==']') {j.p++;ws(&j);return j.p==j.end;}
    do {
        if(*count>=LS_SWEEP_RULES || !take(&j,'{')) return false;
        rule_t *r=&staging[*count]; memset(r,0,sizeof(*r)); unsigned seen=0;
        do {
            char key[24],v[64]; if(!string(&j,key,sizeof(key)) || !take(&j,':') || !string(&j,v,sizeof(v))) return false;
            unsigned bit=0;
            if(!strcmp(key,"category")) {bit=1;int c=category(v);if(c<0) return false;r->category=c;}
            else if(!strcmp(key,"label")) {bit=2;if(!*v || strlen(v)>=sizeof(r->label)) return false;strcpy(r->label,v);}
            else if(!strcmp(key,"vendor")) {bit=4;if(strlen(v)>=sizeof(r->vendor)) return false;strcpy(r->vendor,v);}
            else {
                bit=8; if(!*v) return false; strcpy(r->value,v);
                if(!strcmp(key,"oui")) {r->kind=R_OUI;if(!hexbytes(v,r->bytes,3)) return false;}
                else if(!strcmp(key,"company")) {r->kind=R_COMPANY; if(!hexbytes(v,r->bytes,2)) return false;}
                else if(!strcmp(key,"name")) r->kind=R_NAME;
                else if(!strcmp(key,"ssid")) r->kind=R_SSID;
                else if(!strcmp(key,"uuid")) {
                    if(strlen(v)==4) {r->kind=R_UUID16;if(!hexbytes(v,r->bytes,2)) return false;}
                    else {r->kind=R_UUID128;if(!hexbytes(v,r->bytes,16)) return false;}
                } else return false;
            }
            if(seen&bit) return false;
            seen|=bit;
            ws(&j); if(j.p==j.end) return false; if(*j.p!=',') break; j.p++;
        } while(true);
        if((seen&11)!=11 || !take(&j,'}')) return false;
        (*count)++; ws(&j); if(j.p==j.end) return false; if(*j.p!=',') break; j.p++;
    } while(true);
    if(!take(&j,']')) return false;
    ws(&j); return j.p==j.end;
}
bool ls_sweep_rules_parse(const char *json,size_t len) {
    if(!json || len>LS_SWEEP_RULE_BYTES) return false;
    lock(); unsigned n=0; bool ok=parse_rules(json,len,&n);
    if(ok) {memcpy(rules,staging,n*sizeof(*rules));rule_count=n;}
    unlock();return ok;
}
static const char defaults[]=
    "[{\"category\":\"CAMERA\",\"label\":\"Axis vendor hint\",\"vendor\":\"Axis\",\"oui\":\"00408C\"},"
    "{\"category\":\"CAMERA\",\"label\":\"Axis vendor hint\",\"vendor\":\"Axis\",\"oui\":\"ACCC8E\"},"
    "{\"category\":\"TRACKER\",\"label\":\"Tile vendor advert\",\"vendor\":\"Tile\",\"company\":\"067C\"},"
    "{\"category\":\"TRACKER\",\"label\":\"Tile service\",\"vendor\":\"Tile\",\"uuid\":\"FEED\"},"
    "{\"category\":\"TRACKER\",\"label\":\"Tile service\",\"vendor\":\"Tile\",\"uuid\":\"FEEC\"},"
    "{\"category\":\"BODYCAM\",\"label\":\"Bodycam name hint\",\"vendor\":\"Unverified\",\"name\":\"bodycam\"},"
    "{\"category\":\"ATTACK\",\"label\":\"Flipper name hint\",\"vendor\":\"Unverified\",\"name\":\"Flipper\"},"
    "{\"category\":\"ATTACK\",\"label\":\"Pwnagotchi SSID hint\",\"vendor\":\"Unverified\",\"ssid\":\"pwnagotchi\"}]";
void ls_sweep_defaults(void) {ls_sweep_rules_parse(defaults,strlen(defaults));}
void ls_sweep_init(void) {
    unsigned e=0;
    if(__atomic_compare_exchange_n(&init_once,&e,1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) {
        ls_sweep_defaults();ls_sweep_settings_load();__atomic_store_n(&init_once,2,__ATOMIC_RELEASE);
    } else while(__atomic_load_n(&init_once,__ATOMIC_ACQUIRE)!=2) vTaskDelay(1);
}
void ls_sweep_rules_seed(void) {
    struct stat st;if(!stat(LS_SWEEP_DIR "/rules.json",&st)) return;
    if(sweep_mkdir(LS_SWEEP_DIR) && stat(LS_SWEEP_DIR,&st)) return;
    FILE *f=fopen(LS_SWEEP_DIR "/rules.json","wb");if(f) {fwrite(defaults,1,strlen(defaults),f);fclose(f);}
}
void ls_sweep_reload_request(void) {lock();reload_requested=true;unlock();ls_sweep_wake();}
bool ls_sweep_reload_pending(void) {lock();bool b=reload_requested;reload_requested=false;unlock();return b;}
bool ls_sweep_log_event(ls_sweep_device_t *out) {
    lock();bool ok=log_count>0 && settings.logging;
    if(ok) {*out=log_queue[0];memmove(log_queue,log_queue+1,--log_count*sizeof(*log_queue));}
    if(!settings.logging) log_count=0;
    unlock();return ok;
}
bool ls_sweep_rules_reload(void) {
    /* Serialise staging/file buffer with scan readers; no BLE task does SD IO. */
    lock(); FILE *f=fopen(LS_SWEEP_DIR "/rules.json","rb");
    if(!f) {unlock();return false;}
    size_t n=fread(rule_text,1,LS_SWEEP_RULE_BYTES+1,f); bool ok=!ferror(f) && n<=LS_SWEEP_RULE_BYTES; fclose(f);
    unsigned count=0; if(ok) ok=parse_rules(rule_text,n,&count);
    if(ok) {memcpy(rules,staging,count*sizeof(*rules));rule_count=count;}
    unlock();return ok;
}
static bool rule_ad(const rule_t *r,unsigned t,const uint8_t *p,size_t n,const char *name) {
    if(r->kind==R_NAME) return contains(name,r->value);
    if(r->kind==R_COMPANY) return t==0xff && n>=2 && p[0]==r->bytes[1] && p[1]==r->bytes[0];
    if(r->kind==R_UUID16) {
        if(t==0x16) return n>=2 && p[0]==r->bytes[1] && p[1]==r->bytes[0];
        if(t==2 || t==3) for(size_t i=0;i+1<n;i+=2) if(p[i]==r->bytes[1] && p[i+1]==r->bytes[0]) return true;
    }
    if(r->kind==R_UUID128 && (t==6 || t==7 || t==0x21)) {
        for(size_t i=0;i+15<n;i+=16) {bool hit=true;for(int k=0;k<16;k++) if(p[i+k]!=r->bytes[15-k]) hit=false;if(hit) return true;if(t==0x21) break;}
    } return false;
}
bool ls_sweep_parse_ble(const uint8_t *ad,size_t len,ls_sweep_match_t *out) {
    if(!ad || !out || len>LS_BLE_AD_MAX) return false;
    memset(out,0,sizeof(*out)); char name[64]=""; bool samsung=false,found=false;int dult_state=-1;
    /* Validate the complete envelope before trusting any field. */
    if(!ls_ble_ad_valid(ad,len)) return false;
    size_t off=0; ls_ble_ad_field_t field;
    while(ls_ble_ad_next(ad,len,&off,&field)>0) {
        size_t n=field.len;unsigned t=field.type;const uint8_t *p=field.data;
        if((t==2 || t==3) && n%2) return false;
        if((t==4 || t==5) && n%4) return false;
        if((t==6 || t==7) && n%16) return false;
        if(t==8 || t==9) {size_t k=n<sizeof(name)-1?n:sizeof(name)-1;for(size_t a=0;a<k;a++) name[a]=p[a]>=32 && p[a]<127?p[a]:'?';name[k]=0;}
        if(t==0xff && n>=2 && le16(p)==0x75) samsung=true;
        if(t==0x16 && n>=4 && le16(p)==0xfcb2) dult_state=(p[3]&1)?SW_STATE_NEAR:SW_STATE_SEPARATED;
        if((t==2 || t==3 || t==0x16) && n>=2) for(size_t a=0;a+1<n;a+=2) {if(le16(p+a)==0x1802) out->immediate_alert=true;if(t==0x16) break;}
    }
    off=0;
    while(ls_ble_ad_next(ad,len,&off,&field)>0) {
        unsigned t=field.type;const uint8_t *p=field.data;size_t n=field.len;
        if(t==0x16 && n>=4 && le16(p)==0xfcb2) {
            match(out,SW_TRACKER,"DULT tag","DULT");out->state=(p[3]&1)?SW_STATE_NEAR:SW_STATE_SEPARATED;found=true;break;
        }
        if(t==0xff && (n==29 || n==6) && le16(p)==0x4c && p[2]==0x12 && p[3]==n-4) {
            match(out,SW_TRACKER,"Apple Find My","Apple");out->state=(n==6 || (p[4]&0x04))?SW_STATE_NEAR:SW_STATE_SEPARATED;found=true;break;
        }
        if(t==0x16 && (n==23 || n==24 || n==35 || n==36) && le16(p)==0xfeaa &&
           (p[2]==0x40 || p[2]==0x41) && (n==23 || n==24 || n==35 || n==36)) {
            match(out,SW_TRACKER,"Google Find Hub","Google");out->state=p[2]==0x41?SW_STATE_SEPARATED:SW_STATE_NEAR;found=true;break;
        }
    }
    if(!found && samsung && contains(name,"SmartTag")) {match(out,SW_TRACKER,"SmartTag name + company","Samsung");found=true;}
    if(found) {if(dult_state>=0) out->state=dult_state;out->immediate_alert=false;return true;} /* Never ring network tags. */
    lock();
    for(unsigned r=0;r<rule_count && !found;r++) {
        off=0;
        while(ls_ble_ad_next(ad,len,&off,&field)>0)
            if(rule_ad(&rules[r],field.type,field.data,field.len,name)) {match(out,rules[r].category,rules[r].label,rules[r].vendor);found=true;break;}
    }
    unlock();return found;
}
bool ls_sweep_parse_wifi(const uint8_t mac[6],const char *ssid,ls_sweep_match_t *out) {
    if(!mac || !ssid || !out || strlen(ssid)>32) return false;
    memset(out,0,sizeof(*out));bool found=false;lock();
    for(unsigned r=0;r<rule_count;r++) if((rules[r].kind==R_OUI && !(mac[0]&2) && !memcmp(mac,rules[r].bytes,3)) ||
        (rules[r].kind==R_SSID && contains(ssid,rules[r].value))) {match(out,rules[r].category,rules[r].label,rules[r].vendor);found=true;break;}
    unlock();return found;
}
void ls_sweep_settings_get(ls_sweep_settings_t *o) {if(o) {lock();*o=settings;unlock();}}
#ifdef ESP_PLATFORM
typedef struct {bool save;ls_sweep_settings_t value;} nvs_job_t;
static esp_err_t nvs_job(void *arg) {
    nvs_job_t *j=arg;nvs_handle_t h;esp_err_t e=nvs_open("sweep",j->save?NVS_READWRITE:NVS_READONLY,&h);
    if(e!=ESP_OK) return e;
    ls_sweep_settings_blob_t blob={.magic=SWEEP_SETTINGS_MAGIC,.build=SWEEP_SETTINGS_BUILD,.value=j->value};
    size_t n=sizeof(blob);
    if(j->save) {e=nvs_set_blob(h,"settings_v1",&blob,n);if(e==ESP_OK) e=nvs_commit(h);}
    else {
        e=nvs_get_blob(h,"settings_v1",&blob,&n);
        if(e==ESP_OK && (n!=sizeof(blob) || blob.magic!=SWEEP_SETTINGS_MAGIC || blob.build!=SWEEP_SETTINGS_BUILD)) e=ESP_ERR_INVALID_SIZE;
        if(e==ESP_OK) j->value=blob.value;
    }
    nvs_close(h);return e;
}
#endif
static bool valid_settings(const ls_sweep_settings_t *o) {
    if(!o) return false;
    const unsigned char *raw=(const unsigned char *)o;
    return (o->version>=1 && o->version<=3) && o->enabled<32 && o->muted<32 && o->filter<=SW_CATS && o->hunt_radio<=1 && o->hunt_type<=3 &&
        raw[offsetof(ls_sweep_settings_t,logging)]<=1 && raw[offsetof(ls_sweep_settings_t,receive_only)]<=1 && raw[offsetof(ls_sweep_settings_t,hunt)]<=1 &&
        (o->version==1 || raw[offsetof(ls_sweep_settings_t,alerts_muted)]<=1) &&
        (o->version<3 || raw[offsetof(ls_sweep_settings_t,on)]<=1);
}
static bool settings_save(const ls_sweep_settings_t *value) {
#ifdef ESP_PLATFORM
    nvs_job_t j={.save=true,.value=*value};
    if(ls_nvs_run(nvs_job,&j,0)!=ESP_OK) {lock();snprintf(status.status,sizeof(status.status),"Settings active / NVS save failed");unlock();return false;}
#endif
    return true;
}
bool ls_sweep_settings_set(const ls_sweep_settings_t *o) {
    if(!valid_settings(o)) return false;
    ls_sweep_settings_t value=*o;
    if(value.version==1) value.alerts_muted=false;
    if(value.version<3) {lock();value.on=settings.on;unlock();}
    value.version=3;value.receive_only=true;
    lock(); settings=value;if(settings.alerts_muted) {
        queued=0;
        if(settings.hunt && near_signal.adverts && !memcmp(near_signal.mac,settings.hunt_mac,6)) hunt_consumed=near_signal.adverts;
        for(unsigned i=0;i<LS_SWEEP_CAP;i++) if(settings.hunt && table[i].serial && table[i].radio==settings.hunt_radio && table[i].address_type==settings.hunt_type && !memcmp(table[i].mac,settings.hunt_mac,6)) hunt_consumed=table[i].adverts;
    }unlock();
    return settings_save(&value);
}
void ls_sweep_settings_load(void) {
#ifdef ESP_PLATFORM
    lock();settings=(ls_sweep_settings_t){.version=3,.enabled=15,.receive_only=true,.alerts_muted=true};
    status.requested=false;queued=0;unlock();
    nvs_job_t j={0};if(ls_nvs_run(nvs_job,&j,0)==ESP_OK && valid_settings(&j.value)) {
        if(j.value.version==1) j.value.alerts_muted=false; /* Ignore old padding. */
        if(j.value.version<3) j.value.on=false; /* Ignore former padding. */
        j.value.version=3;lock();settings=j.value;settings.receive_only=true;
        status.requested=settings.on;
        if(settings.on) status.last_advert_us=esp_timer_get_time();
        unlock();ls_sweep_wake();
    }
#endif
}
__attribute__((weak)) void ls_sweep_wake(void) {}
void ls_sweep_start(bool on) {
    lock();bool changed=settings.on!=on;settings.on=on;
    ls_sweep_settings_t value=settings;
    if(on && !status.requested) status.last_advert_us=esp_timer_get_time();
    status.requested=on;
    if(!on) {queued=log_count=0;storm=false;tracker_beeps=0;memset(table,0,sizeof(table));memset(recent,0,sizeof(recent));memset(nearby,0,sizeof(nearby));memset(&near_signal,0,sizeof(near_signal));memset(rate_count,0,sizeof(rate_count));}
    unlock();
    /* Save the captured preference without reapplying an old settings snapshot. */
    if(changed) (void)settings_save(&value);
    ls_sweep_wake();
}
bool ls_sweep_pending(void) {lock();bool b=status.requested || status.running || reload_requested;unlock();return b;}
void ls_sweep_runtime_status(bool on,const char *s) {lock();status.running=on;snprintf(status.status,sizeof(status.status),"%s",s);unlock();}
static void recover(int64_t now) {
    if(storm && now>=flood_until) {
        storm=false;tracker_beeps=0;memset(recent,0,sizeof(recent));
        for(unsigned i=0;i<LS_SWEEP_CAP;i++) if(table[i].match.category==SW_TRACKER) table[i].alerted_us=0;
    }
}
void ls_sweep_status(ls_sweep_status_t *out,int64_t now) {
    if(out) {
        lock();recover(now);
        expire(now);
        for(unsigned i=0;i<16;i++) if(recent[i].at && now-recent[i].at>=60000000) memset(&recent[i],0,sizeof(recent[i]));
        status.owner_nearby=0;
        for(unsigned i=0;i<LS_SWEEP_CAP;i++) status.owner_nearby+=nearby[i].at && now-nearby[i].at<60000000;
        unsigned n=0;for(unsigned i=0;i<60;i++) if(rate_count[i] && rate_second[i]>now/1000000-10 && rate_second[i]<=now/1000000) n+=rate_count[i];
        status.adverts_s=n/10.0f;
        *out=status;out->flood=storm;out->rules=rule_count;unlock();
    }
}
static bool novel(const uint8_t *mac,unsigned type,int64_t now) {
    recent_t *slot=&recent[0];unsigned distinct=0;
    for(unsigned i=0;i<16;i++) {
        if(recent[i].at && recent[i].type==type && !memcmp(mac,recent[i].mac,6)) {recent[i].at=now?now:1;return false;}
        if(recent[i].at && now-recent[i].at<3000000) distinct++;
        if(recent[i].at<slot->at) slot=&recent[i];
    }
    memcpy(slot->mac,mac,6);slot->type=type;slot->at=now?now:1;
    if(distinct>=11) {if(!storm) stats.flood_entries++;storm=true;flood_until=now+5000000;queued=0;}
    if(storm) flood_until=now+5000000;
    return true;
}
static void signal_sample(ls_sweep_device_t *slot,int64_t now,int64_t dt) {
    if(slot->samples==24) memmove(slot->trend,slot->trend+1,23);else slot->samples++;
    slot->trend[slot->samples-1]=slot->rssi;
    slot->adverts++;
    int64_t bucket=now/2500000;unsigned bi=bucket%24;
    if(!slot->bucket_adverts[bi] || slot->bucket[bi]!=bucket) {slot->bucket[bi]=bucket;slot->bucket_adverts[bi]=0;}
    slot->history[bi]=slot->rssi;if(slot->bucket_adverts[bi]<UINT16_MAX) slot->bucket_adverts[bi]++;
    slot->median[slot->median_at]=slot->rssi;slot->median_at=(slot->median_at+1)%5;if(slot->median_n<5) slot->median_n++;
    int8_t values[5];memcpy(values,slot->median,slot->median_n);
    for(unsigned i=1;i<slot->median_n;i++) {int8_t v=values[i];unsigned j=i;while(j && values[j-1]>v) {values[j]=values[j-1];j--;}values[j]=v;}
    float med=values[slot->median_n/2];
    if(slot->adverts==1) slot->fast=slot->slow=med;
    else {float seconds=fmaxf(0,dt/1e6f);slot->fast+=(1-expf(-seconds/2))*(med-slot->fast);slot->slow+=(1-expf(-seconds/8))*(med-slot->slow);}
    slot->fast_history[bi]=slot->fast;slot->slow_history[bi]=slot->slow;
    if(slot->adverts==1 || slot->slow>=slot->slow_peak || now-slot->slow_peak_us>=15000000) {slot->slow_peak=slot->slow;slot->slow_peak_us=now;}
    if(slot->adverts==1 || slot->fast>=slot->peak || now-slot->peak_us>=15000000) {slot->peak=slot->fast;slot->peak_us=now;}

}
bool ls_sweep_nearby_target(ls_sweep_device_t *out,int64_t now) {
    if(!out) return false;
    lock();bool live=near_signal.adverts && now-near_signal.seen_us<60000000;
    if(live) *out=near_signal;
    unlock();return live;
}
void ls_sweep_observe(const uint8_t mac[6],uint8_t radio,uint8_t type,int rssi,const ls_sweep_match_t *m,int64_t now) {
    if(!mac || !m || m->category>=SW_CATS || now<0 || radio>1 || type>3) return;
    lock();recover(now);
    if(!status.requested || !(settings.enabled&(1u<<m->category))) {unlock();return;}
    stats.accepted[m->category]++;
    if(m->category==SW_TRACKER && m->state==SW_STATE_NEAR) {
        recent_t *slot=&nearby[0];
        for(unsigned i=0;i<LS_SWEEP_CAP;i++) {
            if(nearby[i].at && nearby[i].type==type && !memcmp(nearby[i].mac,mac,6)) {slot=&nearby[i];break;}
            if(nearby[i].at<slot->at) slot=&nearby[i];
        }
        memcpy(slot->mac,mac,6);slot->type=type;slot->at=now?now:1;
        for(unsigned i=0;i<LS_SWEEP_CAP;i++) if(table[i].serial && table[i].radio==radio && table[i].address_type==type && !memcmp(table[i].mac,mac,6)) memset(&table[i],0,sizeof(table[i]));
        bool same=near_signal.adverts && near_signal.radio==radio && near_signal.address_type==type && !memcmp(near_signal.mac,mac,6);
        int64_t dt=same?now-near_signal.seen_us:0;
        if(!same) {memset(&near_signal,0,sizeof(near_signal));near_signal.serial=UINT32_MAX;near_signal.first_us=now;}
        memcpy(near_signal.mac,mac,6);near_signal.radio=radio;near_signal.address_type=type;
        near_signal.match=*m;near_signal.rssi=rssi<-127?-127:rssi>0?0:rssi;near_signal.seen_us=now;
        signal_sample(&near_signal,now,dt);near_signal.adverts=++near_adverts;
        unlock();return;
    }
    /* A crowded room is not evidence of spoofing. Quiet audio only. */
    if(m->category==SW_TRACKER) novel(mac,(radio<<4)|type,now);
    ls_sweep_device_t *slot=NULL,*old=&table[0];
    for(unsigned i=0;i<LS_SWEEP_CAP;i++) {
        ls_sweep_device_t *d=&table[i];
        if(d->serial && now-d->seen_us>=(d->with_you?INT64_C(900000000):LS_SWEEP_LIST_US)) {memset(d,0,sizeof(*d));stats.expiries++;}
        if(d->serial && d->radio==radio && d->address_type==type && !memcmp(d->mac,mac,6)) {slot=d;break;}
        if(!d->serial && !slot) slot=d;
        if(d->seen_us<old->seen_us) old=d;
    }
    if(!slot) {slot=old;stats.evicted_capacity++;}
    if(!slot->serial || slot->radio!=radio || slot->address_type!=type || memcmp(slot->mac,mac,6)) {
        memset(slot,0,sizeof(*slot));slot->serial=++serial;memcpy(slot->mac,mac,6);slot->radio=radio;slot->address_type=type;slot->first_us=slot->separated_us=now;
    } else {
        /* Delayed ring reports never age or reclassify a contact backwards. */
        if(now<slot->seen_us) {unlock();return;}
        if(slot->match.category!=m->category) stats.category_changes++;
    }
    if(slot->match.state!=m->state) {slot->separated_us=now;slot->with_you=false;slot->moved_m=0;slot->places=0;}
    int64_t dt=now-slot->seen_us;
    slot->match=*m;slot->rssi=rssi<-127?-127:rssi>0?0:rssi;slot->seen_us=now;
    signal_sample(slot,now,dt);
    if(ls_sweep_section(slot,now,status.gps_fix)==SW_WITH_YOU) slot->with_you=true;
    if(!slot->alerted_us || now-slot->alerted_us>=30000000) {
        if(settings.logging && log_count<8) log_queue[log_count++]=*slot;
        if(!storm && now-tracker_window>=3000000) {tracker_window=now;tracker_beeps=0;}
        if(!settings.alerts_muted && queued<3 && (m->category!=SW_TRACKER || (!storm && tracker_beeps<3))) {queue[queued++]=m->category;if(m->category==SW_TRACKER) tracker_beeps++;}
        else {status.dropped++;stats.alerts_suppressed++;}
        slot->alerted_us=now?now:1;
    }
    unlock();
}
void ls_sweep_ble(const uint8_t mac[6],uint8_t type,int rssi,const uint8_t *ad,size_t len,int64_t now) {
    if(!mac || !ad || type>3 || now<0) return;
    if(!ls_sweep_pending()) return;
    lock();stats.adverts_in++;advert(now);note_address(mac,type,now);unlock();
    ls_sweep_match_t m;
    if(ls_rid_is_advert(ad,len)) {
        /* The BLE owner already feeds RID. Validate through its receiver before
         * emitting a drone classification (malformed envelopes stay silent). */
        if(ls_rid_advert(mac,type,rssi,ad,len,now)) {memset(&m,0,sizeof(m));match(&m,SW_DRONE,"ASTM Remote ID","Unverified");ls_sweep_observe(mac,0,type,rssi,&m,now);}return;
    }
    bool hit=ls_sweep_parse_ble(ad,len,&m);
    if(hit && m.state==SW_STATE_NEAR) {ls_sweep_observe(mac,0,type,rssi,&m,now);return;}
    ls_sweep_settings_t o;ls_sweep_settings_get(&o);
    if(o.enabled&(1<<SW_ATTACK)) {
        lock();if(now-spam_window>=1000000) {spam_window=now;spam_count=0;}bool spam=++spam_count>=200;unlock();
        if(spam && !hit) {
            /* Collapse traffic-rate heuristics to one row, not one per spoofed MAC. */
            static const uint8_t burst[6]={0xff,0xff,0xff,0xff,0xff,0xff};
            memset(&m,0,sizeof(m));match(&m,SW_ATTACK,"BLE advert burst (heuristic)","Unverified");ls_sweep_observe(burst,0,0,rssi,&m,now);return;
        }
    }
    if(hit) ls_sweep_observe(mac,0,type,rssi,&m,now);
}
void ls_sweep_wifi(const uint8_t mac[6],const char *ssid,int rssi,int64_t now) {if(!mac || !ssid || now<0 || !ls_sweep_pending()) return;lock();if(now>status.last_advert_us) status.last_advert_us=now;unlock();ls_sweep_match_t m;if(ls_sweep_parse_wifi(mac,ssid,&m)) ls_sweep_observe(mac,1,0,rssi,&m,now);}
/* Small coherent summary: no table copy, sorting, allocation or persistence. */
void ls_sweep_summary(ls_sweep_summary_t *out,int64_t now) {
    memset(out,0,sizeof(*out));lock();expire(now);
    out->running=status.running;out->muted=settings.alerts_muted;
    const ls_sweep_device_t *top[2]={NULL,NULL};
    for(unsigned i=0;i<LS_SWEEP_CAP;i++) {
        const ls_sweep_device_t *d=&table[i];
        if(!d->serial || d->match.category>=SW_CATS || !(settings.enabled&(1<<d->match.category)) ||
           (settings.filter && settings.filter!=d->match.category+1)) continue;
        out->counts[d->match.category]++;
        if(!top[0] || d->rssi>top[0]->rssi || (d->rssi==top[0]->rssi && d->serial<top[0]->serial)) {top[1]=top[0];top[0]=d;}
        else if(!top[1] || d->rssi>top[1]->rssi || (d->rssi==top[1]->rssi && d->serial<top[1]->serial)) top[1]=d;
    }
    for(unsigned i=0;i<2;i++) if(top[i]) {
        out->serial[i]=top[i]->serial;out->rssi[i]=top[i]->rssi;out->category[i]=top[i]->match.category;
    }
    unlock();
}
size_t ls_sweep_snapshot(ls_sweep_device_t *out,size_t cap,int64_t now) {
    if(!out) return 0;
    lock();recover(now);size_t n=0;
    for(unsigned i=0;i<LS_SWEEP_CAP;i++) {
        ls_sweep_device_t *d=&table[i];
        if(d->serial && now-d->seen_us>=(d->with_you?INT64_C(900000000):LS_SWEEP_LIST_US)) {memset(d,0,sizeof(*d));stats.expiries++;}
        if(d->serial && n<cap && (settings.enabled&(1<<d->match.category)) && (!settings.filter || settings.filter==d->match.category+1)) out[n++]=*d;
    }
    for(size_t i=1;i<n;i++) {ls_sweep_device_t d=out[i];size_t j=i;while(j && (ls_sweep_section(&d,now,status.gps_fix)<ls_sweep_section(&out[j-1],now,status.gps_fix) ||
        (ls_sweep_section(&d,now,status.gps_fix)==ls_sweep_section(&out[j-1],now,status.gps_fix) &&
         (d.rssi>out[j-1].rssi || (d.rssi==out[j-1].rssi && d.serial<out[j-1].serial))))) {out[j]=out[j-1];j--;}out[j]=d;}
    unlock();return n;
}
bool ls_sweep_hunt(const uint8_t mac[6],uint8_t radio,uint8_t type) {
    if(radio>1 || type>3) return false;
    ls_sweep_settings_t o;ls_sweep_settings_get(&o);o.hunt=mac!=NULL;if(mac) memcpy(o.hunt_mac,mac,6);o.hunt_radio=radio;o.hunt_type=type;
    lock();hunt_sound=0;hunt_consumed=0;
    if(mac && near_signal.adverts && near_signal.radio==radio && near_signal.address_type==type && !memcmp(near_signal.mac,mac,6)) hunt_consumed=near_signal.adverts;
    for(unsigned i=0;i<LS_SWEEP_CAP;i++) if(mac && table[i].serial && table[i].radio==radio && table[i].address_type==type && !memcmp(table[i].mac,mac,6)) hunt_consumed=table[i].adverts;
    unlock();return ls_sweep_settings_set(&o);
}
int ls_sweep_alert(int64_t now) {
    lock();recover(now);int c=-1;
    if(!status.requested || settings.alerts_muted) {
        queued=0;
        if(settings.hunt && near_signal.adverts && !memcmp(near_signal.mac,settings.hunt_mac,6)) hunt_consumed=near_signal.adverts;
        for(unsigned i=0;i<LS_SWEEP_CAP;i++) if(settings.hunt && table[i].serial && table[i].radio==settings.hunt_radio && table[i].address_type==settings.hunt_type && !memcmp(table[i].mac,settings.hunt_mac,6)) hunt_consumed=table[i].adverts;
        unlock();return -1;
    }
    if(settings.hunt) for(unsigned i=0;i<LS_SWEEP_CAP;i++) {
        ls_sweep_device_t *d=&table[i];
        if(!d->serial || d->radio!=settings.hunt_radio || d->address_type!=settings.hunt_type || memcmp(d->mac,settings.hunt_mac,6) || now-d->seen_us>5000000 || (storm && d->match.category==SW_TRACKER)) continue;
        /* Each advert may produce one click, never a timer pulse. */
        if(d->adverts!=hunt_consumed) {hunt_consumed=d->adverts;hunt_sound=d->seen_us;c=d->match.category;}break;
    }
    if(settings.hunt) {
        ls_sweep_device_t *d=&near_signal;
        if(d->adverts && d->radio==settings.hunt_radio && d->address_type==settings.hunt_type && !memcmp(d->mac,settings.hunt_mac,6) && now-d->seen_us<5000000 && d->adverts!=hunt_consumed) {hunt_consumed=d->adverts;c=d->match.category;}
        queued=0;unlock();return c>=0 && !(settings.muted&(1<<c))?c:-1;}
    if(c<0 && queued && now-last_sound>=700000) {c=queue[0];memmove(queue,queue+1,--queued);last_sound=now;}
    if(c>=0 && (!(settings.enabled&(1<<c)) || (settings.muted&(1<<c)) || (storm && c==SW_TRACKER))) c=-1;
    unlock();return c;
}
void ls_sweep_clear(void) {lock();memset(table,0,sizeof(table));memset(recent,0,sizeof(recent));memset(unique,0,sizeof(unique));memset(nearby,0,sizeof(nearby));memset(&near_signal,0,sizeof(near_signal));memset(rate_count,0,sizeof(rate_count));status.last_advert_us=0;status.gps_fix=false;queued=log_count=0;storm=false;flood_until=last_sound=hunt_sound=tracker_window=0;tracker_beeps=spam_count=0;unlock();}
static void mac_text(char *s,const ls_sweep_device_t *d) {
    const uint8_t *p=d->mac;snprintf(s,18,"%02X:%02X:%02X:%02X:%02X:%02X",p[d->radio?0:5],p[d->radio?1:4],p[d->radio?2:3],p[d->radio?3:2],p[d->radio?4:1],p[d->radio?5:0]);
}
int ls_sweep_command(int argc,char **argv) {
    ls_sweep_init();
    if(argc==2 && (!strcmp(argv[1],"on") || !strcmp(argv[1],"off"))) {ls_sweep_start(!strcmp(argv[1],"on"));return 0;}
    if(argc==3 && !strcmp(argv[1],"mute") && (!strcmp(argv[2],"on") || !strcmp(argv[2],"off"))) {
        ls_sweep_settings_t o;ls_sweep_settings_get(&o);o.alerts_muted=!strcmp(argv[2],"on");
        return ls_sweep_settings_set(&o)?0:1;
    }
    if(argc==3 && (!strcmp(argv[1],"mute") || !strcmp(argv[1],"unmute"))) {
        int c=-1;for(int i=0;i<SW_CATS;i++) if(strlen(argv[2])==strlen(ls_sweep_category(i)) && contains(argv[2],ls_sweep_category(i))) c=i;
        if(c<0) return 1;
        ls_sweep_settings_t o;ls_sweep_settings_get(&o);
        if(!strcmp(argv[1],"mute")) o.muted|=1<<c;else o.muted&=~(1<<c);
        return ls_sweep_settings_set(&o)?0:1;
    }
    if(argc==3 && !strcmp(argv[1],"rules") && !strcmp(argv[2],"reload")) return ls_sweep_rules_reload()?0:1;
    if(argc==3 && !strcmp(argv[1],"hunt")) {
        if(!strcmp(argv[2],"off")) return ls_sweep_hunt(NULL,0,0)?0:1;
        uint8_t mac[6];if(!hexbytes(argv[2],mac,6)) return 1;
        /* Match canonical printed MAC against live table; preserve radio/type. */
        lock();bool found=false;uint8_t radio=0,type=0,raw[6];
        for(unsigned i=0;i<LS_SWEEP_CAP;i++) if(table[i].serial) {char s[18];uint8_t printed[6];mac_text(s,&table[i]);hexbytes(s,printed,6);if(!memcmp(mac,printed,6)) {memcpy(raw,table[i].mac,6);radio=table[i].radio;type=table[i].address_type;found=true;break;}}
        unlock();return found && ls_sweep_hunt(raw,radio,type)?0:1;
    }
    if(argc==1 || (argc==2 && !strcmp(argv[1],"status"))) {ls_sweep_status_t s;ls_sweep_status(&s,esp_timer_get_time());ls_sweep_settings_t o;ls_sweep_settings_get(&o);printf("SWEEP %s / %s / %s / %u rules / flood=%u / %s\n",o.alerts_muted?"MUTED":"SOUND",s.requested?"requested":"off",s.running?"running":"idle",s.rules,s.flood,s.status);return 0;}
    if(argc==2 && !strcmp(argv[1],"stats")) {
        ls_sweep_stats_t s;ls_sweep_transport_stats_t t;
        ls_sweep_stats(&s,esp_timer_get_time());ls_sweep_transport_stats(&t);
        printf("SWEEP RX adverts_in=%lu sweep_in=%lu dispatched=%lu ring_drops=%lu overflow=%lu invalid=%lu ring=%u/%u high_water=%u transport=%s\n",
            (unsigned long)t.adverts_in,(unsigned long)s.adverts_in,(unsigned long)t.dispatched,(unsigned long)t.ring_drops,
            (unsigned long)t.ring_overflow,(unsigned long)t.invalid_reports,t.ring_size,t.ring_capacity,t.ring_high_water,t.available?"BLE":"unavailable");
        printf("SWEEP scanning=%s while_connected=%s\n",t.scanning?"yes":"no",t.while_connected?"yes":"no");
        printf("SWEEP accepted CAMERA=%lu BODYCAM=%lu DRONE=%lu TRACKER=%lu ATTACK=%lu\n",
            (unsigned long)s.accepted[0],(unsigned long)s.accepted[1],(unsigned long)s.accepted[2],(unsigned long)s.accepted[3],(unsigned long)s.accepted[4]);
        printf("SWEEP table=%u/%u evictions_capacity=%lu expiries=%lu category_changes=%lu flood_entries=%lu alerts_suppressed=%lu passing_timeout_s=60 with_you_timeout_s=900\n",
            s.table_size,s.capacity,(unsigned long)s.evicted_capacity,(unsigned long)s.expiries,(unsigned long)s.category_changes,(unsigned long)s.flood_entries,(unsigned long)s.alerts_suppressed);
        printf("SWEEP unique_addresses_min=%u unique_cap=1024 unique_saturated=%lu filter_duplicates=%u duplicates_suppressed=%s scan_interval_units=%u scan_window_units=%u (0=NimBLE default)\n",
            s.unique_addresses_min,(unsigned long)s.unique_saturated,t.filter_duplicates,t.available?"0":"unavailable",t.scan_interval_units,t.scan_window_units);
        return 0;
    }
    if(argc==2 && !strcmp(argv[1],"list")) {
        ls_sweep_device_t *v;
#ifdef ESP_PLATFORM
        v=heap_caps_malloc(sizeof(*v)*LS_SWEEP_CAP,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
#else
        v=malloc(sizeof(*v)*LS_SWEEP_CAP);
#endif
        if(!v) return 1;
        size_t n=ls_sweep_snapshot(v,LS_SWEEP_CAP,esp_timer_get_time());
        for(size_t i=0;i<n;i++) {char s[18];mac_text(s,&v[i]);printf("%s %s %s %s %d dBm\n",s,ls_sweep_category(v[i].match.category),v[i].match.label,v[i].match.vendor,v[i].rssi);}free(v);return 0;
    }
    puts("sweep on|off|status|stats|list|hunt <mac|off>|mute on|off|mute CAMERA|BODYCAM|DRONE|TRACKER|ATTACK|rules reload");return 1;
}
