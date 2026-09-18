#include "rec_watch.h"
#include "rec_state.h"
#include "ls_mixrf.h"
#include "ls_lora.h"
#include "ls_fsk_capture.h"
#include "ls_haptic.h"
#include "settings.h"
#include "ls_sub_fsk.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
    rec_source_t source;
    uint32_t hz; int edges, peak, reason; uint64_t ms;
    /* Carried with the capture rather than read from global settings when it
       is filed: the settings can be changed between hearing something and
       writing it down, and a record that describes the wrong ones cannot be
       replayed and cannot be told apart from one that can. */
    rec_fsk_mod_t mod; bool has_mod;
    int32_t pulse[REC_WATCH_EDGES];
} capture_t;
static rec_watch_catalog_t *s_catalog, *s_checkpoint;
static QueueHandle_t s_free, s_pending;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static EXT_RAM_BSS_ATTR rec_watch_status_t s_status;
static uint32_t s_pin_id;
static uint32_t s_export_id;
/* Queued the same way a pin or an export is: the catalog belongs to the
   storage worker, so the UI asks and the worker acts. */
static uint32_t s_replay_id; static int s_replay_dbm;
static void replay(uint32_t id,int dbm,char *result,size_t len);
static uint32_t s_scan_min,s_scan_max,s_scan_secs;
static int s_scan_bins=REC_SCAN_BINS;
static volatile bool s_scan_busy;
static EXT_RAM_BSS_ATTR rec_scan_bin_t s_scan[REC_SCAN_BINS];
static int s_scan_n; static float s_scan_floor;
static volatile int s_scan_pct;
static EXT_RAM_BSS_ATTR rec_scan_bin_t s_scan_live[REC_SCAN_BINS];
static int s_scan_live_n; static float s_scan_live_floor=-120.0f;
static volatile bool s_scan_stop_req;
static int s_scan_hits;
static int s_on_hit_loaded=-1;
static uint32_t s_last_hit;
static float s_gate_db=-1.0f;
float rec_watch_scan_threshold(void)
{
    /* Loaded on first use rather than at init: settings comes up after this
       translation unit's statics and a value read too early is the default
       forever. */
    if(s_gate_db<0)s_gate_db=(float)settings_get_subghz_gate_db();
    return s_gate_db;
}
void rec_watch_scan_set_threshold(float db)
{
    if(db<REC_SCAN_DETECT_MIN)db=REC_SCAN_DETECT_MIN;
    if(db>REC_SCAN_DETECT_MAX)db=REC_SCAN_DETECT_MAX;
    s_gate_db=db;
    settings_set_subghz_gate_db((int)(db+0.5f));
}
static volatile bool s_catch_armed;
void rec_watch_scan_on_hit(rec_scan_on_hit_t m)
{
    if(m>REC_SCAN_ON_HIT_CATCH)return;
    s_on_hit_loaded=(int)m;
    settings_set_subghz_on_hit((int)m);
}
rec_scan_on_hit_t rec_watch_scan_on_hit_get(void)
{
    if(s_on_hit_loaded<0)s_on_hit_loaded=settings_get_subghz_on_hit();
    return (rec_scan_on_hit_t)s_on_hit_loaded;
}
uint32_t rec_watch_scan_last_hit(void){ return s_last_hit; }
int rec_watch_scan_live(rec_scan_bin_t *out,int max)
{
    if(!out || max<=0)return 0;
    int n=s_scan_live_n<max?s_scan_live_n:max;
    for(int i=0;i<n;i++)out[i]=s_scan_live[i];
    return n;
}
float rec_watch_scan_live_floor(void){ return s_scan_live_floor; }
int  rec_watch_scan_hits(void){ return s_scan_hits; }
void rec_watch_scan_stop(void){ s_scan_stop_req=true; }
static EXT_RAM_BSS_ATTR char s_scan_stage[40];
int rec_watch_scan_progress(void){ return s_scan_pct; }
const char *rec_watch_scan_stage(void){ return s_scan_stage; }
static void stage(const char *what,int pct)
{
    snprintf(s_scan_stage,sizeof(s_scan_stage),"%s",what?what:"");
    s_scan_pct=pct<0?0:pct>100?100:pct;
}
static void scan_band(uint32_t min_hz,uint32_t max_hz,uint32_t secs,char *result,size_t len);
static uint32_t s_learn_secs;
static volatile bool s_learn_busy;
static EXT_RAM_BSS_ATTR rec_learn_t s_learn;
static void learn_run(uint32_t secs,char *result,size_t len);
static bool s_pin_value, s_starting;
static uint32_t s_boot;
static rec_source_t s_source;
/* One per source, sized off the enum. These were [2] and indexed by a
   source id, so a third receiver filing anything at all wrote past them. */
static uint32_t source_received[REC_SOURCE_COUNT],source_dropped[REC_SOURCE_COUNT];
static const char *DIR="/sdcard/subghz";
#define REC_WATCH_STACK_WORDS (6144 / sizeof(StackType_t))
static EXT_RAM_BSS_ATTR StackType_t s_worker_stack[REC_WATCH_STACK_WORDS];
static DRAM_ATTR StaticTask_t s_worker_tcb;

/* The mesh owns the SX1262 between sessions, so a watch on it has to ask.

   Declared rather than included: ls_mesh.h belongs to the meshcore component,
   which depends on this one, and requiring it back is a dependency cycle.
   Weak, so a build without a mesh links and reports the radio as free - which
   on such a build it is. */
__attribute__((weak)) bool ls_mesh_radio_hold(bool hold){(void)hold;return true;}
__attribute__((weak)) bool ls_mesh_radio_held(void){return true;}

__attribute__((weak)) bool rec_watch_notify(const char *peer, const char *text)
{ (void)peer; (void)text; return false; }

static void publish(void)
{
    rec_ook24_t decoded[REC_WATCH_SLOTS] = {0};
    for(int i=0;i<REC_WATCH_SLOTS;i++) if(s_catalog->record[i].event.id)
        rec_decode_ook24(s_catalog->record[i].pulse, s_catalog->record[i].event.edges, &decoded[i]);
    portENTER_CRITICAL(&s_lock);
    s_status.count=0;
    s_status.boot_id=s_boot;
    for(int i=0;i<REC_WATCH_SLOTS;i++) if(s_catalog->record[i].event.id) {
        int n=s_status.count++;
        s_status.event[n]=s_catalog->record[i].event;
        memcpy(s_status.preview[n],s_catalog->record[i].pulse,sizeof(s_status.preview[n]));
        s_status.decoded[n] = decoded[i];
    }
    s_status.ready=true;
    portEXIT_CRITICAL(&s_lock);
}
typedef struct { uint64_t last_alert; bool attempted_alert; } capture_flow_t;
static void consume_capture(capture_t *cap, capture_flow_t *flow)
{
    bool novel=false;
    int slot=rec_watch_observe_mod(s_catalog,cap->source,cap->hz,cap->pulse,cap->edges,
        s_boot,cap->ms,cap->peak,cap->reason,cap->has_mod?&cap->mod:NULL,&novel);
    uint64_t now=(uint64_t)esp_timer_get_time()/1000;
    char peer[17];
    portENTER_CRITICAL(&s_lock);
    bool enabled=s_status.enabled;
    memcpy(peer,s_status.peer,sizeof(peer));
    if(slot<0){s_status.dropped++;source_dropped[cap->source]++;} else {s_status.received++;source_received[cap->source]++;s_status.pending_save=true;}
    portEXIT_CRITICAL(&s_lock);
    rec_ook24_t qualified;
    bool confirmed=slot>=0 && ((novel && rec_decode_ook24(cap->pulse,cap->edges,&qualified)) || s_catalog->record[slot].event.count==2);
    if(confirmed && enabled && peer[0]) {
        if((!flow->attempted_alert || now-flow->last_alert>=60000) && now-cap->ms<5000) {
            char text[80];
            rec_ook24_t decoded;
            const char *source=rec_source_name(cap->source);
            if(rec_decode_ook24(cap->pulse,cap->edges,&decoded))
                snprintf(text,sizeof(text),"%s OOK24 %06lX %.4fMHz (%u matching frames)",
                    source,(unsigned long)decoded.value,cap->hz/1e6,decoded.repeats);
            else snprintf(text,sizeof(text),"%s RAW #%lu %.4fMHz %d edges",source,
                (unsigned long)s_catalog->record[slot].event.id,cap->hz/1e6,cap->edges);
            flow->last_alert=now;flow->attempted_alert=true;
            bool ok=rec_watch_notify(peer,text);
            portENTER_CRITICAL(&s_lock);
            if(ok)s_status.alert_sent++;else s_status.alert_failed++;
            portEXIT_CRITICAL(&s_lock);
        } else {
            portENTER_CRITICAL(&s_lock);s_status.alert_suppressed++;portEXIT_CRITICAL(&s_lock);
        }
    }
    xQueueSend(s_free,&cap,0);
    publish();
}
/* Called between sector operations; bounded by the existing two-buffer pool.
   The checkpoint image is frozen in PSRAM, so live observations stay writable. */
static void pump_capture(void *context)
{
    capture_t *cap=NULL;
    if(xQueueReceive(s_pending,&cap,0)==pdTRUE)
        consume_capture(cap,(capture_flow_t *)context);
}
static void worker(void *arg)
{
    (void)arg;
    bool restored=rec_watch_restore(DIR,s_catalog);
    portENTER_CRITICAL(&s_lock);
    snprintf(s_status.storage,sizeof(s_status.storage),"%s",restored?"SD archive restored":"RAM; awaiting SD checkpoint");
    portEXIT_CRITICAL(&s_lock);
    s_status.saved=restored;
    publish();
    uint64_t saved=s_catalog->sequence, checkpoint=0;
    capture_flow_t flow={0};
    bool was_enabled=false;
    for(;;) {
        capture_t *cap=NULL;
        if(xQueueReceive(s_pending,&cap,pdMS_TO_TICKS(100))==pdTRUE) {
            consume_capture(cap,&flow);
        }
        portENTER_CRITICAL(&s_lock);
        uint32_t pin=s_pin_id;bool value=s_pin_value;s_pin_id=0;
        uint32_t export_id=s_export_id;s_export_id=0;
        uint32_t replay_id=s_replay_id;int replay_dbm=s_replay_dbm;s_replay_id=0;
        uint32_t scan_min=s_scan_min,scan_max=s_scan_max,scan_secs=s_scan_secs;s_scan_min=0;
        uint32_t learn_secs=s_learn_secs;s_learn_secs=0;
        bool enabled=s_status.enabled;
        portEXIT_CRITICAL(&s_lock);
        if(pin) { rec_watch_pin(s_catalog,pin,value);portENTER_CRITICAL(&s_lock);s_status.pending_save=true;portEXIT_CRITICAL(&s_lock);publish(); }
        if(learn_secs) {
            char result[112];
            learn_run(learn_secs,result,sizeof(result));
            portENTER_CRITICAL(&s_lock);
            snprintf(s_status.export_status,sizeof(s_status.export_status),"%s",result);
            portEXIT_CRITICAL(&s_lock);
            publish();
        }
        if(scan_min) {
            char result[112];
            scan_band(scan_min,scan_max,scan_secs,result,sizeof(result));
            portENTER_CRITICAL(&s_lock);
            snprintf(s_status.export_status,sizeof(s_status.export_status),"%s",result);
            portEXIT_CRITICAL(&s_lock);
            publish();
            /* Started here rather than inside the sweep: the sweep still
               holds the radio at that point, and enabling the receiver from
               under it would be two owners of one part. */
            if(s_catch_armed) {
                s_catch_armed=false;
                if(rec_watch_enable(true)) {
                    portENTER_CRITICAL(&s_lock);
                    snprintf(s_status.export_status,sizeof(s_status.export_status),
                             "Caught %.4f MHz - receiver started",s_last_hit/1e6);
                    portEXIT_CRITICAL(&s_lock);
                } else {
                    portENTER_CRITICAL(&s_lock);
                    snprintf(s_status.export_status,sizeof(s_status.export_status),
                             "Found %.4f MHz - receiver would not start",s_last_hit/1e6);
                    portEXIT_CRITICAL(&s_lock);
                }
                publish();
            }
        }
        if(replay_id) {
            char result[112];
            replay(replay_id,replay_dbm,result,sizeof(result));
            portENTER_CRITICAL(&s_lock);
            snprintf(s_status.export_status,sizeof(s_status.export_status),"%s",result);
            portEXIT_CRITICAL(&s_lock);
            publish();
        }
        if(export_id) {
            uint64_t total=0,free_bytes=UINT64_MAX;
            char result[112];
            if(esp_vfs_fat_info("/sdcard",&total,&free_bytes)==ESP_OK) mkdir(DIR,0775);
            rec_watch_export(DIR,s_catalog,export_id,free_bytes,result,sizeof(result));
            portENTER_CRITICAL(&s_lock);
            snprintf(s_status.export_status,sizeof(s_status.export_status),"%s",result);
            s_status.exporting=false;
            portEXIT_CRITICAL(&s_lock);
        }
        uint64_t now=(uint64_t)esp_timer_get_time()/1000;
        /* Starting after a long idle must not spend the first received burst
           writing the previous archive while the two capture buffers fill. */
        if(enabled && !was_enabled)checkpoint=now;
        was_enabled=enabled;
        if(s_catalog->sequence!=saved && now-checkpoint >= (enabled?30000:3000)) {
            checkpoint=now;
            uint64_t total=0,free_bytes=UINT64_MAX;
            bool ok=esp_vfs_fat_info("/sdcard",&total,&free_bytes)==ESP_OK;
            if(ok) {
                memcpy(s_checkpoint,s_catalog,sizeof(*s_checkpoint));
                mkdir(DIR,0775);
                ok=rec_watch_store_pumped(DIR,s_checkpoint,free_bytes,pump_capture,&flow);
            }
            if(ok)saved=s_checkpoint->sequence;
            portENTER_CRITICAL(&s_lock);
            s_status.saved=ok;s_status.pending_save=!ok || saved!=s_catalog->sequence;s_status.save_failed=!ok;
            snprintf(s_status.storage,sizeof(s_status.storage),"%s",ok?"SD saved; 2 files, < 520 KiB total":"RAM ONLY: SD missing/full/write failed");
            portEXIT_CRITICAL(&s_lock);
        }
    }
}
bool rec_watch_start(void)
{
    portENTER_CRITICAL(&s_lock);
    bool ready=s_status.ready;
    if(s_starting || ready) { portEXIT_CRITICAL(&s_lock);return ready; }
    s_starting=true;
    portEXIT_CRITICAL(&s_lock);
    s_catalog=heap_caps_calloc(1,sizeof(*s_catalog),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    s_checkpoint=heap_caps_malloc(sizeof(*s_checkpoint),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    capture_t *pool=heap_caps_calloc(2,sizeof(*pool),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    s_free=xQueueCreate(2,sizeof(capture_t*));s_pending=xQueueCreate(2,sizeof(capture_t*));
    bool ok=s_catalog && s_checkpoint && pool && s_free && s_pending;
    if(ok) {
        for(int i=0;i<2;i++) {capture_t *p=&pool[i];xQueueSend(s_free,&p,0);}
        s_boot=esp_random();
        ok=xTaskCreateStaticPinnedToCore(worker,"subghz_io",
            REC_WATCH_STACK_WORDS,NULL,1,s_worker_stack,&s_worker_tcb,0)!=NULL;
    }
    if(!ok) {
        if(s_free)vQueueDelete(s_free);
        if(s_pending)vQueueDelete(s_pending);
        s_free=s_pending=NULL;heap_caps_free(pool);heap_caps_free(s_catalog);s_catalog=NULL;
        heap_caps_free(s_checkpoint);s_checkpoint=NULL;
        portENTER_CRITICAL(&s_lock);
        s_starting=false;
        snprintf(s_status.storage,sizeof(s_status.storage),"Monitor allocation failed");
        portEXIT_CRITICAL(&s_lock);
    }
    return ok;
}
/* What the SX1262 listens with. Deliberately generic - this is a receiver
   for whatever the operator points it at, and a default that matched one
   particular product would be a guess dressed as a setting. Changed through
   rec_watch_fsk_set. */
static rec_fsk_mod_t s_fsk;
static bool s_fsk_loaded;

void rec_watch_fsk_get(rec_fsk_mod_t *out)
{
    if(!s_fsk_loaded) {
        int pre=32,bw=59;
        settings_get_subghz_fsk(&s_fsk.bitrate,&s_fsk.deviation_hz,
                                &s_fsk.sync_word,&pre,&bw);
        s_fsk.preamble_bits=(uint16_t)pre;
        s_fsk.bandwidth_khz=(uint16_t)bw;
        s_fsk_loaded=true;
    }
    if(out)*out=s_fsk;
}

/* Why the last attempt to start did not. The screen used to print one
   sentence about the RTL and the CC1101 whatever had actually gone wrong,
   which on a third receiver was both untrue and too long for the line. */
static char s_fsk_error[72];
const char *rec_watch_error(void){ return s_fsk_error; }

bool rec_watch_fsk_set(const rec_fsk_mod_t *in)
{
    if(!in || rec_watch_enabled())return false;
    if(in->bitrate<600 || in->bitrate>300000)return false;
    if(in->deviation_hz<600 || in->deviation_hz>200000)return false;
    if(in->preamble_bits<8 || in->preamble_bits>1024)return false;
    s_fsk=*in;
    /* The modem cannot hear what will not fit its filter. Rather than refuse
       - which strands whoever is halfway through typing a rate and a
       deviation - widen to the narrowest rung that holds the signal, and let
       the screen show what it settled on. */
    const uint32_t need=(uint32_t)in->bitrate+2u*in->deviation_hz;
    uint32_t bw=ls_lora_fsk_bw_snap((uint32_t)in->bandwidth_khz*1000u);
    if(bw<need) bw=ls_lora_fsk_bw_snap(need);
    s_fsk.bandwidth_khz=(uint16_t)((bw+500u)/1000u);
    s_fsk_loaded=true;
    /* Worked out with a LEARN run or typed in by hand; either way it is the
       last thing anybody wants to do twice. */
    settings_set_subghz_fsk(s_fsk.bitrate,s_fsk.deviation_hz,s_fsk.sync_word,
                            s_fsk.preamble_bits,s_fsk.bandwidth_khz);
    return true;
}

#define REC_FSK_STACK_WORDS (4096 / sizeof(StackType_t))
static EXT_RAM_BSS_ATTR StackType_t s_fsk_stack[REC_FSK_STACK_WORDS];
static DRAM_ATTR StaticTask_t s_fsk_tcb;
static volatile bool s_fsk_run;

/* Its own task, and not a turn in the storage worker's loop.

   The worker writes the archive to the card inline, which takes far longer
   than the 60 ms a frame spends on air, so polling the radio from there
   drops exactly the frames somebody is standing at the bench pressing a
   button to produce. This does nothing but read the receiver. */
static void fsk_poller(void *arg)
{
    (void)arg;
    static EXT_RAM_BSS_ATTR uint8_t bits[LS_FSK_CAPTURE_BYTES*8+1024+32];
    static EXT_RAM_BSS_ATTR int32_t raw[LS_FSK_CAPTURE_BYTES*8+1024+32];
    while(s_fsk_run) {
        uint8_t packet[LS_FSK_CAPTURE_BYTES];
        float rssi=-200.0f;
        const int n=ls_lora_fsk_poll(packet,sizeof(packet),&rssi);
        if(n>0) {
            /* Stored as edge timings like every other source, so the archive,
               its card format, the preview strip and the .sub export all keep
               working without knowing this source exists. */
            ls_fsk_capture_t c;
            memset(&c,0,sizeof(c));
            c.freq_hz=rec_get_freq();
            c.bitrate=s_fsk.bitrate; c.deviation_hz=s_fsk.deviation_hz;
            c.bandwidth_hz=(uint32_t)s_fsk.bandwidth_khz*1000u;
            c.sync_word=s_fsk.sync_word; c.preamble_bits=s_fsk.preamble_bits;
            c.len=(uint8_t)(n>LS_FSK_CAPTURE_BYTES?LS_FSK_CAPTURE_BYTES:n);
            memcpy(c.data,packet,c.len);
            const size_t nb=ls_fsk_bitstream(&c,bits,sizeof(bits));
            const size_t nr=nb?ls_fsk_raw_data(bits,nb,c.bitrate,raw,
                                sizeof(raw)/sizeof(raw[0])):0;
            if(nr>=6 && nr<=REC_WATCH_EDGES)
                rec_watch_submit_mod(REC_SOURCE_SX1262,c.freq_hz,raw,(int)nr,
                                     (int)rssi,REC_END_GAP,&s_fsk);
        }
        vTaskDelay(pdMS_TO_TICKS(n>0?1:5));
    }
    vTaskDelete(NULL);
}

/* Take the radio, configure it, and start listening. The mesh owns the
   SX1262 the rest of the time, so this pauses it - loudly, by failing rather
   than half-starting when the mesh will not let go. */
static bool fsk_session(bool on)
{
    if(on) {
        if(s_fsk_run)return true;
        s_fsk_error[0]=0;
        const int64_t deadline=esp_timer_get_time()+1000000;
        while(!ls_mesh_radio_hold(true) && esp_timer_get_time()<deadline)
            vTaskDelay(pdMS_TO_TICKS(10));
        if(!ls_mesh_radio_held()){
            ls_mesh_radio_hold(false);
            snprintf(s_fsk_error,sizeof(s_fsk_error),"Mesh would not release the radio");
            return false;
        }
        ls_fsk_cfg_t cfg={0};
        cfg.freq_hz=rec_get_freq();
        cfg.bitrate=s_fsk.bitrate;
        cfg.deviation_hz=s_fsk.deviation_hz;
        /* Snapped, not passed through: a bandwidth between two rungs of the
           filter ladder is refused, and the refusal reads as a session that
           will not start for no reason. */
        cfg.bandwidth_hz=ls_lora_fsk_bw_snap((uint32_t)s_fsk.bandwidth_khz*1000u);
        if(cfg.bitrate+2u*cfg.deviation_hz>cfg.bandwidth_hz)
            cfg.bandwidth_hz=ls_lora_fsk_bw_snap(cfg.bitrate+2u*cfg.deviation_hz);
        cfg.sync_word=s_fsk.sync_word;
        cfg.preamble_bits=s_fsk.preamble_bits;
        cfg.payload_bytes=LS_FSK_CAPTURE_BYTES;
        const esp_err_t err=ls_lora_fsk_begin(&cfg);
        if(err!=ESP_OK){
            ls_mesh_radio_hold(false);
            snprintf(s_fsk_error,sizeof(s_fsk_error),"SX1262 refused %lu bd / %.1f kHz dev",
                (unsigned long)cfg.bitrate,cfg.deviation_hz/1000.0);
            return false;
        }
        s_fsk_run=true;
        if(!xTaskCreateStaticPinnedToCore(fsk_poller,"subghz_fsk",
                REC_FSK_STACK_WORDS,NULL,4,s_fsk_stack,&s_fsk_tcb,0)) {
            s_fsk_run=false;
            ls_lora_fsk_end();
            ls_mesh_radio_hold(false);
            snprintf(s_fsk_error,sizeof(s_fsk_error),"No memory for the receive task");
            return false;
        }
        return true;
    }
    if(!s_fsk_run)return true;
    s_fsk_run=false;
    /* Let the poller see the flag and leave before the radio moves under it. */
    vTaskDelay(pdMS_TO_TICKS(30));
    ls_lora_fsk_end();
    ls_mesh_radio_hold(false);
    return true;
}

bool rec_watch_enable(bool on)
{
    portENTER_CRITICAL(&s_lock);bool ready=s_status.ready;portEXIT_CRITICAL(&s_lock);
    if(on && !ready)return false;
    if (rec_watch_source() == REC_SOURCE_CC1101 && !ls_mixrf_capture(on, rec_get_freq()))
        return false;
    if (rec_watch_source() == REC_SOURCE_SX1262 && !fsk_session(on))
        return false;
    portENTER_CRITICAL(&s_lock);
    bool ok=!on || s_status.ready;
    if(ok)s_status.enabled=on;
    portEXIT_CRITICAL(&s_lock);
    return ok;
}
bool rec_watch_enabled(void)
{
    portENTER_CRITICAL(&s_lock);bool on=s_status.enabled;portEXIT_CRITICAL(&s_lock);return on;
}
/* Copy the slots that hold something, not all sixteen of them.
 *
 * `*out = s_status` moved 4712 bytes, and it moved them with interrupts off:
 * both the catalog and the screen's copy live in PSRAM, portENTER_CRITICAL
 * disables interrupts on the core that takes it, and scr_subghz calls this
 * once per drawn frame. Idle - nothing captured - is the case that paid the
 * most for the least, copying sixteen empty slots every frame.
 *
 * Nothing is lost by stopping at `count`, because nothing past it was ever
 * populated: publish() writes entries 0..count-1 and leaves the rest holding
 * whatever an older, longer catalog left there. So the old whole-struct copy
 * was already delivering stale bytes past `count`, every consumer already
 * bounds itself by it, and this only declines to carry them across.
 */
void rec_watch_snapshot(rec_watch_status_t *out)
{
    if(!out)return;
    portENTER_CRITICAL(&s_lock);
    rec_watch_status_copy(out,&s_status,s_status.count);
    rec_source_t source=s_source;
    out->received=source_received[source];
    out->dropped=source_dropped[source];
    portEXIT_CRITICAL(&s_lock);
    rec_watch_filter_status(out,source);
}
void rec_watch_submit(uint32_t hz,const int32_t *pulse,int edges,int peak,int reason)
{
    rec_watch_submit_from(REC_SOURCE_RTL, hz, pulse, edges, peak, reason);
}
void rec_watch_submit_from(rec_source_t source,uint32_t hz,const int32_t *pulse,int edges,int peak,int reason)
{
    rec_watch_submit_mod(source,hz,pulse,edges,peak,reason,NULL);
}
void rec_watch_submit_mod(rec_source_t source,uint32_t hz,const int32_t *pulse,int edges,int peak,int reason,const rec_fsk_mod_t *mod)
{
    if(!pulse || edges<6 || edges>REC_WATCH_EDGES)return;
    uint64_t now=(uint64_t)esp_timer_get_time()/1000;
    capture_t *cap=NULL;
    portENTER_CRITICAL(&s_lock);
    if(source!=s_source || !s_status.enabled) {portEXIT_CRITICAL(&s_lock);return;}
    /* The fixed two-buffer queue bounds work; a time gate loses real bursts
       when raw noise arrives immediately before a transmitter. */
    portEXIT_CRITICAL(&s_lock);
    if(xQueueReceive(s_free,&cap,0)!=pdTRUE) {
        portENTER_CRITICAL(&s_lock);s_status.dropped++;source_dropped[source]++;portEXIT_CRITICAL(&s_lock);return;
    }
    cap->source=source;cap->hz=hz;cap->edges=edges;cap->peak=peak;cap->reason=reason;cap->ms=now;
    cap->has_mod=mod!=NULL;
    if(mod)cap->mod=*mod; else memset(&cap->mod,0,sizeof(cap->mod));
    memcpy(cap->pulse,pulse,(size_t)edges*sizeof(*pulse));
    xQueueSend(s_pending,&cap,0);
}
rec_source_t rec_watch_source(void)
{
    portENTER_CRITICAL(&s_lock);rec_source_t source=s_source;portEXIT_CRITICAL(&s_lock);
    return source;
}
bool rec_watch_select_source(rec_source_t source)
{
    if (source >= REC_SOURCE_COUNT) return false;
    portENTER_CRITICAL(&s_lock);
    bool ok = !s_status.enabled;
    if (ok) s_source = source;
    portEXIT_CRITICAL(&s_lock);
    if (ok && source == REC_SOURCE_CC1101) ls_mixrf_start();
    return ok;
}
bool rec_watch_request_replay(uint32_t id,int dbm)
{
    if(!id || dbm<-9 || dbm>22)return false;
    portENTER_CRITICAL(&s_lock);
    bool ok=!s_replay_id && !s_status.enabled;
    if(ok){s_replay_id=id;s_replay_dbm=dbm;}
    portEXIT_CRITICAL(&s_lock);
    return ok;
}

/* Send one capture. Runs on the storage worker because that is where the
   catalog is owned; it is a deliberate keypress, not a stream, so the few
   hundred milliseconds it costs that worker are not competing with
   anything. */
static void replay(uint32_t id,int dbm,char *result,size_t len)
{
    const rec_watch_record_t *r=NULL;
    for(int i=0;i<REC_WATCH_SLOTS;i++)
        if(s_catalog->record[i].event.id==id) r=&s_catalog->record[i];
    if(!r){snprintf(result,len,"Replay: no capture #%lu",(unsigned long)id);return;}
    if(!rec_source_can_replay((rec_source_t)r->event.source)) {
        snprintf(result,len,"Replay: %s cannot transmit",
                 rec_source_name((rec_source_t)r->event.source));
        return;
    }
    if(!r->event.bitrate) {
        snprintf(result,len,"Replay: capture has no modulation recorded");
        return;
    }
    static EXT_RAM_BSS_ATTR uint8_t payload[LS_FSK_CAPTURE_BYTES];
    const size_t n=ls_fsk_payload_from_raw(r->pulse,r->event.edges,
        r->event.bitrate,r->event.preamble_bits,payload,sizeof(payload));
    if(!n){snprintf(result,len,"Replay: could not rebuild the frame");return;}

    const int64_t deadline=esp_timer_get_time()+1000000;
    while(!ls_mesh_radio_hold(true) && esp_timer_get_time()<deadline)
        vTaskDelay(pdMS_TO_TICKS(10));
    if(!ls_mesh_radio_held()) {
        ls_mesh_radio_hold(false);
        snprintf(result,len,"Replay: mesh would not release the radio");
        return;
    }
    ls_fsk_cfg_t cfg={0};
    cfg.freq_hz=r->event.frequency;
    cfg.bitrate=r->event.bitrate;
    cfg.deviation_hz=r->event.deviation_hz;
    cfg.bandwidth_hz=ls_lora_fsk_bw_snap(cfg.bitrate+2u*cfg.deviation_hz);
    cfg.sync_word=r->event.sync_word;
    cfg.preamble_bits=r->event.preamble_bits;
    cfg.payload_bytes=(uint8_t)n;
    cfg.power_dbm=(int8_t)dbm;
    esp_err_t err=ls_lora_fsk_begin(&cfg);
    if(err==ESP_OK) {
        err=ls_lora_fsk_send(payload,n);
        if(err==ESP_OK) {
            const int64_t end=esp_timer_get_time()+2000000;
            while(!ls_lora_send_done() && esp_timer_get_time()<end)
                vTaskDelay(pdMS_TO_TICKS(5));
        }
        const esp_err_t ended=ls_lora_fsk_end();
        if(err==ESP_OK)err=ended;
    }
    ls_mesh_radio_hold(false);
    if(err==ESP_OK) snprintf(result,len,"Sent #%lu, %u bytes at %d dBm",
        (unsigned long)id,(unsigned)n,dbm);
    else snprintf(result,len,"Replay failed: %s",esp_err_to_name(err));
}

bool rec_watch_request_scan(uint32_t min_hz,uint32_t max_hz,uint32_t seconds,
                            int bins)
{
    if(min_hz<150000000u || max_hz>960000000u || max_hz<=min_hz)return false;
    /* 0 means run until stopped. */
    if(seconds>180)return false;
    if(bins<4 || bins>REC_SCAN_BINS)bins=REC_SCAN_BINS;
    portENTER_CRITICAL(&s_lock);
    bool ok=!s_scan_min && !s_status.enabled;
    if(ok){s_scan_min=min_hz;s_scan_max=max_hz;s_scan_secs=seconds;s_scan_bins=bins;}
    portEXIT_CRITICAL(&s_lock);
    return ok;
}
bool rec_watch_scan_busy(void){ return s_scan_busy; }
int rec_watch_scan_result(rec_scan_bin_t *out,int max)
{
    if(!out || max<=0)return 0;
    int n=s_scan_n<max?s_scan_n:max;
    for(int i=0;i<n;i++)out[i]=s_scan[i];
    return n;
}
float rec_watch_scan_floor(void){ return s_scan_floor; }

/* Sweep, and keep what each bin heard at its loudest.

   One pass takes about 200 ms and a button press lasts 60, so a single sweep
   answers "was it transmitting exactly then" - which is almost never the
   question. Repeating for the whole window and keeping per-bin peaks answers
   "did anything speak here while I was listening", which is. */
static void scan_band(uint32_t min_hz,uint32_t max_hz,uint32_t secs,char *result,size_t len)
{
    s_scan_busy=true;
    s_scan_n=0;
    stage("Taking the radio",0);
    const int64_t deadline=esp_timer_get_time()+1000000;
    while(!ls_mesh_radio_hold(true) && esp_timer_get_time()<deadline)
        vTaskDelay(pdMS_TO_TICKS(10));
    if(!ls_mesh_radio_held()) {
        ls_mesh_radio_hold(false);
        s_scan_busy=false;
        snprintf(result,len,"Scan: mesh would not release the radio");
        return;
    }
    if(ls_lora_scan_begin(min_hz,max_hz)!=ESP_OK) {
        ls_mesh_radio_hold(false);
        s_scan_busy=false;
        snprintf(result,len,"Scan: the receiver refused that range");
        return;
    }
    static EXT_RAM_BSS_ATTR float peak[REC_SCAN_BINS];
    static EXT_RAM_BSS_ATTR float row[REC_SCAN_BINS];
    const int nb=s_scan_bins;
    for(int i=0;i<nb;i++)peak[i]=-200.0f;
    const int64_t start=esp_timer_get_time();
    /* Zero means keep going until stopped. Capped anyway, because a sweep
       holds the radio the mesh needs and an instrument left running by
       accident should not take the node down for the rest of the day. */
    const int64_t end=start+(int64_t)(secs?secs:180)*1000000;
    int passes=0;
    s_scan_stop_req=false;
    s_scan_hits=0;
    s_scan_n=0;
    /* The live spectrum is per sweep too. Left set, it means "a pass
       has already been drawn", so the first pass of the NEXT sweep
       blends its readings into the previous band's instead of snapping
       to them - and a peak hold that decays 0.8 dB a pass keeps a
       marker from a band nobody is looking at any more for a minute. */
    s_scan_live_n=0;
    /* ls_lora_scan_pass, not ls_lora_scan_sweep.

       Sweep takes every look for every bin before it returns, which on this
       task - pinned to core 0 - starves that core's idle task for seconds at
       a time and the task watchdog kills the firmware. Pass does a bounded
       batch with a 20 ms budget and comes back, so there is somewhere to
       yield from and somewhere to report progress from. Both of those were
       the same bug: it looked frozen because it was, right up until it was
       killed for it. */
    while(esp_timer_get_time()<end && !s_scan_stop_req) {
        bool row_done=false;
        const int got=ls_lora_scan_pass(row,nb,&row_done);
        if(got<=0)break;
        if(row_done) {
            for(int i=0;i<nb;i++) if(row[i]>peak[i]) peak[i]=row[i];
            passes++;

            /* Publish the row as it is, for drawing, and work out the floor
               from this pass rather than from the accumulated peaks - the
               peaks only ever rise, so a floor taken from them would drift
               up and stop detecting anything. */
            float lo=row[0];
            for(int i=1;i<nb;i++) if(row[i]<lo) lo=row[i];
            /* The floor gets ballistics too, or the whole display shifts
               under the columns every pass. */
            if(s_scan_live_n) s_scan_live_floor += (lo-s_scan_live_floor)*0.25f;
            else              s_scan_live_floor = lo;

            for(int i=0;i<nb;i++) {
                s_scan_live[i].hz=ls_lora_scan_bin_hz(min_hz,max_hz,nb,i);
                if(!s_scan_live_n) { s_scan_live[i].dbm=row[i]; s_scan_live[i].hold=row[i]; continue; }
                /* Instant attack, slow release: a signal appears the pass it
                   arrives, and noise does not flicker between passes. */
                if(row[i] > s_scan_live[i].dbm) s_scan_live[i].dbm = row[i];
                else s_scan_live[i].dbm += (row[i]-s_scan_live[i].dbm)*0.30f;
                /* And a peak that falls about a dB a pass, so a burst that
                   landed between looks at this bin still leaves a mark. */
                if(row[i] > s_scan_live[i].hold) s_scan_live[i].hold = row[i];
                else s_scan_live[i].hold -= 0.8f;
                if(s_scan_live[i].hold < s_scan_live[i].dbm)
                    s_scan_live[i].hold = s_scan_live[i].dbm;
            }
            s_scan_live_n=nb;

            /* Against the RAW reading, not the smoothed one. Ballistics
               exist so a person can read the display; letting them decide
               what counts as a detection would mean the release curve, not
               the receiver, chose what got recorded. */
            int fresh_hits=0; uint32_t fresh_hz=0; float fresh_dbm=-200.0f;
            for(int i=0;i<nb;i++) {
                if(row[i] < lo+rec_watch_scan_threshold()) continue;
                const uint32_t hz=s_scan_live[i].hz;
                int at=-1;
                for(int j=0;j<s_scan_n;j++) if(s_scan[j].hz==hz){at=j;break;}
                const int64_t now_us=esp_timer_get_time();
                if(at>=0) {
                    if(row[i]>s_scan[at].dbm) s_scan[at].dbm=row[i];
                    if(s_scan[at].seen<UINT32_MAX) s_scan[at].seen++;
                    s_scan[at].last_us=now_us;
                }
                else if(s_scan_n<REC_SCAN_BINS) {
                    s_scan[s_scan_n].hz=hz; s_scan[s_scan_n].dbm=row[i];
                    s_scan[s_scan_n].seen=1;
                    s_scan[s_scan_n].first_us=s_scan[s_scan_n].last_us=now_us;
                    s_scan_n++; s_scan_hits++;
                    if(row[i]>fresh_dbm){fresh_dbm=row[i];fresh_hz=hz;}
                    fresh_hits++;
                }
            }
            /* Once per pass, for the strongest new thing in it. A buzz per
               bin would fire thirty-two times for one transmitter. */
            if(fresh_hits && fresh_hz) {
                s_last_hit=fresh_hz;
                const rec_scan_on_hit_t mode=rec_watch_scan_on_hit_get();
                /* At most one alert every two seconds, whatever the
                   threshold is set to. A threshold below the noise makes
                   every bin a detection, and that once meant one buzz per
                   bin until the list filled - thirty-two of them. A limit
                   here means a bad setting is merely a bad setting rather
                   than the device shaking itself across the bench. */
                static int64_t last_alert;
                const int64_t now_alert=esp_timer_get_time();
                const bool may_alert=now_alert-last_alert>2000000;
                if(mode!=REC_SCAN_ON_HIT_NOTHING && may_alert) {
                    last_alert=now_alert;
                    ls_haptic_play(LS_HAPTIC_ALERT);
                    char peer[17];
                    portENTER_CRITICAL(&s_lock);
                    bool alerts=s_status.alerts;
                    memcpy(peer,s_status.peer,sizeof(peer));
                    portEXIT_CRITICAL(&s_lock);
                    if(alerts && peer[0]) {
                        char text[80];
                        snprintf(text,sizeof(text),"SubGHz: %.4f MHz at %.0f dBm",
                                 fresh_hz/1e6,(double)fresh_dbm);
                        rec_watch_notify(peer,text);
                    }
                }
                /* And the hand-off: stop looking around, point at it, and
                   let the receiver try to make a capture out of it. */
                if(mode==REC_SCAN_ON_HIT_CATCH) {
                    rec_set_freq(fresh_hz);
                    s_scan_stop_req=true;
                    s_catch_armed=true;
                }
            }
        }
        const int64_t now=esp_timer_get_time();
        char what[40];
        snprintf(what,sizeof(what),"%d pass%s, %d found",passes,passes==1?"":"es",s_scan_hits);
        /* Elapsed rather than a fraction of a deadline: this runs until it is
           stopped, so there is no fraction to be a fraction of. */
        stage(what,(int)(((now-start)/1000000)%100));
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    ls_lora_scan_end();
    stage("",0);
    ls_mesh_radio_hold(false);

    if(!passes) {
        s_scan_busy=false;
        snprintf(result,len,"Scan: no sweep completed");
        return;
    }
    /* The quietest bin THIS sweep measured. Only the first nb entries
       were written; peak[] is a static that outlives the call, so
       reducing over all of REC_SCAN_BINS mixes in whatever a previous
       sweep of a different band left in the tail - which a narrow
       sweep after a wide one reports as a floor tens of dB off. */
    float lo=peak[0];
    for(int i=1;i<nb;i++) if(peak[i]<lo) lo=peak[i];
    s_scan_floor=lo;

    /* Strongest first, so the list opens on the thing worth looking at. */
    for(int i=1;i<s_scan_n;i++) {
        rec_scan_bin_t k=s_scan[i]; int j=i-1;
        while(j>=0 && s_scan[j].dbm<k.dbm){s_scan[j+1]=s_scan[j];j--;}
        s_scan[j+1]=k;
    }
    s_scan_busy=false;
    if(s_scan_n) snprintf(result,len,"Scan: %d pass%s, %d found, strongest %.0f dBm",
        passes,passes==1?"":"es",s_scan_n,(double)s_scan[0].dbm);
    else snprintf(result,len,"Scan: %d pass%s, nothing crossed %.0f dB over the floor",
        passes,passes==1?"":"es",(double)rec_watch_scan_threshold());
}

bool rec_watch_request_learn(uint32_t seconds)
{
    if(seconds<5 || seconds>90)return false;
    portENTER_CRITICAL(&s_lock);
    bool ok=!s_learn_secs && !s_status.enabled;
    if(ok)s_learn_secs=seconds;
    portEXIT_CRITICAL(&s_lock);
    return ok;
}
bool rec_watch_learn_busy(void){ return s_learn_busy; }
bool rec_watch_learn_result(rec_learn_t *out)
{
    if(!out)return false;
    *out=s_learn;
    return s_learn.valid;
}
bool rec_watch_learn_apply(void)
{
    if(!s_learn.valid)return false;
    rec_fsk_mod_t m; rec_watch_fsk_get(&m);
    m.bitrate=s_learn.bitrate;
    m.deviation_hz=s_learn.deviation_hz;
    m.sync_word=s_learn.sync_word;
    if(s_learn.preamble_bits)m.preamble_bits=s_learn.preamble_bits;
    return rec_watch_fsk_set(&m);
}

/* The two tones, and how far apart they are.

   This is the only one of the three answers that is a measurement. A 2-FSK
   carrier sits as two humps either side of centre, so a fine sweep finds
   them without demodulating anything - which matters, because the
   demodulator cannot be configured until the deviation is roughly right.
   Returns 0 when it cannot see two. */
static uint32_t measure_deviation(uint32_t centre,uint32_t secs)
{
    const uint32_t span=120000u;
    if(centre<span || ls_lora_scan_begin(centre-span/2,centre+span/2)!=ESP_OK)
        return 0;
    static EXT_RAM_BSS_ATTR float peak[REC_SCAN_BINS],row[REC_SCAN_BINS];
    for(int i=0;i<REC_SCAN_BINS;i++)peak[i]=-200.0f;
    const int64_t start=esp_timer_get_time();
    const int64_t end=start+(int64_t)secs*1000000;
    while(esp_timer_get_time()<end) {
        bool row_done=false;
        const int got=ls_lora_scan_pass(row,REC_SCAN_BINS,&row_done);
        if(got<=0)break;
        if(row_done) for(int i=0;i<REC_SCAN_BINS;i++) if(row[i]>peak[i])peak[i]=row[i];
        stage("Measuring the tones",
              (int)((esp_timer_get_time()-start)*25/((int64_t)secs*1000000)));
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    ls_lora_scan_end();

    float lo=peak[0],hi=peak[0];
    for(int i=1;i<REC_SCAN_BINS;i++){if(peak[i]<lo)lo=peak[i];if(peak[i]>hi)hi=peak[i];}
    /* Nothing transmitted while we looked. */
    if(hi-lo<8.0f)return 0;
    const float gate=lo+(hi-lo)*0.5f;
    int first=-1,last=-1;
    for(int i=0;i<REC_SCAN_BINS;i++) if(peak[i]>=gate){ if(first<0)first=i; last=i; }
    if(first<0 || last<=first)return 0;
    /* Two tones means two humps with a dip between them. Without this
       test the outermost bins above the gate are measured whatever is
       under them, so one carrier two bins wide reports a deviation of
       half a bin - inside the sanity range, so it is returned as a
       measurement, and LEARN then opens a filter far too narrow to hear
       anything while telling the operator it measured the signal.
       Reporting nothing is the honest answer: the caller falls back to
       the configured deviation, which is at least one somebody chose. */
    bool valley=false;
    for(int i=first+1;i<last;i++) if(peak[i]<gate){valley=true;break;}
    if(!valley)return 0;
    const uint32_t a=ls_lora_scan_bin_hz(centre-span/2,centre+span/2,REC_SCAN_BINS,first);
    const uint32_t b=ls_lora_scan_bin_hz(centre-span/2,centre+span/2,REC_SCAN_BINS,last);
    /* Half the separation between the outer edges of the two tones. */
    const uint32_t dev=(b-a)/2;
    return (dev>=600 && dev<=200000)?dev:0;
}

#define LEARN_KEEP 6
#define LEARN_BYTES 24

/* Try a rate, and see whether the same frame comes out the same way twice.

   A wrong bit rate slices the same burst differently every time, so the
   captures disagree. The right one produces identical bytes. That is the
   whole test, and it is why this needs somebody pressing the button
   repeatedly rather than one transmission. */
static int try_rate(uint32_t hz,uint32_t rate,uint32_t dev,uint32_t secs,
                    uint8_t *best,int *best_len)
{
    ls_fsk_cfg_t cfg={0};
    cfg.freq_hz=hz;
    cfg.bitrate=rate;
    cfg.deviation_hz=dev;
    cfg.bandwidth_hz=ls_lora_fsk_bw_snap(rate+2u*dev);
    /* 0xAA repeated: eight bits of the preamble itself, which is the one
       pattern every frame is guaranteed to contain before its real sync. */
    cfg.sync_word=0xAAAAAAAAu;
    cfg.sync_bits=8;
    cfg.preamble_bits=16;
    cfg.payload_bytes=LEARN_BYTES;
    if(ls_lora_fsk_begin(&cfg)!=ESP_OK)return 0;

    static uint8_t seen[LEARN_KEEP][LEARN_BYTES];
    int n=0;
    const int64_t end=esp_timer_get_time()+(int64_t)secs*1000000;
    while(esp_timer_get_time()<end && n<LEARN_KEEP) {
        uint8_t buf[LEARN_BYTES]; float rssi;
        const int got=ls_lora_fsk_poll(buf,sizeof(buf),&rssi);
        if(got>0){memcpy(seen[n],buf,LEARN_BYTES);n++;}
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    ls_lora_fsk_end();
    if(n<2)return n?1:0;

    int best_count=0,best_i=0;
    for(int i=0;i<n;i++) {
        int c=0;
        for(int j=0;j<n;j++) if(!memcmp(seen[i],seen[j],LEARN_BYTES))c++;
        if(c>best_count){best_count=c;best_i=i;}
    }
    if(best_count>=2 && best){memcpy(best,seen[best_i],LEARN_BYTES);*best_len=LEARN_BYTES;}
    return best_count;
}

/* The sync word is whatever follows the preamble run. */
static uint32_t sync_from(const uint8_t *b,int len)
{
    int i=0;
    while(i<len && (b[i]==0xAA || b[i]==0x55))i++;
    if(i+4>len)return 0;
    return ((uint32_t)b[i]<<24)|((uint32_t)b[i+1]<<16)|
           ((uint32_t)b[i+2]<<8)|b[i+3];
}

static void learn_run(uint32_t secs,char *result,size_t len)
{
    static const uint32_t RATE[]={600,1200,2400,4800,9600,19200,38400};
    const int n_rate=(int)(sizeof(RATE)/sizeof(RATE[0]));
    s_learn_busy=true;
    memset(&s_learn,0,sizeof(s_learn));
    stage("Taking the radio",0);

    const int64_t deadline=esp_timer_get_time()+1000000;
    while(!ls_mesh_radio_hold(true) && esp_timer_get_time()<deadline)
        vTaskDelay(pdMS_TO_TICKS(10));
    if(!ls_mesh_radio_held()) {
        ls_mesh_radio_hold(false); s_learn_busy=false;
        snprintf(result,len,"Learn: mesh would not release the radio");
        return;
    }

    const uint32_t hz=rec_get_freq();
    /* A quarter of the window on the measurement, the rest on the search. */
    uint32_t dev=measure_deviation(hz,secs/4?secs/4:1);
    bool measured=dev!=0;
    if(!dev) { rec_fsk_mod_t m; rec_watch_fsk_get(&m); dev=m.deviation_hz; }

    const uint32_t each=(secs-(secs/4))/(uint32_t)n_rate;
    uint8_t best[LEARN_BYTES]; int best_len=0, best_agreed=0; uint32_t best_rate=0;
    int tried=0;
    for(int i=0;i<n_rate;i++) {
        uint8_t got[LEARN_BYTES]; int got_len=0;
        char what[40];
        snprintf(what,sizeof(what),"Trying %lu baud",(unsigned long)RATE[i]);
        stage(what,25+i*75/n_rate);
        const int agreed=try_rate(hz,RATE[i],dev,each?each:1,got,&got_len);
        tried++;
        if(agreed>best_agreed){best_agreed=agreed;best_rate=RATE[i];
            if(got_len){memcpy(best,got,LEARN_BYTES);best_len=got_len;}}
    }
    ls_mesh_radio_hold(false);
    stage("",0);

    s_learn.tried=(uint8_t)tried;
    s_learn.agreed=(uint8_t)best_agreed;
    s_learn.deviation_hz=dev;
    if(best_agreed>=2 && best_rate) {
        s_learn.bitrate=best_rate;
        s_learn.sync_word=sync_from(best,best_len);
        s_learn.confidence=(uint8_t)(best_agreed*100/LEARN_KEEP);
        s_learn.valid=s_learn.sync_word!=0;
        snprintf(s_learn.note,sizeof(s_learn.note),"%s deviation, %d of %d agreed",
                 measured?"measured":"assumed",best_agreed,LEARN_KEEP);
        snprintf(result,len,"Learn: %lu baud, sync %08lX, %u%% confident",
            (unsigned long)best_rate,(unsigned long)s_learn.sync_word,
            s_learn.confidence);
    } else {
        snprintf(s_learn.note,sizeof(s_learn.note),"nothing repeated at any rate");
        snprintf(result,len,"Learn: heard nothing consistent - press it repeatedly");
    }
    s_learn_busy=false;
}

bool rec_watch_request_pin(uint32_t id,bool pin)
{
    portENTER_CRITICAL(&s_lock);
    bool ok=id && !s_pin_id && s_status.ready;
    if(ok){s_pin_id=id;s_pin_value=pin;}
    portEXIT_CRITICAL(&s_lock);return ok;
}
bool rec_watch_alert_target(const char *peer)
{
    if(!peer || (peer[0] && strlen(peer)!=16))return false;
    for(const char *p=peer;*p;p++)if(!((*p>='0'&&*p<='9')||(*p>='a'&&*p<='f')||(*p>='A'&&*p<='F')))return false;
    portENTER_CRITICAL(&s_lock);
    snprintf(s_status.peer,sizeof(s_status.peer),"%s",peer);
    s_status.alerts=peer[0]!=0;
    portEXIT_CRITICAL(&s_lock);return true;
}

bool rec_watch_request_export(uint32_t id)
{
    portENTER_CRITICAL(&s_lock);
    bool found=false;
    for(int i=0;i<s_status.count;i++) if(s_status.event[i].id==id) found=true;
    bool ok=id && found && s_status.ready && !s_status.exporting;
    if(ok) {
        s_export_id=id;s_status.exporting=true;
        snprintf(s_status.export_status,sizeof(s_status.export_status),"Export queued; saving selected pattern");
    }
    portEXIT_CRITICAL(&s_lock);return ok;
}
