#include "ls_detector.h"
#include "ls_detector_air.h"
#include "ls_notify.h"
#include "ls_sweep_app.h"
#include "esp_attr.h"
#include "ls_haptic.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>
#ifdef ESP_PLATFORM
#include "esp_vfs_fat.h"
#endif
#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#define make_dir(p) mkdir(p,0775)
#endif
#ifndef LS_DETECTOR_DIR
#define LS_DETECTOR_DIR "/sdcard/detector"
#endif
#ifndef LS_DETECTOR_ROTATE
#define LS_DETECTOR_ROTATE (4u*1024u*1024u)
#endif
/* The worker only queues banners. The TUI owner drains them and calls the
 * quiet API; notification state is never mutated from the wireless task. */
static EXT_RAM_BSS_ATTR ls_notice_t banners[4];
static unsigned banner_head,banner_count;
static unsigned char banner_lock;
static bool acquire(void) {return !__atomic_test_and_set(&banner_lock,__ATOMIC_ACQUIRE);}
static void release(void) {__atomic_clear(&banner_lock,__ATOMIC_RELEASE);}
void ls_detector_banner_poll(void) {
    ls_notice_t n;bool ready=false;
    if(!acquire())return;
    if(banner_count) {n=banners[banner_head];banner_head=(banner_head+1)%4;banner_count--;ready=true;}
    release();if(ready) {
        for(int i=0;i<ls_tui_screen_count();i++) {
            const char *name=ls_tui_screen_name(i);
            if(name && !strcmp(name,"DETECTOR")){n.screen=i;break;}
        }
        ls_notify_post_quiet(&n);
    }
}
uint8_t ls_detector_deliver(const ls_detector_event_t *e,uint8_t sinks) {
    uint8_t accepted=0;
    if((sinks&DET_BANNER) && acquire()) {
        if(banner_count<4) {
            ls_notice_t *n=&banners[(banner_head+banner_count++)%4];memset(n,0,sizeof(*n));
            strcpy(n->title,"DETECTOR");const char *category=e->test?"TEST":e->source==DET_LORA?"LoRa activity":e->source==DET_RID?"REMOTE ID":e->category<SW_CATS?ls_sweep_category(e->category):"ACTIVITY";
            char air[64]="";
            if(e->source==DET_ADSB) {category="AIRCRAFT";ls_detector_air_event_text(e,air,sizeof(air));}
            if(e->source==DET_ADSB && e->coalesced)snprintf(n->body,sizeof(n->body),"%s %.48s +%u grouped",category,air,(unsigned)e->coalesced);
            else if(e->source==DET_ADSB)snprintf(n->body,sizeof(n->body),"%s %.48s / %.60s",category,air,e->reason);
            else if(e->coalesced)snprintf(n->body,sizeof(n->body),"%s +%u grouped",category,(unsigned)e->coalesced);
            else snprintf(n->body,sizeof(n->body),"%s / %s",category,e->test?"sink test":e->reason);
            static const uint8_t colors[]={TUI_YELLOW,TUI_MAGENTA,TUI_CYAN,TUI_GREEN,TUI_RED};
            n->hue=n->accent=e->source==DET_ADSB?(ls_detector_air_alarm(e->squawk,e->emergency)?TUI_RED:TUI_BLUE|TUI_BRIGHT):e->category<5?colors[e->category]:TUI_CYAN;
            n->screen=-1;n->quiet=true;n->steady=true;
            accepted|=DET_BANNER;
        }
        release();
    }
    if((sinks&DET_VIBRATE) && ls_haptic_present() && ls_haptic_play(LS_HAPTIC_ALERT))accepted|=DET_VIBRATE;
    return accepted;
}
static bool directory(const char *p) {struct stat st;return !make_dir(p) || (!stat(p,&st) && S_ISDIR(st.st_mode));}
static void path(char *p,size_t n,unsigned i) {snprintf(p,n,LS_DETECTOR_DIR "/events/detector-%u.jsonl",i);}
static bool rotate(void) {
    char from[192],to[192];path(from,sizeof(from),7);
    if(remove(from) && errno!=ENOENT)return false;
    for(int i=6;i>=0;i--) {path(from,sizeof(from),(unsigned)i);path(to,sizeof(to),(unsigned)i+1);if(rename(from,to) && errno!=ENOENT)return false;}
    return true;
}
static EXT_RAM_BSS_ATTR char line[3072],io_buffer[4096];
static FILE *log_file;
static size_t log_bytes;
static bool batching,batch_failed;
static bool close_log(void) {
    if(log_file) {if(fclose(log_file))batch_failed=true;log_file=NULL;}
    return !batch_failed;
}
void ls_detector_log_begin(void) {batching=true;batch_failed=false;}
bool ls_detector_log_end(void) {batching=false;return close_log();}
/* Worker-only, one complete bounded JSONL record per write. An incomplete
 * final record is left at EOF in the older file and never concatenated. */
bool ls_detector_write(const ls_detector_event_t *e,uint64_t boot) {
#ifdef ESP_PLATFORM
    uint64_t total,available;
    if(!log_file && (esp_vfs_fat_info("/sdcard",&total,&available)!=ESP_OK || available<8u*1024u*1024u))return false;
#endif
    if(!directory(LS_DETECTOR_DIR) || !directory(LS_DETECTOR_DIR "/events"))return false;
    time_t utc=time(NULL);char utc_text[32]="null";
    if(utc>=1704067200)snprintf(utc_text,sizeof(utc_text),"%" PRId64,(int64_t)utc);
    char signal[16]="null";if(e->source && e->source!=DET_ADSB)snprintf(signal,sizeof(signal),"%d",e->rssi);
    char air[200]="";
    if(e->source==DET_ADSB) {
        /* Broadcast identifiers only. Position, range and the receiver fix are never logged. */
        char alt[16]="null";if(e->altitude_ft!=INT32_MIN)snprintf(alt,sizeof(alt),"%d",(int)e->altitude_ft);
        int bit=ls_detector_air_category_bit(e->emitter_tc,e->emitter_ca);
        snprintf(air,sizeof(air),"\"icao\":\"%06X\",\"callsign\":\"%s\",\"squawk\":%u,\"emergency_state\":%u,\"altitude_baro_ft\":%s,\"adsb_category\":%s%s%s,",
            (unsigned)e->icao,e->callsign,(unsigned)e->squawk,(unsigned)e->emergency,alt,bit<0?"null":"\"",bit<0?"":ls_detector_air_category_name((unsigned)bit),bit<0?"":"\"");
    }
    int n=snprintf(line,sizeof(line),
        "{\"version\":1,\"revision\":%u,\"boot\":\"%016" PRIx64 "\",\"sequence\":%u,\"uptime_us\":%" PRId64 ",\"utc\":%s,\"source\":\"%s\",\"rule_ids\":[%s],\"entity\":\"%016" PRIx64 "\",\"category\":\"%s\",\"rssi\":%s,\"validated_message\":%s,\"signal_units\":\"dBm\",\"band\":%u,\"rate_guess_hz\":%u,\"hits_per_min\":%u,%s\"gps_valid\":false,\"test\":%s,\"selected\":%u,\"allowed\":%u,\"accepted\":%u,\"failed\":%u,\"coalesced\":%u,\"dropped\":%u,\"suppressed\":%u}\n",
        (unsigned)e->revision,boot,(unsigned)e->sequence,e->at,utc_text,e->test?"test":ls_detector_source(e->source),e->rule_ids,e->entity,ls_sweep_category(e->category),signal,e->validated?"true":"false",e->band,e->rate_guess,(unsigned)e->hits_per_min,air,e->test?"true":"false",e->selected,e->allowed,e->delivered,e->failed,(unsigned)e->coalesced,(unsigned)e->dropped,(unsigned)e->suppressed);
    if(n<0 || (size_t)n>=sizeof(line) || (unsigned)n>LS_DETECTOR_ROTATE)return false;
    if(batch_failed)return false;
    char name[192];path(name,sizeof(name),0);struct stat st;bool rollover=false;
    if(log_file && log_bytes+(unsigned)n>LS_DETECTOR_ROTATE) {
        if(!close_log() || !rotate())return false;
    }
    if(!log_file) {
        if(!stat(name,&st)) {
            rollover=(uint64_t)st.st_size+(unsigned)n>LS_DETECTOR_ROTATE;
            if(st.st_size && !rollover) {
                FILE *tail=fopen(name,"rb");if(!tail)return false;
                if(fseek(tail,-1,SEEK_END) || fgetc(tail)!='\n')rollover=true;
                fclose(tail);
            }
        } else if(errno!=ENOENT)return false;
        if(rollover && !rotate())return false;
        log_file=fopen(name,"ab");if(!log_file)return false;
        setvbuf(log_file,io_buffer,_IOFBF,sizeof(io_buffer));
        log_bytes=!stat(name,&st)?(size_t)st.st_size:0;
    }
    bool ok=fwrite(line,1,(size_t)n,log_file)==(size_t)n;
    log_bytes+=(size_t)n;
    if(!ok)batch_failed=true;
    if(!batching)ok=close_log() && ok;
    return ok;
}
