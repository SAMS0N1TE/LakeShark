/* RF-only Drone Watch. Evidence is not protocol identity. The LR sweeps are
 * sequential, so wide occupancy is deliberately only POSSIBLE. No RX payload
 * calls, TX calls, operator fields, or video demodulation belong here.
 * Public ELRS parameters: ExpressLRS/ExpressLRS tag 3.5.3,
 * src/lib/FHSS/FHSS.cpp and src/src/common.cpp. ISM2G4 80 centres:
 * 2400.4..2479.4 MHz. LR2021 supports SF5..8/BW812500, but its ordinary
 * coding/interleaving is not an SX1280 ELRS decoder. Activity only. */
#include "ls_drone_watch.h"
#include "ls_dronelink.h"
#include "ls_seen.h"
#include "ls_rid.h"
#include "ls_wireless.h"
#include "ls_df.h"
#include "ls_mesh.h"
#include "board/ls_lora.h"
#include "radio/radio_endpoint.h"
#include "core/settings.h"
#include "core/settings_schema.h"
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
#include "ls_textpack.h"

typedef struct {
    unsigned version, gate_db, min_dbm, ttl_s, alert_s, approach_db10s,
        slice_ms, hopper_channels, hopper_hits, cadence_percent, wide_mhz,
        wide_hits, wifi_tolerance_us, video_percent;
    unsigned fpv_mhz[40];
} config_t;
static EXT_RAM_BSS_ATTR config_t cfg;
typedef struct {
    ls_drone_watch_entry_t pub;
    int64_t notice_us, rate_start;
    unsigned rate_hits;
    bool announced, approach_announced;
} track_t;
static EXT_RAM_BSS_ATTR track_t tracks[LS_DW_ENTRIES];
static EXT_RAM_BSS_ATTR ls_drone_watch_status_t view;
static StaticSemaphore_t mutex_mem;
static SemaphoreHandle_t mutex;
static unsigned once;
static ls_drone_watch_notice_fn notice_fn;
static void *notice_ctx;
static bool legacy;
static void init(void) {
    unsigned e=0;
    if(__atomic_compare_exchange_n(&once,&e,1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) {
        mutex=xSemaphoreCreateMutexStatic(&mutex_mem);
        __atomic_store_n(&once,2,__ATOMIC_RELEASE);
    } else while(__atomic_load_n(&once,__ATOMIC_ACQUIRE)!=2)vTaskDelay(1);
}
static void lock(void) {init();xSemaphoreTake(mutex,portMAX_DELAY);}
/* Scan cursor: written by the RF worker, read by the UI without the lock.
   Plain words; a torn read only shows one stale position for a frame. */
static volatile uint32_t cur_lo[LS_DW_SOURCES], cur_hi[LS_DW_SOURCES];
static volatile int64_t cur_us[LS_DW_SOURCES];
static volatile unsigned live_count;
static void cursor(ls_dw_source_t s, uint64_t lo_hz, uint64_t hi_hz, int64_t us)
{
    if ((unsigned)s >= LS_DW_SOURCES) return;
    cur_lo[s] = (uint32_t)(lo_hz / 1000000u); cur_hi[s] = (uint32_t)(hi_hz / 1000000u); cur_us[s] = us;
}
void ls_drone_watch_cursor(ls_dw_cursor_t out[LS_DW_SOURCES])
{
    for (unsigned i = 0; i < LS_DW_SOURCES; i++)
        out[i] = (ls_dw_cursor_t){ cur_lo[i], cur_hi[i], cur_us[i] };
}
unsigned ls_drone_watch_count(void) { return live_count; }
static void unlock(void) {xSemaphoreGive(mutex);}
static bool trylock(void) {return __atomic_load_n(&once,__ATOMIC_ACQUIRE)==2 && xSemaphoreTake(mutex,0);}
static bool valid_level(float v) {return isfinite(v) && v>=-160 && v<=0;}
const char *ls_drone_watch_source(ls_dw_source_t s) {
    static const char *const names[]={"elrs900","elrs24","hopper900","wide24","analog58","rid"};
    return (unsigned)s<LS_DW_SOURCES?names[s]:"unknown";
}
const char *ls_drone_watch_label(ls_dw_source_t s) {
    static const char *const names[]={"ELRS 868/915 LoRa activity","ELRS 2.4 LoRa activity",
        "900 MHz hopper","2.4 GHz wideband link (possible)","5.8 GHz analog FPV (likely)","BLE Remote ID"};
    return (unsigned)s<LS_DW_SOURCES?names[s]:"unknown";
}
static uint32_t prefs(void) {return settings_get_drone_watch();}
static bool enabled(ls_dw_source_t s) {uint32_t p=prefs();return (p&1) && (p&(4u<<s));}
bool ls_drone_watch_enable(bool on) {return settings_set_drone_watch((prefs()&~1u)|(on?1:0));}
bool ls_drone_watch_alerts(bool on) {return settings_set_drone_watch((prefs()&~2u)|(on?2:0));}
bool ls_drone_watch_source_enable(ls_dw_source_t s,bool on) {
    if((unsigned)s>=LS_DW_SOURCES)return false;
    return settings_set_drone_watch((prefs()&~(4u<<s))|(on?(4u<<s):0));
}
void ls_drone_watch_legacy(bool on) {__atomic_store_n(&legacy,on,__ATOMIC_RELEASE);}
void ls_drone_watch_notice_hook(ls_drone_watch_notice_fn fn,void *ctx) {lock();notice_fn=fn;notice_ctx=ctx;unlock();}

/* Numeric, closed schema: every key required once, no coercion, unknown keys,
 * trailing input, duplicate keys, partial arrays, or integer overflow. */
static void space(const char **p,const char *end) {while(*p<end && isspace((unsigned char)**p))++*p;}
static bool ch(const char **p,const char *end,char c) {space(p,end);if(*p==end || **p!=c)return false;++*p;return true;}
static bool integer(const char **p,const char *end,unsigned *v) {
    space(p,end);if(*p==end || !isdigit((unsigned char)**p))return false;
    bool zero=**p=='0';unsigned n=0,x=0;
    while(*p<end && isdigit((unsigned char)**p)) {
        if(++n>9 || (zero && n>1))return false;
        x=x*10+(unsigned)(*(*p)++-'0');
    } *v=x;return true;
}
bool ls_drone_watch_configure(const char *json,size_t len) {
    if(!json || len>4096)return false;
    const char *p=json,*end=json+len;
    config_t c={0};unsigned seen=0;
    static const char *const keys[]={"version","gate_db","min_dbm","ttl_s","alert_s",
        "approach_db10s","slice_ms","hopper_channels","hopper_hits","cadence_percent",
        "wide_mhz","wide_hits","wifi_tolerance_us","video_percent","fpv_mhz"};
    unsigned *values[]={&c.version,&c.gate_db,&c.min_dbm,&c.ttl_s,&c.alert_s,&c.approach_db10s,
        &c.slice_ms,&c.hopper_channels,&c.hopper_hits,&c.cadence_percent,&c.wide_mhz,
        &c.wide_hits,&c.wifi_tolerance_us,&c.video_percent};
    if(!ch(&p,end,'{'))return false;
    do {
        char key[32];unsigned n=0,k=15;
        if(!ch(&p,end,'"'))return false;
        while(p<end && *p!='"') {if(n==sizeof(key)-1)return false;key[n++]=*p++;}key[n]=0;
        if(!ch(&p,end,'"') || !ch(&p,end,':'))return false;
        for(unsigned i=0;i<15;i++)if(!strcmp(key,keys[i]))k=i;
        if(k==15 || (seen&(1u<<k)))return false;
        seen|=1u<<k;
        if(k==14) {
            if(!ch(&p,end,'['))return false;
            for(unsigned i=0;i<40;i++) {
                if(i && !ch(&p,end,','))return false;
                if(!integer(&p,end,&c.fpv_mhz[i]) || c.fpv_mhz[i]<5645 || c.fpv_mhz[i]>5945)return false;
            }
            if(!ch(&p,end,']'))return false;
        } else if(!integer(&p,end,values[k]))return false;
    }while(ch(&p,end,','));
    if(!ch(&p,end,'}'))return false;
    space(&p,end);
    if(p!=end || seen!=32767 || c.version!=1 || c.gate_db<6 || c.gate_db>40 ||
       c.min_dbm<30 || c.min_dbm>140 || c.ttl_s<10 || c.ttl_s>600 ||
       c.alert_s<5 || c.alert_s>3600 || c.approach_db10s<1 || c.approach_db10s>20 ||
       c.slice_ms<500 || c.slice_ms>10000 || c.hopper_channels<4 || c.hopper_channels>16 ||
       c.hopper_hits<c.hopper_channels || c.hopper_hits>32 || c.cadence_percent<5 || c.cadence_percent>40 ||
       c.wide_mhz<10 || c.wide_mhz>40 || c.wide_hits<3 || c.wide_hits>16 ||
       c.wifi_tolerance_us<1000 || c.wifi_tolerance_us>20000 || c.video_percent<10 || c.video_percent>80)return false;
    lock();cfg=c;LS_SNPUTS(t_ls_drone_watch_v1_configuration_loaded, view.config, sizeof(view.config), "v1 configuration loaded");
    for(unsigned i=0;i<LS_DW_SOURCES;i++)if(!view.reason[i][0])LS_SNPUTS(t_ls_drone_watch_waiting_for_source_slice, view.reason[i], sizeof(view.reason[i]), "waiting for source slice");
    unlock();return true;
}
size_t ls_drone_watch_snapshot(ls_drone_watch_entry_t *out,size_t cap) {
    if(!out && cap)return SIZE_MAX;
    if(!trylock())return SIZE_MAX;
    size_t n=0;for(unsigned i=0;i<LS_DW_ENTRIES && n<cap;i++)if(tracks[i].pub.id)out[n++]=tracks[i].pub;
    unlock();return n;
}
bool ls_drone_watch_read(uint64_t id,ls_drone_watch_entry_t *out) {
    if(!out || !trylock())return false;
    bool found=false;for(unsigned i=0;i<LS_DW_ENTRIES;i++)if(id && tracks[i].pub.id==id) {*out=tracks[i].pub;found=true;break;}
    unlock();return found;
}
bool ls_drone_watch_status(ls_drone_watch_status_t *out) {
    if(!out || !trylock())return false;
    *out=view;uint32_t p=prefs();out->enabled=p&1;out->alerts=p&2;out->sources=(p>>2)&63;
    unlock();return true;
}
static void state(ls_dw_source_t s,bool running,const char *why) {
    lock();view.running[s]=running;snprintf(view.reason[s],sizeof(view.reason[s]),"%s",why);unlock();
}
/* Hop evidence for ELRS: hits on many channels in a short window. A single
   LoRa node (a mesh neighbour) or one burst of Wi-Fi sits on one channel and
   never qualifies. Worker only, like every producer. */
#define HOP_RING 24
typedef struct { int64_t us[HOP_RING]; uint32_t ch[HOP_RING]; float dbm[HOP_RING]; unsigned w; } hops_t;
static EXT_RAM_BSS_ATTR hops_t hops[2];
static unsigned hop_distinct;   /* set just before observe() for an ELRS source */
static bool hop_evidence(hops_t *h,uint32_t ch,float dbm,int64_t now,float *best) {
    h->us[h->w%HOP_RING]=now;h->ch[h->w%HOP_RING]=ch;h->dbm[h->w%HOP_RING]=dbm;h->w++;
    uint32_t seen[HOP_RING];unsigned n=0;*best=-200;
    for(unsigned i=0;i<HOP_RING && i<h->w;i++) {
        if(now-h->us[i]>3000000)continue;
        if(h->dbm[i]>*best)*best=h->dbm[i];
        bool dup=false;for(unsigned k=0;k<n;k++)dup|=seen[k]==h->ch[i];
        if(!dup)seen[n++]=h->ch[i];
    }
    hop_distinct=n;
    return n>=3 && *best>=-90.0f;
}
static void observe(ls_dw_source_t source,uint64_t key,uint64_t hz,float level,
                    bool calibrated,const char *channel,const char *identity,int64_t now) {
    if(!cfg.version || (unsigned)source>=LS_DW_SOURCES || !valid_level(level) || now<0 || !enabled(source))return;
    /* Source namespace plus canonical frequency or RID's stable serial. */
    uint64_t id=((uint64_t)(source+1)<<56)|(key&UINT64_C(0x00ffffffffffffff));
    lock();track_t *t=NULL,*oldest=&tracks[0];
    for(unsigned i=0;i<LS_DW_ENTRIES;i++) {
        if(tracks[i].pub.id==id) {t=&tracks[i];break;}
        if(!tracks[i].pub.id && !t)t=&tracks[i];
        if(tracks[i].pub.last_us<oldest->pub.last_us)oldest=&tracks[i];
    }
    if(!t) {t=oldest;view.dropped++;}
    bool fresh=t->pub.id!=id;
    if(fresh) {memset(t,0,sizeof(*t));t->pub.id=id;t->pub.first_us=now;t->pub.peak_dbm=level;t->rate_start=now;}
    ls_drone_watch_entry_t *e=&t->pub;
    if(!fresh && now<=e->last_us) {unlock();return;}
    e->source=source;e->centre_hz=hz;e->band_mhz=source==LS_DW_RID?2400:hz<1000000000?900:hz<3000000000?2400:5800;
    if(source==LS_DW_ELRS900)e->band_mhz=hz<880000000?868:915;
    e->rssi_dbm=level;if(level>e->peak_dbm)e->peak_dbm=level;
    e->calibrated=calibrated;e->last_us=now;
    e->confidence=source==LS_DW_RID?LS_DW_CONFIRMED:
        (source==LS_DW_ELRS900 || source==LS_DW_ELRS24)?(hop_distinct>=6?LS_DW_LIKELY:LS_DW_POSSIBLE):
        source==LS_DW_ANALOG58?LS_DW_LIKELY:LS_DW_POSSIBLE;
    if(channel)snprintf(e->channel,sizeof(e->channel),"%s",channel);
    if(identity)snprintf(e->identity,sizeof(e->identity),"%.40s",identity);
    /* At most one point per second; use the latest measured level within it. */
    unsigned n=e->history_count;
    if(n && now/1000000==e->history[n-1].us/1000000)e->history[n-1]=(ls_dw_rssi_t){now,level};
    else {
        if(n==LS_DW_HISTORY) {memmove(e->history,e->history+1,(n-1)*sizeof(e->history[0]));n--;}
        e->history[n++]=(ls_dw_rssi_t){now,level};e->history_count=n;
    }
    /* Least-squares slope over the most recent 10 seconds, after >=3 s.
     * EWMA smooths the slope, not a timer animation or an inferred distance. */
    double sx=0,sy=0,sxx=0,sxy=0;unsigned points=0;
    for(unsigned i=0;i<n;i++)if(now-e->history[i].us<=10000000) {
        double x=(e->history[i].us-now)/1e6,y=e->history[i].dbm;
        sx+=x;sy+=y;sxx+=x*x;sxy+=x*y;points++;
    }
    if(points>=4) {
        double den=points*sxx-sx*sx;
        if(den>1) {float slope=(float)(10*(points*sxy-sx*sy)/den);e->db_per_10s=.65f*e->db_per_10s+.35f*slope;}
    }
    e->trend=e->db_per_10s>=cfg.approach_db10s?LS_DW_APPROACHING:
             e->db_per_10s<=-(float)cfg.approach_db10s?LS_DW_RECEDING:LS_DW_STEADY;
    int64_t dt=now-t->rate_start;
    t->rate_hits++;
    if(dt>=1000000) {e->hits_per_second=t->rate_hits*1e6f/(float)dt;t->rate_hits=0;t->rate_start=now;}
    bool approaching=e->trend==LS_DW_APPROACHING;
    if(!approaching)t->approach_announced=false;
    bool alert=(prefs()&2) && (!t->announced || (approaching && !t->approach_announced)) &&
        (!t->announced || now-t->notice_us>=(int64_t)cfg.alert_s*1000000);
    ls_drone_watch_notice_fn fn=notice_fn;void *ctx=notice_ctx;
    /* Queue-only call. No sinks, disk, sound or slow work under this lock. */
    if(alert && ls_seen_drone_watch(id,(int)level,calibrated,e->band_mhz,approaching,now)) {
        t->notice_us=now;t->announced=true;t->approach_announced=approaching;
    } else {if(alert)view.dropped++;alert=false;}
    unlock();if(alert && fn)fn(id,approaching,ctx);
}
void ls_drone_watch_lora900(unsigned band,unsigned channel,float dbm,int64_t us) {
    uint64_t hz=band==915?903500000u+channel*600000u:865275000u+(uint64_t)channel*4300000u/12;
    float best;
    if(!hop_evidence(&hops[0],(uint32_t)(hz/100000u),dbm,us,&best))return;
    /* One link per band, whatever channel this hop landed on. */
    observe(LS_DW_ELRS900,band==915?915:868,hz,best,true,NULL,NULL,us);
}
void ls_drone_watch_activity(ls_dw_source_t s,uint64_t hz,float dbm,int64_t us) {
    if(s!=LS_DW_ELRS24 || hz<2400400000u || hz>2479400000u)return;
    float best;
    if(!hop_evidence(&hops[1],(uint32_t)(hz/100000u),dbm,us,&best))return;
    observe(s,2400,hz,best,true,NULL,NULL,us);
}

typedef struct {
    float floor[96];
    unsigned learned, hits, used, channels[16];
    int64_t last, period;
    unsigned beacon_hits;
    bool high;
} evidence_t;
static EXT_RAM_BSS_ATTR evidence_t evidence[3];
void ls_drone_watch_spectrum(ls_dw_source_t source,const float *dbm,size_t n,uint32_t lo,uint32_t hi,int64_t us) {
    if(!cfg.version || (unsigned)source>=LS_DW_SOURCES || !enabled(source) || !dbm || n<16 || n>96 || hi<=lo || us<0)return;
    if(source!=LS_DW_HOPPER900 && source!=LS_DW_WIDE24 && source!=LS_DW_ELRS24)return;
    if((source==LS_DW_HOPPER900 && (lo<860000000u || hi>930000000u)) ||
       (source!=LS_DW_HOPPER900 && (lo<2400000000u || hi>2483500000u)))return;
    cursor(source,lo,hi,us);
    evidence_t *v=&evidence[source==LS_DW_HOPPER900?0:source==LS_DW_WIDE24?1:2];
    for(size_t i=0;i<n;i++)if(!valid_level(dbm[i]))return;
    if(!v->learned) {for(size_t i=0;i<n;i++)v->floor[i]=dbm[i];v->learned=1;return;}
    unsigned hot=0,run=0,width=0,peak=0;
    for(size_t i=0;i<n;i++) {
        bool high=dbm[i]>=-(float)cfg.min_dbm && dbm[i]-v->floor[i]>=cfg.gate_db;
        if(high) {hot++;if(++run>width)width=run;if(dbm[i]>dbm[peak])peak=(unsigned)i;}
        else {run=0;v->floor[i]+=(dbm[i]-v->floor[i])*.08f;}
    }
    uint32_t hz=lo+(uint32_t)((uint64_t)(hi-lo)*peak/(n-1));
    bool wide=width>1 && (uint64_t)(width-1)*(hi-lo)>=(uint64_t)cfg.wide_mhz*1000000*(n-1);
    bool high=source==LS_DW_WIDE24?wide:(hot>0 && hot<=3);
    if(!high) {v->high=false;return;}
    /* Repeated high rows of a steady node/occupied channel are not bursts.
     * Retuning through a wide signal cannot satisfy the narrow-hop test. */
    if(v->high)return;
    v->high=true;
    int64_t delta=us-v->last;
    if(v->last && delta<=0)return;
    if(v->last && delta>2000000) {v->hits=v->used=v->beacon_hits=0;v->period=0;}
    if(v->last && delta>0 && delta<=2000000) {
        /* Beacon cadence, including missed beacons (up to 16 intervals). */
        int64_t multiple=(delta+51200)/102400;
        if(multiple>=1 && multiple<=16 && llabs(delta-multiple*102400)<cfg.wifi_tolerance_us)v->beacon_hits++;
        else v->beacon_hits=0;
        if(v->period && llabs(delta-v->period)*100>v->period*cfg.cadence_percent && source==LS_DW_HOPPER900) {
            v->hits=v->used=0;
        }
        v->period=v->period?(v->period*3+delta)/4:delta;
    }
    v->last=us;v->hits++;
    bool known=false;for(unsigned i=0;i<v->used;i++)if(v->channels[i]==hz)known=true;
    if(!known && v->used<16)v->channels[v->used++]=hz;
    if(source==LS_DW_HOPPER900) {
        if(v->hits>=cfg.hopper_hits && v->used>=cfg.hopper_channels)
            observe(source,900, hz,dbm[peak],true,NULL,NULL,us);
    } else if(source==LS_DW_ELRS24) {
        /* Unsupported packet parameters: RSSI bursts are not ELRS identity. */
        if(v->hits>=cfg.hopper_hits && v->used>=cfg.hopper_channels) {
            observe(source,2400,hz,dbm[peak],true,"RSSI fallback",NULL,us);
            lock();for(unsigned i=0;i<LS_DW_ENTRIES;i++)if(tracks[i].pub.source==source)tracks[i].pub.confidence=LS_DW_POSSIBLE;unlock();
        }
    } else {
        /* Conservative exclusion of persistent channel 1/6/11 occupancy as
         * well as periodic beacons. Rejecting some real links is preferable
         * to warning about every nearby AP. RSSI cannot identify DJI. */
        bool wifi=false;
        const uint32_t centres[]={2412000000u,2437000000u,2462000000u};
        for(unsigned c=0;c<3;c++)if(hz>centres[c]-10000000u && hz<centres[c]+10000000u)wifi=true;
        if(v->hits>=cfg.wide_hits && v->beacon_hits<2 && !wifi)
            observe(source,2400,hz,dbm[peak],true,"wide (possible)",NULL,us);
    }
}
bool ls_drone_watch_video(const float *p,size_t n,uint32_t rate) {
    if(!p || n<256 || n>8192 || rate<100000 || rate>2000000 || (uint64_t)n*1000<2*rate)return false;
    float mean=0,var=0;
    for(size_t i=0;i<n;i++) {if(!isfinite(p[i]) || p[i]<0)return false;mean+=p[i];}mean/=n;
    for(size_t i=0;i<n;i++) {float d=p[i]-mean;var+=d*d;}
    if(mean<=0 || var/n<mean*mean*.0001)return false; /* CW is not video */
    /* Normalized line-rate spectral prominence, bounded direct DFT at two
     * public PAL/NTSC line rates plus flanking controls. No pixel recovery. */
    const float f[]={15625,15734,13000,18000};float power[4];
    for(unsigned k=0;k<4;k++) {
        float re=0,im=0,w=6.283185307f*f[k]/rate;
        float cr=cosf(w),ci=sinf(w),xr=1,xi=0;
        for(size_t i=0;i<n;i++) {float d=p[i]-mean;re+=d*xr;im+=d*xi;float nr=xr*cr-xi*ci;xi=xr*ci+xi*cr;xr=nr;}
        power[k]=(re*re+im*im)/(var*n);
    }
    float peak=fmaxf(power[0],power[1]);
    return peak*100>cfg.video_percent && peak>4*fmaxf(power[2],power[3]);
}
void ls_drone_watch_analog(unsigned channel,const float *p,size_t n,uint32_t rate,float level,bool calibrated,int64_t us) {
    if(channel>=40 || !cfg.version || !ls_drone_watch_video(p,n,rate))return;
    char name[16];snprintf(name,sizeof(name),"%c%u %u","ABEFR"[channel/8],channel%8+1,cfg.fpv_mhz[channel]);
    uint64_t hz=(uint64_t)cfg.fpv_mhz[channel]*1000000;
    observe(LS_DW_ANALOG58,hz,hz,level,calibrated,name,NULL,us);
}

/* All state below belongs exclusively to the RF worker. */
static unsigned turn=LS_DW_SOURCES, elrs_channel, elrs_sf, fpv_channel;
static int64_t turn_at, cycle_at, rid_at, elrs_hop_at, legacy_poll_at;
static bool held, scan, packet, fallback;
static ls_lora_cfg_t saved;
static bool saved_rx;
static ls_radio_session_t *sdr;
static unsigned flush;
static size_t iq_bytes;
static EXT_RAM_BSS_ATTR uint8_t iq[8192*2];
static EXT_RAM_BSS_ATTR float envelope[8192], row[84];
static EXT_RAM_BSS_ATTR ls_rid_drone_t rid[LS_RID_MAX];
static volatile bool on_screen;
static bool waiting;   /* hold_bg refused once; stays registered so the mesh can hand over */
void ls_drone_watch_foreground(bool on) {on_screen=on;ls_dronelink_may_pause_mesh(on);}
static void release(void) {
    if(scan) {ls_lora_scan_end();scan=false;}
    if(packet) {ls_lora_configure(&saved);if(saved_rx)ls_lora_receive();packet=false;}
    if(held || waiting) {ls_mesh_radio_hold_bg(false);held=false;waiting=false;}
    if(sdr) {ls_radio_iq_stop(sdr);ls_radio_release(sdr);sdr=NULL;}
    iq_bytes=0;
    ls_dronelink_suspend();
}
static bool claim(void) {
    /* A foreground owner always wins; another background holder (FM, P25 on
       the LR2021) is left alone; MeshCore is paused only while DRONES is open. */
    if(ls_mesh_radio_wanted() || (!held && !waiting && ls_mesh_radio_bg()))return false;
    if(!on_screen && ls_mesh_running())return false;
    if(held)return true;
    held=ls_mesh_radio_hold_bg(true);
    /* Refused: the request stays registered and the mesh task pauses and
       hands over; withdrawing here would restart that handshake forever. */
    waiting=!held;
    return held;
}
static bool tune_elrs(void) {
    ls_lora_cfg_t c={.freq_hz=2400400000u+elrs_channel*1000000u,.sf=5+elrs_sf,
        .bw_hz=812500,.cr=6,.preamble=12,.sync_word=0x12,.implicit_rx_bytes=8,
        .cal_min_mhz=2400,.cal_max_mhz=2480};
    if(ls_lora_configure(&c)!=ESP_OK)return false;
    cursor(LS_DW_ELRS24,c.freq_hz-406250u,c.freq_hz+406250u,esp_timer_get_time());
    const ls_lora_cfg_t *actual=ls_lora_cfg();
    return actual && actual->bw_hz==812500 && actual->sf==c.sf && ls_lora_receive()==ESP_OK;
}
static void rid_pass(int64_t now) {
#ifdef ESP_PLATFORM
    ls_wireless_drone_rid(enabled(LS_DW_RID));
#endif
    if(!enabled(LS_DW_RID)) {state(LS_DW_RID,false,"disabled");return;}
    /* Reuse the passive BLE receiver. Never enable connections or TX.
     * Empty cache does not prove the co-processor is scanning. */
    size_t n=ls_rid_snapshot(rid,LS_RID_MAX,now);
#ifdef ESP_PLATFORM
    ls_rid_rx_t status=ls_wireless_rid_status();
    static const char *const reasons[]={"BLE receiver off","BLE hardware unavailable","BLE receiver starting",
        "BLE receiver failed","BLE receiver paused","passive BLE RID listening"};
    state(LS_DW_RID,status==LS_RID_RX_LISTENING,reasons[status]);
#else
    state(LS_DW_RID,n!=0,n?"passive RID cache; aircraft identity only":"no fresh RID; requires passive BLE scan");
#endif
    for(size_t i=0;i<n;i++)observe(LS_DW_RID,rid[i].serial,0,rid[i].rssi,true,NULL,rid[i].uas_id,rid[i].last_us);
}
static void sdr_step(int64_t now) {
    uint64_t hz=(uint64_t)cfg.fpv_mhz[fpv_channel]*1000000;
    if(!sdr) {
        ls_radio_requirements_t req={.required_caps=LS_RADIO_RX_IQ_U8,.min_hz=hz-200000,.max_hz=hz,
            .sample_rate_hz=2000000,.iq_format=LS_RADIO_IQ_FORMAT_U8_INTERLEAVED,
            .preferred_endpoint_id=LS_RADIO_ENDPOINT_HACKRF_USB};
        ls_radio_err_t err=ls_radio_acquire("drone-watch",&req,&sdr);
        if(err!=LS_RADIO_OK) {sdr=NULL;state(LS_DW_ANALOG58,false,ls_radio_err_name(err));return;}
        ls_radio_iq_config_t c={.center_hz=hz-200000,.sample_rate_hz=2000000,.bandwidth_hz=2000000,
            .gain_mode=LS_RADIO_GAIN_MANUAL,.gain_tenths_db=160},actual;
        if(ls_radio_iq_configure(sdr,&c,&actual)!=LS_RADIO_OK || actual.center_hz!=hz-200000 ||
           actual.sample_rate_hz!=2000000 || ls_radio_iq_start(sdr)!=LS_RADIO_OK) {
            release();state(LS_DW_ANALOG58,false,LS_TEXT(t_ls_drone_watch_hackrf_configure_start_failed, "HackRF configure/start failed"));return;
        }
        flush=2;iq_bytes=0;
    }
    size_t got=0;
    ls_radio_err_t err=ls_radio_iq_read(sdr,iq+iq_bytes,sizeof(iq)-iq_bytes,5,&got);
    if(got>sizeof(iq)-iq_bytes || (got&1) || (err!=LS_RADIO_OK && err!=LS_RADIO_ERR_TIMEOUT)) {
        release();state(LS_DW_ANALOG58,false,LS_TEXT(t_ls_drone_watch_hackrf_disconnected_read_failed, "HackRF disconnected/read failed"));return;
    }
    iq_bytes+=got;if(iq_bytes<sizeof(iq))return;iq_bytes=0;
    if(flush) {flush--;return;}
    cursor(LS_DW_ANALOG58,hz-4000000u,hz+4000000u,now);
    float mean=0;unsigned rails=0;
    for(unsigned i=0;i<8192;i++) {
        float a=iq[2*i]-127.5f,b=iq[2*i+1]-127.5f;
        envelope[i]=a*a+b*b;mean+=envelope[i];
        rails+=iq[2*i]==0 || iq[2*i]==255 || iq[2*i+1]==0 || iq[2*i+1]==255;
    }
    mean/=8192;
    /* Strong centre carrier plus line structure. Full-scale power and a
     * centre carrier are measured, never calibrated to pretend RF dBm. */
    float level=mean>0?10*log10f((float)mean/(2*127.5f*127.5f)):-160;
    /* Shared FIND spectral helper removes DC and measures the carrier
     * 200 kHz off the tuner centre. A DC spike must never qualify as RF. */
    float carrier=ls_df_iq_band_db(iq,2048,2000000,200000,25000);
    if(rails<82 && level>-45 && carrier>-40)
        ls_drone_watch_analog(fpv_channel,envelope,8192,2000000,level,false,now);
    state(LS_DW_ANALOG58,true,LS_TEXT(t_ls_drone_watch_hackrf_rx_carrier_line, "HackRF RX: carrier/line-rate evidence; relative dB"));
    fpv_channel=(fpv_channel+1)%40;
    uint64_t actual=0;hz=(uint64_t)cfg.fpv_mhz[fpv_channel]*1000000;
    if(ls_radio_iq_retune(sdr,hz-200000,true,&actual)!=LS_RADIO_OK || actual!=hz-200000) {
        release();state(LS_DW_ANALOG58,false,LS_TEXT(t_ls_drone_watch_hackrf_retune_failed, "HackRF retune failed"));return;
    }
    flush=2;
}
void ls_drone_watch_step(int64_t now) {
    if(!cfg.version || now<0)return;
    for(unsigned i=0;i<5;i++)if(!enabled(i) && !(i==LS_DW_ELRS900 && __atomic_load_n(&legacy,__ATOMIC_ACQUIRE)))state(i,false,"disabled");
    if(now-rid_at>=250000) {rid_at=now;rid_pass(now);}
    lock();for(unsigned i=0;i<LS_DW_ENTRIES;i++)if(tracks[i].pub.id && now-tracks[i].pub.last_us>(int64_t)cfg.ttl_s*1000000)memset(&tracks[i],0,sizeof(tracks[i]));
    {unsigned live=0;for(unsigned i=0;i<LS_DW_ENTRIES;i++)live+=tracks[i].pub.id!=0;live_count=live;}unlock();
    bool old=__atomic_load_n(&legacy,__ATOMIC_ACQUIRE);
    if(!(prefs()&1) && !old) {release();turn=LS_DW_SOURCES;for(unsigned i=0;i<LS_DW_SOURCES;i++)state(i,false,"watch off");return;}
    if(!(prefs()&0x7cu) && !old) {release();turn=LS_DW_SOURCES;return;}
    if(turn==LS_DW_SOURCES || now-turn_at>=(int64_t)cfg.slice_ms*1000 ||
       (turn<LS_DW_SOURCES && !enabled(turn) && !(turn==LS_DW_ELRS900 && old))) {
        if(turn<LS_DW_SOURCES)state(turn,false,LS_TEXT(t_ls_drone_watch_waiting_for_round_robin, "waiting for round-robin slice"));
        release();
        unsigned previous=turn;
        unsigned next=turn==LS_DW_SOURCES?0:(turn+1)%5;
        for(unsigned i=0;i<5;i++,next=(next+1)%5)if(enabled(next) || (next==LS_DW_ELRS900 && old))break;
        turn=next;turn_at=now;
        if(previous==LS_DW_SOURCES || turn<=previous) {lock();view.cycle_ms=cycle_at?(uint32_t)((now-cycle_at)/1000):0;unlock();cycle_at=now;}
    }
    if(!enabled(turn) && !(turn==LS_DW_ELRS900 && old)) {state(turn,false,"disabled");return;}
    if(turn==LS_DW_ANALOG58) {sdr_step(now);return;}
    if(ls_mesh_radio_wanted() || (!on_screen && ls_mesh_running()) ||
       (!held && !waiting && ls_mesh_radio_bg() && turn!=LS_DW_ELRS900)) {
        release();state(turn,false,on_screen?"another app holds the radio":"waiting for MeshCore (open DRONES to scan now)");return;
    }
    if(turn==LS_DW_ELRS900) {
        if(now>=legacy_poll_at) {ls_dronelink_tick(true,now);legacy_poll_at=now+125000;}
        state(turn,ls_dronelink_pending(),ls_dronelink_status());return;
    }
    uint32_t caps=ls_lora_caps();
    bool hf=turn==LS_DW_ELRS24 || turn==LS_DW_WIDE24;
    if(!ls_lora_present() || !(caps&LS_LORA_CAP_RSSI_INST) ||
       (hf && (!(caps&LS_LORA_CAP_BAND_1G5_2G5) || !ls_lora_rx_range_ok(caps,2400000000u,2483500000u)))) {
        state(turn,false,hf?"LR2021 HF RSSI input unavailable":"RSSI_INST unavailable");return;
    }
    if(!claim()) {state(turn,false,"radio lease busy");return;}
    if(turn==LS_DW_ELRS24 && !scan) {
        if(!packet && !fallback) {
            const ls_lora_cfg_t *c=ls_lora_cfg();if(c)saved=*c;else ls_lora_cfg_default(&saved);
            saved_rx=ls_lora_is_receiving();packet=true;fallback=!tune_elrs();
            if(fallback) {ls_lora_configure(&saved);if(saved_rx)ls_lora_receive();packet=false;}
            else {elrs_hop_at=now+250000;state(turn,true,LS_TEXT(t_ls_drone_watch_sf5_8_bw812_5k, "SF5..8/BW812.5k preamble only; coding differs from SX1280"));return;}
        }
        if(packet) {
            float level;int flags=ls_lora_activity(&level);
            if(flags<0) {release();state(turn,false,LS_TEXT(t_ls_drone_watch_hf_activity_read_failed, "HF activity read failed"));return;}
            if(flags&3)ls_drone_watch_activity(LS_DW_ELRS24,2400400000u+elrs_channel*1000000u,level,now);
            if(now>=elrs_hop_at) {
                elrs_channel=(elrs_channel+1)%80;if(!elrs_channel)elrs_sf=(elrs_sf+1)%4;
                elrs_hop_at=now+250000;
                if(!tune_elrs()) {release();state(turn,false,LS_TEXT(t_ls_drone_watch_hf_parameters_unsupported_retry, "HF parameters unsupported; retry RSSI next slice"));fallback=true;}
            }
            return;
        }
    }
    if(!scan) {
        uint32_t lo=hf?2400000000u:860000000u,hi=hf?2483500000u:930000000u;
        if(ls_lora_scan_begin(lo,hi)!=ESP_OK) {release();state(turn,false,LS_TEXT(t_ls_drone_watch_rssi_sweep_unavailable, "RSSI sweep unavailable"));return;}
        scan=true;
    }
    bool done=false;unsigned n=hf?84:71;
    if(!ls_lora_scan_pass(row,n,&done)) {release();state(turn,false,"RSSI sweep failed");return;}
    state(turn,true,turn==LS_DW_ELRS24?"parameters unsupported; RSSI-burst fallback (not ELRS identity)":
        turn==LS_DW_WIDE24?"sequential RSSI; wide-link heuristic (possible)":"RSSI narrow bursts/hop cadence");
    if(done)ls_drone_watch_spectrum(turn,row,n,hf?2400000000u:860000000u,hf?2483500000u:930000000u,esp_timer_get_time());
}

#ifdef ESP_PLATFORM
extern const char _binary_defaults_json_start[],_binary_defaults_json_end[];
static EXT_RAM_BSS_ATTR StackType_t worker_stack[6144/sizeof(StackType_t)];
static StaticTask_t worker_tcb;
static TaskHandle_t worker_task;
static void worker(void *arg) {
    (void)arg;
    bool ok=ls_drone_watch_configure(_binary_defaults_json_start,
        (size_t)(_binary_defaults_json_end-_binary_defaults_json_start)-1);
    if(!ok) {state(LS_DW_ELRS900,false,LS_TEXT(t_ls_drone_watch_embedded_configuration_invalid, "embedded configuration invalid"));vTaskDelete(NULL);return;}
    /* Boot-only SD read, on RF worker, never on render/console tasks. */
    static EXT_RAM_BSS_ATTR char json[4097];
    FILE *f=fopen("/sdcard/drone-watch/settings.json","rb");
    if(f) {size_t n=fread(json,1,sizeof(json),f);bool read_ok=!ferror(f);fclose(f);
        if(!read_ok || !ls_drone_watch_configure(json,n)) {lock();LS_SNPUTS(t_ls_drone_watch_sd_schema_rejected_embedded, view.config, sizeof(view.config), "SD schema rejected; embedded v1 retained");unlock();}}
    for(;;) {
        ls_drone_watch_step(esp_timer_get_time());
        uint32_t p=prefs();bool active=((p&1) && (p&0x7c)) || __atomic_load_n(&legacy,__ATOMIC_ACQUIRE);
        vTaskDelay(pdMS_TO_TICKS(active?5:200));
    }
}
#endif
bool ls_drone_watch_start(void) {
    init();
#ifdef ESP_PLATFORM
    lock();
    if(!worker_task)worker_task=xTaskCreateStaticPinnedToCore(worker,"drone-watch",sizeof(worker_stack),NULL,1,worker_stack,&worker_tcb,0);
    unlock();
    if(worker_task)ls_wireless_wake();
    return worker_task!=NULL;
#else
    return true;
#endif
}
int ls_drone_watch_command(int argc,char **argv) {
    if(argc==2 && (!strcmp(argv[1],"on") || !strcmp(argv[1],"off")))return ls_drone_watch_enable(!strcmp(argv[1],"on"))?0:1;
    if(argc==4 && !strcmp(argv[1],"src")) {
        if(strcmp(argv[3],"on") && strcmp(argv[3],"off"))return 1;
        for(unsigned i=0;i<LS_DW_SOURCES;i++)if(!strcmp(argv[2],ls_drone_watch_source(i)))return ls_drone_watch_source_enable(i,!strcmp(argv[3],"on"))?0:1;
        return 1;
    }
    if(argc>2 || (argc==2 && strcmp(argv[1],"list")))return 1;
    if(argc==2) {
        /* One bounded entry at a time; don't put a 32-entry snapshot on the
         * console's internal stack or hold the lock across console output. */
        for(unsigned i=0;i<LS_DW_ENTRIES;i++) {
            lock();ls_drone_watch_entry_t e=tracks[i].pub;unlock();if(!e.id)continue;
            LS_PRINTF(t_ls_drone_watch_016llx_s_s_3f, "%016llx %s %s %.3f MHz %.1f %s peak %.1f trend %+.1f dB/10s %.2f hits/s confidence %u\n",
                (unsigned long long)e.id,ls_drone_watch_source(e.source),e.channel,e.centre_hz/1e6,
                e.rssi_dbm,e.calibrated?"dBm":"dB relative",e.peak_dbm,e.db_per_10s,e.hits_per_second,(unsigned)e.confidence);
        }
    } else {
        ls_drone_watch_status_t s;if(!ls_drone_watch_status(&s))return 1;
        LS_PRINTF(t_ls_drone_watch_drone_watch_s_alerts, "DRONE WATCH %s alerts %s cycle %lu ms; %s\n",s.enabled?"ON":"OFF",s.alerts?"ON":"OFF",(unsigned long)s.cycle_ms,s.config);
        for(unsigned i=0;i<LS_DW_SOURCES;i++)printf("%s: %s %s\n",ls_drone_watch_source(i),s.running[i]?"RUN":"SKIP",s.reason[i]);
    }
    return 0;
}
