#include "esp_log.h"
#include "rec_watch.h"
#include "rec_state.h"
#include "ls_mixrf.h"
#include "ls_mixrf_control.h"
#include "ls_lora.h"
#include "ls_fsk_capture.h"
#include "rec_lr_ook.h"
#include "rec_scan_live.h"
#include "lr_wf_session.h"
#include "ls_haptic.h"
#include "settings.h"
#include "ls_sub_fsk.h"
#include "subghz_file.h"
#include "ls_det_signal.h"
#pragma weak ls_seen_signal
#include "rec_ism.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "ls_decoded.h"
#include "ls_tmach_ring.h"
#include "ls_textpack.h"

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
static bool s_replay_busy;
static bool s_iq_busy,s_iq_pending;
static char s_replay_result[112];
static uint32_t s_replay_id; static int s_replay_dbm;
static void replay(uint32_t id,int dbm,char *result,size_t len);
/* A file to send instead of a catalog entry, for the FILES app. */
static char s_replay_path[160];
static void replay_file(const char *path,int dbm,char *result,size_t len);
static uint32_t s_scan_min,s_scan_max,s_scan_secs;
static int s_scan_bins=REC_SCAN_BINS;
static TaskHandle_t s_worker_task;
static bool s_watch_pending, s_watch_waiting, s_watch_on, s_watch_result;
/* Set while the worker is acting on a request: it then finishes, and the
   caller waits for it rather than withdrawing a request already underway. */
static bool s_watch_taken;
static bool rec_watch_enable_owned(bool on);
static volatile bool s_scan_busy;
static rec_source_t s_scan_source;
static EXT_RAM_BSS_ATTR rec_scan_bin_t s_scan[REC_SCAN_BINS];
static int s_scan_n; static float s_scan_floor;
static volatile int s_scan_pct;
static EXT_RAM_BSS_ATTR rec_scan_bin_t s_scan_live[REC_SCAN_BINS];
static int s_scan_live_n; static float s_scan_live_floor=-120.0f, s_scan_live_spread;
static volatile bool s_scan_stop_req;
static int s_scan_hits;
/* Every time a bin rises through the threshold - a burst, not a frequency.
   A remote pressed five times on one frequency is five of these and one
   entry in s_scan. */
static volatile int s_scan_events;
static EXT_RAM_BSS_ATTR bool s_scan_above[REC_SCAN_BINS];
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
void rec_watch_scan_preview_threshold(float db)
{
    if(db<REC_SCAN_DETECT_MIN)db=REC_SCAN_DETECT_MIN;
    if(db>REC_SCAN_DETECT_MAX)db=REC_SCAN_DETECT_MAX;
    s_gate_db=db;
}
void rec_watch_scan_set_threshold(float db)
{
    rec_watch_scan_preview_threshold(db);
    settings_set_subghz_gate_db((int)(s_gate_db+0.5f));
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
    if(!out || max<=0)return s_scan_n;
    int n=s_scan_live_n<max?s_scan_live_n:max;
    for(int i=0;i<n;i++)out[i]=s_scan_live[i];
    return n;
}
float rec_watch_scan_live_floor(void){ return s_scan_live_floor; }
float rec_watch_scan_live_spread(void){ return s_scan_live_spread; }
int  rec_watch_scan_hits(void){ return s_scan_hits; }
int  rec_watch_scan_events(void){ return s_scan_events; }
bool rec_watch_scan_catch(uint32_t hz)
{
    if(!hz || !s_scan_busy)return false;
    if(!rec_set_freq(hz))return false;
    portENTER_CRITICAL(&s_lock);
    bool ok=s_scan_busy && !s_scan_stop_req;
    if(ok){s_last_hit=hz;s_catch_armed=true;s_scan_stop_req=true;}
    portEXIT_CRITICAL(&s_lock);
    return ok;
}
void rec_watch_scan_stop(void)
{
    portENTER_CRITICAL(&s_lock);
    s_scan_stop_req=true;s_catch_armed=false;
    portEXIT_CRITICAL(&s_lock);
}
static EXT_RAM_BSS_ATTR char s_scan_stage[40];
int rec_watch_scan_progress(void){ return s_scan_pct; }
const char *rec_watch_scan_stage(void){ return s_scan_stage; }
static void stage(const char *what,int pct)
{
    snprintf(s_scan_stage,sizeof(s_scan_stage),"%s",what?what:"");
    s_scan_pct=pct<0?0:pct>100?100:pct;
}
static void scan_band(uint32_t min_hz,uint32_t max_hz,uint32_t secs,int nb,char *result,size_t len);
static uint32_t s_learn_secs,s_learn_hz;
static volatile bool s_learn_busy;
static bool s_learn_rewatch;   /* WATCH back on when the running LEARN ends */
static EXT_RAM_BSS_ATTR rec_learn_t s_learn;
static void learn_run(uint32_t hz,uint32_t secs,char *result,size_t len);
static void learn_job(uint32_t hz,uint32_t secs);
static bool s_pin_value, s_starting;
static uint32_t s_boot;
static rec_source_t s_source;
/* One per source, sized off the enum. These were [2] and indexed by a
   source id, so a third receiver filing anything at all wrote past them. */
static uint32_t source_received[REC_SOURCE_COUNT],source_dropped[REC_SOURCE_COUNT];
static const char *DIR="/sdcard/subghz";
#define REC_WATCH_STACK_WORDS (6144 / sizeof(StackType_t))
#define WORKER_PRIORITY 1
#define ARCHIVE_IO_PRIORITY 5
static EXT_RAM_BSS_ATTR StackType_t s_worker_stack[REC_WATCH_STACK_WORDS];
static StaticTask_t s_worker_tcb;

/* The mesh owns the SX1262 between sessions, so a watch on it has to ask.

   Declared rather than included: ls_mesh.h belongs to the meshcore component,
   which depends on this one, and requiring it back is a dependency cycle.
   Weak, so a build without a mesh links and reports the radio as free - which
   on such a build it is. */
__attribute__((weak)) bool ls_mesh_radio_hold(bool hold){(void)hold;return true;}
__attribute__((weak)) bool ls_mesh_radio_held(void){return true;}

__attribute__((weak)) bool rec_watch_notify(const char *peer, const char *text)
{ (void)peer; (void)text; return false; }

/* rtl_433's decoders over every filed capture (rec_ism), whichever radio
   heard it. The label is kept per catalog slot by event id, so a slot reused
   for another pattern loses it. */
static EXT_RAM_BSS_ATTR rec_ism_work_t s_ism_work;
static EXT_RAM_BSS_ATTR lr433_msg_t s_ism_msgs[4];
static EXT_RAM_BSS_ATTR struct { uint32_t id; char text[40]; } s_ism_label[REC_WATCH_SLOTS];

static void ism_decode(const capture_t *cap, int slot)
{
    /* A demodulating source says how it heard the capture. One that times
       edges does not: REC's slicer may have been on the amplitude or the
       frequency, so both families get a look. */
    int k;
    if (cap->has_mod) {
        k = rec_ism_decode_edges(cap->pulse, cap->edges, cap->mod.deviation_hz != 0, &s_ism_work, s_ism_msgs, 4);
    } else {
        k = rec_ism_decode_edges(cap->pulse, cap->edges, false, &s_ism_work, s_ism_msgs, 4);
        k += rec_ism_decode_edges(cap->pulse, cap->edges, true, &s_ism_work, s_ism_msgs + k, 4 - k);
    }
    const int64_t now = esp_timer_get_time();
    for (int i = 0; i < k; i++) {
        const rec_ism_msg_t m = { .m = s_ism_msgs[i], .hz = cap->hz, .rssi = NAN, .source = (uint8_t)cap->source };
        rec_ism_publish(&m, now);
    }
    if (k > 0 && slot >= 0 && slot < REC_WATCH_SLOTS) {
        s_ism_label[slot].id = s_catalog->record[slot].event.id;
        rec_ism_describe(&s_ism_msgs[0], s_ism_label[slot].text, sizeof(s_ism_label[slot].text));
    }
}

static void publish(void)
{
    rec_ook24_t decoded[REC_WATCH_SLOTS] = {0};
    static subghz_pwm_t pwm[REC_WATCH_SLOTS];
    static subghz_nrz_t nrz[REC_WATCH_SLOTS];
    memset(pwm, 0, sizeof(pwm));
    memset(nrz, 0, sizeof(nrz));
    for(int i=0;i<REC_WATCH_SLOTS;i++) if(s_catalog->record[i].event.id) {
        rec_decode_ook24(s_catalog->record[i].pulse, s_catalog->record[i].event.edges, &decoded[i]);
        subghz_pwm_decode(s_catalog->record[i].pulse, s_catalog->record[i].event.edges, &pwm[i]);
        if (!pwm[i].repeats)
            subghz_nrz_decode(s_catalog->record[i].pulse, s_catalog->record[i].event.edges, &nrz[i]);
    }
    portENTER_CRITICAL(&s_lock);
    s_status.count=0;
    s_status.boot_id=s_boot;
    for(int i=0;i<REC_WATCH_SLOTS;i++) if(s_catalog->record[i].event.id) {
        int n=s_status.count++;
        s_status.event[n]=s_catalog->record[i].event;
        memcpy(s_status.preview[n],s_catalog->record[i].pulse,sizeof(s_status.preview[n]));
        s_status.decoded[n] = decoded[i];
        s_status.pwm[n] = pwm[i];
        s_status.nrz[n] = nrz[i];
        if (s_ism_label[i].id && s_ism_label[i].id == s_catalog->record[i].event.id)
            memcpy(s_status.ism[n], s_ism_label[i].text, sizeof(s_status.ism[n]));
        else s_status.ism[n][0] = 0;
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
    if(slot>=0 && novel) {
        /* Something new on the air: onto the decoded timeline. The
           capture's own clock (ms) is when it was heard. */
        ls_decoded_t ev={.kind=LS_DECODED_REC,.at_us=(int64_t)cap->ms*1000,.freq_hz=cap->hz,
            .radio=cap->source==REC_SOURCE_SX1262 && ls_lora_chip()==LS_LORA_CHIP_LR20XX?LS_TM_RADIO_LR2021:
                   cap->source==REC_SOURCE_RTL?LS_TM_RADIO_RTL:LS_TM_RADIO_NONE};
        snprintf(ev.from,sizeof(ev.from),"%s",rec_source_name(cap->source));
        rec_ook24_t code;
        if(rec_decode_ook24(cap->pulse,cap->edges,&code))
            LS_SNPRINTF(t_rec_watch_runtime_ook24_06lx_u_frames, ev.text,sizeof(ev.text),"OOK24 %06lX, %u frames",(unsigned long)code.value,code.repeats);
        else snprintf(ev.text,sizeof(ev.text),"%s, %d edges",cap->has_mod?"FSK burst":"burst",cap->edges);
        ls_decoded_publish(&ev);
    }
    if(slot>=0 && ls_seen_signal) {  /* a detector app: frequency, pattern, new or repeat */
        rec_ook24_t ook;
        ls_det_signal_t sig={.source=DET_REC,.a=cap->hz,.b=s_catalog->record[slot].event.id,.flag=novel};
        if(rec_decode_ook24(cap->pulse,cap->edges,&ook))snprintf(sig.text,sizeof(sig.text),"OOK24");
        else snprintf(sig.text,sizeof(sig.text),"%s",rec_source_name(cap->source));
        ls_seen_signal(&sig);
    }
    ism_decode(cap,slot);
    rec_ook24_t qualified;
    bool confirmed=slot>=0 && ((novel && rec_decode_ook24(cap->pulse,cap->edges,&qualified)) || s_catalog->record[slot].event.count==2);
    if(confirmed && enabled && peer[0]) {
        if((!flow->attempted_alert || now-flow->last_alert>=60000) && now-cap->ms<5000) {
            char text[80];
            rec_ook24_t decoded;
            const char *source=rec_source_name(cap->source);
            if(rec_decode_ook24(cap->pulse,cap->edges,&decoded))
                LS_SNPRINTF(t_rec_watch_runtime_s_ook24_06lx_4fmhz, text,sizeof(text),"%s OOK24 %06lX %.4fMHz (%u matching frames)",
                    source,(unsigned long)decoded.value,cap->hz/1e6,decoded.repeats);
            else LS_SNPRINTF(t_rec_watch_runtime_s_raw_lu_4fmhz, text,sizeof(text),"%s RAW #%lu %.4fMHz %d edges",source,
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
static void sync_cc_watch(void);
#include "rec_lr_iq_runtime.inc"

static void worker(void *arg)
{
    (void)arg;
    s_worker_task=xTaskGetCurrentTaskHandle();
    /* The archive is read at ARCHIVE_IO_PRIORITY, not the worker's 1. Just
       after boot other tasks are reading the card, and FAT hands its lock
       to the highest waiter: at priority 1 the restore took 64-72 s on the
       T-Display-P4, a second a read, and WATCH could not start until it
       was done. At 5 it takes about 4 s. It blocks on the card nearly all
       of that time; its own work (copy and CRC) is about 60 ms, and it
       gives the core up for a tick between reads (rec_watch_store.c). Only
       the restore: checkpoints stay at the worker's priority. */
    const int64_t t0=esp_timer_get_time();
    vTaskPrioritySet(NULL,ARCHIVE_IO_PRIORITY);
    bool restored=rec_watch_restore(DIR,s_catalog);
    vTaskPrioritySet(NULL,WORKER_PRIORITY);
    const int64_t t1=esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    snprintf(s_status.storage,sizeof(s_status.storage),"%s",restored?"SD archive restored":"RAM; awaiting SD checkpoint");
    portEXIT_CRITICAL(&s_lock);
    s_status.saved=restored;
    publish();
    ESP_LOGI("subghz_io","archive %s in %lld ms",restored?"restored":"not restored",(long long)(t1-t0)/1000);
    uint64_t saved=s_catalog->sequence, checkpoint=0;
    capture_flow_t flow={0};
    bool was_enabled=false;
    for(;;) {
        portENTER_CRITICAL(&s_lock);
        bool watch_pending=s_watch_pending,watch_on=s_watch_on;
        if(watch_pending)s_watch_taken=true;
        portEXIT_CRITICAL(&s_lock);
        if(watch_pending) {
            bool ok=rec_watch_enable_owned(watch_on);
            portENTER_CRITICAL(&s_lock);
            s_watch_result=ok;s_watch_pending=false;s_watch_taken=false;
            portEXIT_CRITICAL(&s_lock);
        }
        sync_cc_watch();
        capture_t *cap=NULL;
        if(xQueueReceive(s_pending,&cap,pdMS_TO_TICKS(100))==pdTRUE) {
            consume_capture(cap,&flow);
        }
        portENTER_CRITICAL(&s_lock);
        bool iq_pending=s_iq_pending;s_iq_pending=false;
        uint32_t iq_hz=s_iq_hz;
        uint32_t pin=s_pin_id;bool value=s_pin_value;s_pin_id=0;
        uint32_t export_id=s_export_id;s_export_id=0;
        uint32_t replay_id=s_replay_id;int replay_dbm=s_replay_dbm;s_replay_id=0;
        char replay_path[sizeof(s_replay_path)];
        memcpy(replay_path,s_replay_path,sizeof(replay_path));s_replay_path[0]=0;
        s_replay_busy=replay_id || replay_path[0];
        uint32_t scan_min=s_scan_min,scan_max=s_scan_max,scan_secs=s_scan_secs;s_scan_min=0;
        int scan_bins=s_scan_bins;
        uint32_t learn_secs=s_learn_secs,learn_hz=s_learn_hz;s_learn_secs=0;
        bool enabled=s_status.enabled;
        portEXIT_CRITICAL(&s_lock);
        if(iq_pending) {
            char result[sizeof(s_iq_result)];
            if(s_iq_mode)iq_diagnostic(result,sizeof(result));
            else iq_record(iq_hz,result,sizeof(result));
            portENTER_CRITICAL(&s_lock);
            snprintf(s_iq_result,sizeof(s_iq_result),"%s",result);s_iq_busy=false;
            portEXIT_CRITICAL(&s_lock);
        }
        if(pin) { rec_watch_pin(s_catalog,pin,value);portENTER_CRITICAL(&s_lock);s_status.pending_save=true;portEXIT_CRITICAL(&s_lock);publish(); }
        if(learn_secs){learn_job(learn_hz,learn_secs);publish();}
        if(scan_min) {
            char result[112];
            scan_band(scan_min,scan_max,scan_secs,scan_bins,result,sizeof(result));
            /* The sweep has released its lease before WATCH takes it. Keep
               admission closed until the handoff and its cancellation finish. */
            bool caught=false;
            if(s_catch_armed && rec_watch_source()==s_scan_source) {
                caught=rec_watch_enable_owned(true);
                snprintf(result,sizeof(result),caught?
                    "Caught %.4f MHz - receiver started":
                    "Found %.4f MHz - receiver would not start",s_last_hit/1e6);
            }
            portENTER_CRITICAL(&s_lock);
            bool cancelled=s_scan_stop_req && !s_catch_armed;
            if(caught && cancelled) {
                portEXIT_CRITICAL(&s_lock);
                rec_watch_enable_owned(false);
                portENTER_CRITICAL(&s_lock);
            }
            if(cancelled)LS_SNPRINTF(t_rec_watch_runtime_scan_stopped_d_found, result,sizeof(result),"Scan stopped: %d found",s_scan_n);
            snprintf(s_status.export_status,sizeof(s_status.export_status),"%s",result);
            snprintf(s_status.scan_result,sizeof(s_status.scan_result),"%s",result);
            s_catch_armed=false;s_scan_busy=false;s_status.scan_generation++;
            portEXIT_CRITICAL(&s_lock);
            publish();
        }
        if(replay_path[0]) {
            char result[112];
            replay_file(replay_path,replay_dbm,result,sizeof(result));
            portENTER_CRITICAL(&s_lock);
            snprintf(s_status.export_status,sizeof(s_status.export_status),"%s",result);
            snprintf(s_replay_result,sizeof(s_replay_result),"%s",result);
            portEXIT_CRITICAL(&s_lock);
        }
        if(replay_id) {
            char result[112];
            replay(replay_id,replay_dbm,result,sizeof(result));
            portENTER_CRITICAL(&s_lock);
            snprintf(s_status.export_status,sizeof(s_status.export_status),"%s",result);
            snprintf(s_replay_result,sizeof(s_replay_result),"%s",result);
            portEXIT_CRITICAL(&s_lock);
            publish();
        }
        portENTER_CRITICAL(&s_lock);s_replay_busy=false;portEXIT_CRITICAL(&s_lock);
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
                /* At the worker's own priority: a checkpoint is background
                   work every 30 s, and at ARCHIVE_IO_PRIORITY it kept core 0
                   busy enough that its idle task missed the 5 s watchdog
                   (68 times in an hour of WATCH on the LR2021). */
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
    /* Read every setting the worker uses here, on the caller's stack. The
       worker's stack is in PSRAM, and an NVS read is a flash read, which
       disables the cache - a task whose stack lives behind that cache
       asserts in spi_flash_disable_interrupts_caches_and_other_cpu. */
    (void)rec_watch_scan_threshold();
    (void)rec_watch_scan_on_hit_get();
    rec_watch_fsk_get(NULL);
    /* The receiver chosen last, kept across a reboot (radio_choice.h). */
    const rec_source_t saved=rec_source_of_radio(ls_rsel_saved(LS_RSEL_SUBGHZ_READ));
    if(saved<REC_SOURCE_COUNT) {
        portENTER_CRITICAL(&s_lock);
        if(!s_status.enabled) s_source=saved;
        portEXIT_CRITICAL(&s_lock);
        if(saved==REC_SOURCE_CC1101) ls_mixrf_start();
    }
    s_catalog=heap_caps_calloc(1,sizeof(*s_catalog),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    s_checkpoint=heap_caps_malloc(sizeof(*s_checkpoint),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    capture_t *pool=heap_caps_calloc(2,sizeof(*pool),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    s_free=xQueueCreate(2,sizeof(capture_t*));s_pending=xQueueCreate(2,sizeof(capture_t*));
    bool ok=s_catalog && s_checkpoint && pool && s_free && s_pending;
    if(ok) {
        for(int i=0;i<2;i++) {capture_t *p=&pool[i];xQueueSend(s_free,&p,0);}
        s_boot=esp_random();
        ok=xTaskCreateStaticPinnedToCore(worker,"subghz_io",
            REC_WATCH_STACK_WORDS,NULL,WORKER_PRIORITY,s_worker_stack,&s_worker_tcb,0)!=NULL;
    }
    if(!ok) {
        if(s_free)vQueueDelete(s_free);
        if(s_pending)vQueueDelete(s_pending);
        s_free=s_pending=NULL;heap_caps_free(pool);heap_caps_free(s_catalog);s_catalog=NULL;
        heap_caps_free(s_checkpoint);s_checkpoint=NULL;
        portENTER_CRITICAL(&s_lock);
        s_starting=false;
        LS_SNPUTS(t_rec_watch_runtime_monitor_allocation_failed, s_status.storage, sizeof(s_status.storage), "Monitor allocation failed");
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
    if(!in || rec_watch_enabled() || s_watch_waiting || s_scan_busy || s_learn_busy)return false;
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
static StaticTask_t s_fsk_tcb;
static volatile bool s_fsk_run,s_fsk_exited;
static TaskHandle_t s_fsk_task;

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
    s_fsk_exited=true;
    vTaskSuspend(NULL);
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
        while((!s_scan_busy || s_catch_armed) && !ls_mesh_radio_hold(true) && esp_timer_get_time()<deadline)
            vTaskDelay(pdMS_TO_TICKS(10));
        if((s_scan_busy && !s_catch_armed) || !ls_mesh_radio_held()){
            ls_mesh_radio_hold(false);
            LS_SNPUTS(t_rec_watch_runtime_mesh_would_not_release, s_fsk_error, sizeof(s_fsk_error), "Mesh would not release the radio");
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
        /* The LR2021 with its preamble detector off misses the sync of a
           clean packet: an SX1262 frame its I/Q path decoded whole counted
           only as sync failures. 16 bits, as FIELD and RadioLib use. */
        if(ls_lora_caps() & LS_LORA_CAP_FSK_DETECT)
            cfg.preamble_detect_bits=cfg.preamble_bits>=16?16:8;
        if(s_scan_busy && !s_catch_armed) {
            ls_mesh_radio_hold(false);return false;
        }
        const esp_err_t err=ls_lora_fsk_begin(&cfg);
        if(err!=ESP_OK){
            ls_mesh_radio_hold(false);
            snprintf(s_fsk_error,sizeof(s_fsk_error),"%s",
                err==ESP_ERR_INVALID_STATE?"LoRa session busy; stop its receiver first":
                err==ESP_ERR_NO_MEM?"No memory for FSK; retry later":
                "FSK settings refused; check rate and deviation");
            return false;
        }
        if(s_scan_busy && !s_catch_armed) {
            ls_lora_fsk_end();ls_mesh_radio_hold(false);return false;
        }
        s_fsk_exited=false;s_fsk_run=true;
        s_fsk_task=xTaskCreateStaticPinnedToCore(fsk_poller,"subghz_fsk",
                REC_FSK_STACK_WORDS,NULL,4,s_fsk_stack,&s_fsk_tcb,0);
        if(!s_fsk_task) {
            s_fsk_run=false;
            ls_lora_fsk_end();
            ls_mesh_radio_hold(false);
            LS_SNPUTS(t_rec_watch_runtime_no_memory_for_the, s_fsk_error, sizeof(s_fsk_error), "No memory for the receive task");
            return false;
        }
        return true;
    }
    if(!s_fsk_run)return true;
    s_fsk_run=false;
    /* Let the poller see the flag and leave before the radio moves under it. */
    while(!s_fsk_exited || eTaskGetState(s_fsk_task)!=eSuspended)vTaskDelay(pdMS_TO_TICKS(1));
    vTaskDelete(s_fsk_task);s_fsk_task=NULL;
    ls_lora_fsk_end();
    ls_mesh_radio_hold(false);
    return true;
}


/* READ on the LR2021: its raw I/Q stream through an OOK slicer, so it hears
   any on/off keyed remote the way the SDRs do. The FSK packet receiver
   above stays for a signal LEARN has measured (rec_watch_lr_fsk). */
static EXT_RAM_BSS_ATTR rec_lr_ook_t s_lr_ook;
static bool s_lr_run;
static bool s_lr_fsk, s_lr_fsk_loaded;
/* Kept across a restart, like the rest of the capture setup. */
bool rec_watch_lr_fsk(void)
{
    if(!s_lr_fsk_loaded){s_lr_fsk=settings_get_subghz_lr_fsk();s_lr_fsk_loaded=true;}
    return s_lr_fsk;
}
bool rec_watch_set_lr_fsk(bool fsk)
{
    if(rec_watch_enabled())return false;
    s_lr_fsk=fsk;s_lr_fsk_loaded=true;
    settings_set_subghz_lr_fsk(fsk);
    return true;
}
static void lr_ook_emit(void *ctx,const int32_t *edge,int edges,int peak)
{
    (void)ctx;
    if(edges>REC_WATCH_EDGES)edges=REC_WATCH_EDGES;
    /* The stream is 16-bit; the archive's peak is the RTL's 8-bit scale. */
    peak/=16;if(peak>255)peak=255;
    rec_watch_submit_mod(REC_SOURCE_SX1262,rec_get_freq(),edge,edges,peak,REC_END_GAP,NULL);
}
static void lr_ook_consume(void *ctx,const uint8_t *data,size_t bytes,bool spare)
{
    (void)ctx;(void)spare;
    if(!data){rec_lr_ook_reset(&s_lr_ook);return;}
    rec_lr_ook_feed(&s_lr_ook,data,bytes,lr_ook_emit,NULL);
}
static bool lr_ook_wanted(void)
{
    const uint32_t hz=rec_get_freq();
    return !rec_watch_lr_fsk() && ls_lora_chip()==LS_LORA_CHIP_LR20XX && ls_lora_rx_range_ok(ls_lora_caps(),hz,hz);
}
static bool lr_ook_session(bool on)
{
    if(on) {
        if(s_lr_run)return true;
        s_fsk_error[0]=0;
        const int64_t deadline=esp_timer_get_time()+1000000;
        while((!s_scan_busy || s_catch_armed) && !ls_mesh_radio_hold(true) && esp_timer_get_time()<deadline)
            vTaskDelay(pdMS_TO_TICKS(10));
        if((s_scan_busy && !s_catch_armed) || !ls_mesh_radio_held()) {
            ls_mesh_radio_hold(false);
            LS_SNPUTS(t_rec_watch_runtime_mesh_would_not_release, s_fsk_error, sizeof(s_fsk_error), "Mesh would not release the radio");
            return false;
        }
        const rec_lr_ook_cfg_t cfg={.min_pulse_us=60,.gap_us=30000,.min_edges=6};
        rec_lr_ook_init(&s_lr_ook,&cfg);
        if(lr_wf_start_consumer(rec_get_freq(),lr_ook_consume,NULL)!=ESP_OK) {
            ls_mesh_radio_hold(false);
            LS_SNPUTS(t_rec_watch_runtime_lr2021_stream_busy_close, s_fsk_error, sizeof(s_fsk_error), "LR2021 stream busy; close its waterfall first");
            return false;
        }
        s_lr_run=true;
        return true;
    }
    if(!s_lr_run)return true;
    lr_wf_stop();
    s_lr_run=false;
    ls_mesh_radio_hold(false);
    return true;
}
/* FSK on the LR2021, from its I/Q stream.

   The chip's GFSK packet engine counted every frame an SX1262 sent it as a
   preamble then a sync failure, with sync, rate, deviation and filter set as
   RadioLib sets them, while the same frame recorded as I/Q decoded whole
   (offset 0.5 kHz, deviation 24.7 kHz). So FSK WATCH listens the way OOK
   WATCH does: bursts are gated out of the 125 kS/s stream, decoded in
   software (fsk_packet.c, as REC's I/Q records are), and a packet whose sync
   matches the capture setup is submitted exactly as the packet engine's are,
   so SEND replays it under the same settings. The engine stays for what
   125 kS/s cannot hold. */
/* 1 s: a 64-byte frame with its preamble and sync at 600 bd. 320 ms held
   2400 bd and up, and a 1200 bd frame - learned in one burst at 4800 bd on
   the same pair of boards - was cut short and never decoded. */
#define LRF_PAIRS    125000u
#define LRF_PRE      256u             /* 2 ms before the gate opened                    */
#define LRF_QUIET    625u             /* 5 ms under the gate ends the burst             */
#define LRF_MIN      250u             /* a burst shorter than 2 ms is not a packet      */
#define LRF_EDGES    4096u
static EXT_RAM_BSS_ATTR int16_t s_lrf_buf[2][LRF_PAIRS*2];
static EXT_RAM_BSS_ATTR int16_t s_lrf_pre[LRF_PRE*2];
static EXT_RAM_BSS_ATTR int32_t s_lrf_edges[LRF_EDGES];
static EXT_RAM_BSS_ATTR fsk_pkt_t s_lrf_pkts[4];
static EXT_RAM_BSS_ATTR uint8_t s_lrf_bits[LS_FSK_CAPTURE_BYTES*8+1024+32];
static EXT_RAM_BSS_ATTR int32_t s_lrf_raw[LS_FSK_CAPTURE_BYTES*8+1024+32];
static size_t s_lrf_len[2];
static int32_t s_lrf_peakv[2];
static int32_t s_lrf_airv[2];         /* the gate's floor before the burst              */
static bool s_lrf_tailed[2];          /* the burst ended in LRF_QUIET of quiet          */
static atomic_int s_lrf_full[2];
static struct {
    int32_t floor_acc, floor, peak;
    int32_t air;        /* the floor before the sample that opened the gate */
    uint32_t warm, quiet, pre_at, pre_n;
    size_t n;
    int buf;
    bool on;
} s_lrf;
static bool s_lrf_run;
static TaskHandle_t s_lrf_task;
/* LEARN on the LR2021: a packet heard whole, instead of a capture. One that
   proves itself is taken from the one burst: a CRC that checks, or a radio
   chip's default sync word. Anything else is only a preamble-like run and
   the sixteen bits after it, which a keyed-up FM voice set or a tone gives
   as readily as a remote, so it waits for a second burst with the same word
   at the same rate and deviation. */
#define LRF_CAND 4
static volatile bool s_lrf_learning, s_lrf_learned;
static fsk_pkt_t s_lrf_learn_pkt;
static float s_lrf_learn_dev;
/* Each candidate keeps the capture it came from: one capture of a remote
   repeating its frame is still one burst. */
static EXT_RAM_BSS_ATTR struct { fsk_pkt_t pkt; float dev; uint32_t capture; } s_lrf_cand[LRF_CAND];
static uint32_t s_lrf_capture;
static volatile uint32_t s_lrf_cands, s_lrf_cand_baud;   /* unproven bursts, and the last one's rate */
static uint8_t s_lrf_learn_bursts;
#define LRF_STACK_WORDS (6144 / sizeof(StackType_t))
static EXT_RAM_BSS_ATTR StackType_t s_lrf_stack[LRF_STACK_WORDS];
static StaticTask_t s_lrf_tcb;

/* What 125 kS/s can carry: both tones and a bit inside +/-62.5 kHz. */
static bool lr_fsk_iq_fits(const rec_fsk_mod_t *m)
{
    return m->bitrate<=50000u && m->deviation_hz+m->bitrate/2u<=55000u;
}
/* A decoded sync, sent once or twice, against the top bits of the setup's. */
static bool lr_fsk_sync_ok(const fsk_pkt_t *p,uint32_t want)
{
    const unsigned bits=p->sync_bits;
    if(!bits || bits>32)return false;
    uint64_t got=p->sync;
    unsigned have=bits;
    for(unsigned r=1;r<p->sync_repeats && have+bits<=32;r++){got=(got<<bits)|p->sync;have+=bits;}
    if(have<16)return false;
    return (uint32_t)got==(have==32?want:want>>(32-have));
}
static bool lr_learn_proven(const fsk_pkt_t *p)
{
    return p->sync_known || p->sync_repeats>=2 || p->crc_kind!=FSK_CRC_NONE;
}
/* A word that can be a sync word: all of it and a byte after it were on the
   air, not the padding past the end of the burst, and it is neither one
   level held nor the preamble carrying on. */
static bool lr_learn_word_ok(const fsk_pkt_t *p)
{
    if(p->sync_bits!=16 || p->len<4)return false;
    const int turns=__builtin_popcount((p->sync^(p->sync>>1))&0x7FFFu);
    return turns>=2 && turns<=13;
}
static bool lr_learn_same(const fsk_pkt_t *a,float adev,const fsk_pkt_t *b,float bdev)
{
    if(a->sync!=b->sync || a->inverted!=b->inverted)return false;
    const uint32_t lo=a->baud<b->baud?a->baud:b->baud,hi=a->baud<b->baud?b->baud:a->baud;
    if((hi-lo)*100u>hi*3u)return false;
    if(adev<600.0f || bdev<600.0f)return true;
    const float big=adev>bdev?adev:bdev;
    return fabsf(adev-bdev)<=0.25f*big;
}
/* One decoded burst to LEARN. */
static void lr_learn_offer(const fsk_pkt_t *p,float dev)
{
    if(s_lrf_learned || !p->len || p->baud<600 || p->baud>100000 || !p->sync_bits)return;
    uint8_t bursts=1;
    if(!lr_learn_proven(p)) {
        if(!lr_learn_word_ok(p))return;
        const uint32_t have=s_lrf_cands<LRF_CAND?s_lrf_cands:LRF_CAND;
        bool agreed=false;
        for(uint32_t i=0;i<have && !agreed;i++)
            agreed=s_lrf_cand[i].capture!=s_lrf_capture &&
                   lr_learn_same(&s_lrf_cand[i].pkt,s_lrf_cand[i].dev,p,dev);
        if(!agreed) {
            s_lrf_cand[s_lrf_cands%LRF_CAND].pkt=*p;
            s_lrf_cand[s_lrf_cands%LRF_CAND].dev=dev;
            s_lrf_cand[s_lrf_cands%LRF_CAND].capture=s_lrf_capture;
            s_lrf_cand_baud=p->baud_std?p->baud_std:p->baud;
            s_lrf_cands++;
            return;
        }
        bursts=2;
    }
    s_lrf_learn_pkt=*p;
    s_lrf_learn_dev=dev;
    s_lrf_learn_bursts=bursts;
    s_lrf_learned=true;
}
/* The decoder takes its floor from the capture itself, and the gate leaves
   2 ms before a burst and 5 ms after: over 28 ms of burst that floor is the
   burst. So the gate's own measurements go with the capture. Its floor is
   the air before the burst (|I|+|Q|; three quarters of its square is the
   power). The quiet that shut it is the air after, lower when the chip
   turned its gain down for the burst. */
static void lr_fsk_decode(const int16_t *iq,size_t pairs,int peak,int32_t air,bool tailed)
{
    fsk_pkt_rf_t rf={0};
    const float before=0.75f*(float)air*(float)air;
    float after=0;
    if(tailed && pairs>LRF_QUIET) {
        double acc=0;
        for(size_t k=pairs-LRF_QUIET;k<pairs;k++)
            acc+=(double)iq[2*k]*iq[2*k]+(double)iq[2*k+1]*iq[2*k+1];
        after=(float)(acc/(double)LRF_QUIET);
    }
    const size_t ne=fsk_pkt_iq16_gated_edges(iq,pairs,REC_LR_IQ_RATE,before,after,s_lrf_edges,LRF_EDGES,&rf);
    const int found=ne?fsk_pkt_from_edges(s_lrf_edges,ne,s_lrf_pkts,4):0;
    if(s_lrf_learning) {
        s_lrf_capture++;
        for(int i=0;i<found;i++)lr_learn_offer(&s_lrf_pkts[i],rf.deviation_hz);
        return;
    }
    const rec_fsk_mod_t m=s_fsk;
    for(int i=0;i<found;i++) {
        const fsk_pkt_t *p=&s_lrf_pkts[i];
        if(!p->len || !lr_fsk_sync_ok(p,m.sync_word))continue;
        /* The rate and deviation this packet was MEASURED at, not the capture
           setup's: the I/Q path hears any rate whose sync matches, and a
           1200 bd frame stored as 4800 bd would be replayed four times too
           fast. The setup's values stand in only where a measurement is
           missing. */
        rec_fsk_mod_t got=m;
        const uint32_t baud=p->baud_std?p->baud_std:p->baud;
        if(baud>=600 && baud<=100000)got.bitrate=baud;
        if(rf.deviation_hz>=600.0f && rf.deviation_hz<=200000.0f)got.deviation_hz=(uint32_t)(rf.deviation_hz+0.5f);
        ls_fsk_capture_t c;
        memset(&c,0,sizeof(c));
        c.freq_hz=rec_get_freq();
        c.bitrate=got.bitrate; c.deviation_hz=got.deviation_hz;
        c.bandwidth_hz=(uint32_t)m.bandwidth_khz*1000u;
        c.sync_word=m.sync_word; c.preamble_bits=m.preamble_bits;
        c.len=(uint8_t)(p->len>LS_FSK_CAPTURE_BYTES?LS_FSK_CAPTURE_BYTES:p->len);
        memcpy(c.data,p->data,c.len);
        const size_t nb=ls_fsk_bitstream(&c,s_lrf_bits,sizeof(s_lrf_bits));
        const size_t nr=nb?ls_fsk_raw_data(s_lrf_bits,nb,c.bitrate,s_lrf_raw,
                            sizeof(s_lrf_raw)/sizeof(s_lrf_raw[0])):0;
        if(nr>=6 && nr<=REC_WATCH_EDGES)
            rec_watch_submit_mod(REC_SOURCE_SX1262,c.freq_hz,s_lrf_raw,(int)nr,
                                 peak,REC_END_GAP,&got);
    }
}
static void lr_fsk_worker(void *arg)
{
    (void)arg;
    for(;;) {
        (void)ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
        for(int b=0;b<2;b++)
            if(atomic_load(&s_lrf_full[b])) {
                lr_fsk_decode(s_lrf_buf[b],s_lrf_len[b],s_lrf_peakv[b],s_lrf_airv[b],s_lrf_tailed[b]);
                atomic_store(&s_lrf_full[b],0);
            }
    }
}
static void lr_fsk_reset(void)
{
    s_lrf.on=false;s_lrf.n=0;s_lrf.quiet=0;s_lrf.pre_at=s_lrf.pre_n=0;s_lrf.peak=0;
}
static void lr_fsk_close(void)
{
    const int b=s_lrf.buf;
    const bool cut=s_lrf.n>=LRF_PAIRS;
    if(s_lrf.n>=LRF_MIN) {
        s_lrf_len[b]=s_lrf.n;
        s_lrf_peakv[b]=s_lrf.peak/16>255?255:s_lrf.peak/16;
        s_lrf_airv[b]=s_lrf.air;
        s_lrf_tailed[b]=!cut;
        atomic_store(&s_lrf_full[b],1);
        if(s_lrf_task)xTaskNotifyGive(s_lrf_task);
        s_lrf.buf=b^1;
    }
    lr_fsk_reset();
    /* Still on the air when the buffer ran out: a carrier. The floor only
       follows the air while the gate is shut, so the next sample would open
       it again and the decoder would get the same carrier a second at a
       time for as long as it lasted. The floor relearns on it instead, as
       OOK WATCH does with a station: the gate stays shut until the carrier
       drops or something stands over it. */
    if(cut)s_lrf.warm=0;
}
static void lr_fsk_consume(void *ctx,const uint8_t *data,size_t bytes,bool spare)
{
    (void)ctx;(void)spare;
    if(!data){lr_fsk_reset();return;}
    for(size_t i=0;i+3<bytes;i+=4) {
        const int16_t si=(int16_t)(data[i]|(data[i+1]<<8));
        const int16_t sq=(int16_t)(data[i+2]|(data[i+3]<<8));
        const int32_t mag=(si<0?-(int32_t)si:si)+(sq<0?-(int32_t)sq:sq);
        if(!s_lrf.on) {
            const int32_t air=s_lrf.floor;
            s_lrf.floor_acc+=mag-(s_lrf.floor_acc>>6);
            s_lrf.floor=s_lrf.floor_acc>>6;
            if(s_lrf.floor<8)s_lrf.floor=8;
            s_lrf_pre[2*s_lrf.pre_at]=si;s_lrf_pre[2*s_lrf.pre_at+1]=sq;
            s_lrf.pre_at=(s_lrf.pre_at+1)%LRF_PRE;
            if(s_lrf.pre_n<LRF_PRE)s_lrf.pre_n++;
            if(s_lrf.warm<256){s_lrf.warm++;continue;}
            /* Both buffers waiting on the decoder: this burst is lost. */
            if(mag<=s_lrf.floor*4 || atomic_load(&s_lrf_full[s_lrf.buf]))continue;
            s_lrf.air=air;
            s_lrf.on=true;s_lrf.quiet=0;s_lrf.peak=mag;
            int16_t *out=s_lrf_buf[s_lrf.buf];
            for(uint32_t k=0;k<s_lrf.pre_n;k++) {
                const uint32_t at=(s_lrf.pre_at+LRF_PRE-s_lrf.pre_n+k)%LRF_PRE;
                out[2*k]=s_lrf_pre[2*at];out[2*k+1]=s_lrf_pre[2*at+1];
            }
            s_lrf.n=s_lrf.pre_n;
            continue;
        }
        int16_t *out=s_lrf_buf[s_lrf.buf];
        out[2*s_lrf.n]=si;out[2*s_lrf.n+1]=sq;s_lrf.n++;
        if(mag>s_lrf.peak)s_lrf.peak=mag;
        s_lrf.quiet=mag<s_lrf.floor*2?s_lrf.quiet+1:0;
        if(s_lrf.quiet>=LRF_QUIET || s_lrf.n>=LRF_PAIRS)lr_fsk_close();
    }
}
static bool lr_fsk_iq_wanted(void)
{
    const uint32_t hz=rec_get_freq();
    rec_fsk_mod_t m;rec_watch_fsk_get(&m);
    return rec_watch_lr_fsk() && ls_lora_chip()==LS_LORA_CHIP_LR20XX &&
           ls_lora_rx_range_ok(ls_lora_caps(),hz,hz) && lr_fsk_iq_fits(&m);
}
static bool lr_fsk_iq_session(bool on)
{
    if(on) {
        if(s_lrf_run)return true;
        s_fsk_error[0]=0;
        if(!s_lrf_task)
            s_lrf_task=xTaskCreateStaticPinnedToCore(lr_fsk_worker,"subghz_lrfsk",
                LRF_STACK_WORDS,NULL,3,s_lrf_stack,&s_lrf_tcb,1);
        const int64_t deadline=esp_timer_get_time()+1000000;
        while((!s_scan_busy || s_catch_armed) && !ls_mesh_radio_hold(true) && esp_timer_get_time()<deadline)
            vTaskDelay(pdMS_TO_TICKS(10));
        if((s_scan_busy && !s_catch_armed) || !ls_mesh_radio_held()) {
            ls_mesh_radio_hold(false);
            LS_SNPUTS(t_rec_watch_runtime_mesh_would_not_release, s_fsk_error, sizeof(s_fsk_error), "Mesh would not release the radio");
            return false;
        }
        memset(&s_lrf,0,sizeof(s_lrf));
        s_lrf.floor=8;s_lrf.floor_acc=8<<6;
        if(lr_wf_start_consumer(rec_get_freq(),lr_fsk_consume,NULL)!=ESP_OK) {
            ls_mesh_radio_hold(false);
            LS_SNPUTS(t_rec_watch_runtime_lr2021_stream_busy_close, s_fsk_error, sizeof(s_fsk_error), "LR2021 stream busy; close its waterfall first");
            return false;
        }
        s_lrf_run=true;
        return true;
    }
    if(!s_lrf_run)return true;
    lr_wf_stop();
    s_lrf_run=false;
    ls_mesh_radio_hold(false);
    return true;
}
/* The LoRa chip's WATCH: the OOK stream on an LR2021, its FSK from the same
   stream when the capture setup fits it, FSK packets otherwise. */
static bool lora_session(bool on)
{
    if(on)return lr_ook_wanted()?lr_ook_session(true):
                 lr_fsk_iq_wanted()?lr_fsk_iq_session(true):fsk_session(true);
    const bool ook=lr_ook_session(false);
    const bool iq=lr_fsk_iq_session(false);
    return fsk_session(false) && ook && iq;
}

static bool rec_watch_enable_owned(bool on)
{
    portENTER_CRITICAL(&s_lock);bool ready=s_status.ready;portEXIT_CRITICAL(&s_lock);
    if(on && !ready)return false;
    if(on)s_fsk_error[0]=0;
    /* A sweep or a LEARN run owns the SX1262 until it ends; starting a
       receive session under it would be two tasks driving one radio. */
    if(on && ((s_scan_busy && !s_catch_armed) || s_learn_busy || s_scan_min || s_learn_secs || s_iq_busy || s_replay_busy || s_replay_id || s_replay_path[0])) {
        LS_SNPUTS(t_rec_watch_runtime_stop_scan_before_watch, s_fsk_error, sizeof(s_fsk_error), "Stop SCAN before WATCH");
        return false;
    }
    if (rec_watch_source() == REC_SOURCE_CC1101 && !ls_mixrf_capture(on, rec_get_freq())) {
        ls_mixrf_status_t mix;ls_mixrf_snapshot(&mix);
        snprintf(s_fsk_error,sizeof(s_fsk_error),"%s",ls_mixrf_cc_refusal(&mix));return false;
    }
    if (rec_watch_source() == REC_SOURCE_SX1262 && !lora_session(on))
        return false;
    portENTER_CRITICAL(&s_lock);
    bool ok=!on || s_status.ready;
    if(ok)s_status.enabled=on;
    portEXIT_CRITICAL(&s_lock);
    return ok;
}
/* The persistent worker owns every LoRa WATCH lease, including UI requests. */
bool rec_watch_enable(bool on)
{
    if(rec_watch_source()!=REC_SOURCE_SX1262 || xTaskGetCurrentTaskHandle()==s_worker_task)
        return rec_watch_enable_owned(on);
    portENTER_CRITICAL(&s_lock);
    bool ok=s_status.ready && !s_watch_waiting &&
        (!on || (!s_scan_busy && !s_learn_busy && !s_iq_busy && !s_replay_busy && !s_replay_id && !s_replay_path[0]));
    if(ok){s_watch_on=on;s_watch_waiting=true;s_watch_pending=true;}
    portEXIT_CRITICAL(&s_lock);
    if(!ok)return false;
    /* The worker answers between jobs, and a sweep can hold it for minutes.
       Never wait that long on the caller's task (the UI): an OFF is left
       queued for the worker and reported done, an ON is withdrawn. Once the
       worker has taken it, the request runs to its own bounded end (the mesh
       release waits a second at most) and the caller waits for the answer. */
    const int64_t deadline=esp_timer_get_time()+1500000;
    for(;;) {
        portENTER_CRITICAL(&s_lock);
        bool pending=s_watch_pending,result=s_watch_result;
        if(pending && !s_watch_taken && esp_timer_get_time()>deadline) {
            if(on)s_watch_pending=false;
            else s_status.enabled=false;
            s_watch_waiting=false;
            portEXIT_CRITICAL(&s_lock);
            if(on)LS_SNPUTS(t_rec_watch_runtime_radio_busy_try_again, s_fsk_error, sizeof(s_fsk_error), "Radio busy - try again");
            return !on;
        }
        portEXIT_CRITICAL(&s_lock);
        if(!pending) {
            portENTER_CRITICAL(&s_lock);s_watch_waiting=false;portEXIT_CRITICAL(&s_lock);
            return result;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
/* Synchronize a stopped hardware request before reporting or using WATCH state. */
static void sync_cc_watch(void)
{
    ls_mixrf_status_t mix;
    portENTER_CRITICAL(&s_lock);
    ls_mixrf_snapshot(&mix);
    if(s_source==REC_SOURCE_CC1101 && !mix.capture_requested)s_status.enabled=false;
    portEXIT_CRITICAL(&s_lock);
}
bool rec_watch_enabled(void)
{
    sync_cc_watch();
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
    sync_cc_watch();
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
    bool ok = !s_status.enabled && !s_watch_waiting && !s_scan_busy && !s_learn_busy &&
        !s_iq_busy && !s_replay_busy && !s_replay_id && !s_replay_path[0];
    if (ok) s_source = source;
    portEXIT_CRITICAL(&s_lock);
    if (ok && source == REC_SOURCE_CC1101) ls_mixrf_start();
    return ok;
}
bool rec_watch_request_replay(uint32_t id,int dbm)
{
    if(!id || dbm<-10 || dbm>22)return false;
    portENTER_CRITICAL(&s_lock);
    bool ok=!s_watch_waiting && !s_iq_busy && !s_replay_busy && !s_replay_id && !s_replay_path[0] && !s_status.enabled && !s_scan_busy && !s_scan_min && !s_learn_busy && !s_learn_secs;
    if(ok){s_replay_id=id;s_replay_dbm=dbm;
        snprintf(s_replay_result,sizeof(s_replay_result),"Replay queued");
        snprintf(s_status.export_status,sizeof(s_status.export_status),"Replay queued");}
    portEXIT_CRITICAL(&s_lock);
    return ok;
}

/* Send one capture. Runs on the storage worker because that is where the
   catalog is owned; it is a deliberate keypress, not a stream, so the few
   hundred milliseconds it costs that worker are not competing with
   anything. */
static void send_fsk(const char *what,uint32_t freq_hz,uint32_t bitrate,
                     uint32_t deviation_hz,uint32_t sync_word,
                     uint16_t preamble_bits,const int32_t *pulse,int edges,
                     int dbm,char *result,size_t len);
static void send_ook(const char *what,uint32_t freq_hz,const int32_t *pulse,int edges,
                     int dbm,char *result,size_t len);
static bool ook_on_lr(void);
/* Every replay, CC1101 or LoRa chip, passes subghz_tx_refusal here before
   either radio is touched, so a refusal reads as a reason on screen. */
static bool replay_refused(uint32_t hz,int dbm,size_t edges,uint64_t span_us,
                           char *result,size_t len)
{
    const char *why=subghz_tx_refusal(hz,dbm,edges,span_us);
    if(why)LS_SNPRINTF(t_rec_watch_runtime_replay_refused_3f_mhz, result,len,"Replay refused: %.3f MHz, %s",hz/1e6,why);
    return why!=NULL;
}
static void replay(uint32_t id,int dbm,char *result,size_t len)
{
    const rec_watch_record_t *r=NULL;
    for(int i=0;i<REC_WATCH_SLOTS;i++)
        if(s_catalog->record[i].event.id==id) r=&s_catalog->record[i];
    if(!r){LS_SNPRINTF(t_rec_watch_runtime_replay_no_capture_lu, result,len,"Replay: no capture #%lu",(unsigned long)id);return;}
    /* An OOK pattern is timings, so any radio's capture can be sent by a
       radio that keys OOK; only an FSK one needs the LoRa chip that made it. */
    if(r->event.bitrate && !rec_source_can_replay((rec_source_t)r->event.source)) {
        LS_SNPRINTF(t_rec_watch_runtime_replay_s_cannot_transmit, result,len,"Replay: %s cannot transmit",
                 rec_source_name((rec_source_t)r->event.source));
        return;
    }
    const size_t edges=r->event.edges>0?(size_t)r->event.edges:0;
    if(replay_refused(r->event.frequency,dbm,edges,
                      subghz_span_us(r->pulse,edges),result,len))
        return;
    if(r->event.source==REC_SOURCE_CC1101) {
        bool sent=ls_mixrf_replay(r->event.frequency,r->pulse,r->event.edges,dbm);
        snprintf(result,len,sent?"Sent #%lu on CC1101":"Replay: CC1101 failed/busy; check MIX-RF",(unsigned long)id);
        return;
    }
    char what[16];
    snprintf(what,sizeof(what),"#%lu",(unsigned long)id);
    if(!r->event.bitrate) {
        if(ook_on_lr()) send_ook(what,r->event.frequency,r->pulse,r->event.edges,dbm,result,len);
        else {
            bool sent=ls_mixrf_replay(r->event.frequency,r->pulse,r->event.edges,dbm);
            snprintf(result,len,sent?"Sent %s on CC1101":"Replay: no CC1101 or LR2021 to send OOK",what);
        }
        return;
    }
    send_fsk(what,r->event.frequency,r->event.bitrate,r->event.deviation_hz,
             r->event.sync_word,r->event.preamble_bits,r->pulse,r->event.edges,
             dbm,result,len);
}

/* Rebuild the frame from edge timings and send it once on the SX1262. */
static void send_fsk(const char *what,uint32_t freq_hz,uint32_t bitrate,
                     uint32_t deviation_hz,uint32_t sync_word,
                     uint16_t preamble_bits,const int32_t *pulse,int edges,
                     int dbm,char *result,size_t len)
{
    static EXT_RAM_BSS_ATTR uint8_t payload[LS_FSK_CAPTURE_BYTES];
    const size_t n=ls_fsk_payload_from_raw(pulse,(size_t)edges,
        bitrate,preamble_bits,payload,sizeof(payload));
    if(!n){LS_SNPUTS(t_rec_watch_runtime_replay_could_not_rebuild, result, len, "Replay: could not rebuild the frame");return;}

    const int64_t deadline=esp_timer_get_time()+1000000;
    while(!ls_mesh_radio_hold(true) && esp_timer_get_time()<deadline)
        vTaskDelay(pdMS_TO_TICKS(10));
    if(!ls_mesh_radio_held()) {
        ls_mesh_radio_hold(false);
        LS_SNPUTS(t_rec_watch_runtime_replay_mesh_would_not, result, len, "Replay: mesh would not release the radio");
        return;
    }
    ls_fsk_cfg_t cfg={0};
    cfg.freq_hz=freq_hz;
    cfg.bitrate=bitrate;
    cfg.deviation_hz=deviation_hz;
    cfg.bandwidth_hz=ls_lora_fsk_bw_snap(cfg.bitrate+2u*cfg.deviation_hz);
    cfg.sync_word=sync_word;
    cfg.preamble_bits=preamble_bits;
    cfg.payload_bytes=(uint8_t)n;
    cfg.power_dbm=(int8_t)dbm;
    esp_err_t err=ls_lora_fsk_begin(&cfg);
    if(err==ESP_OK) {
        err=ls_lora_fsk_send(payload,n);
        if(err==ESP_OK) {
            const int64_t end=esp_timer_get_time()+2000000;
            while(!ls_lora_send_done() && esp_timer_get_time()<end)
                vTaskDelay(pdMS_TO_TICKS(5));
            if(!ls_lora_send_done())err=ESP_ERR_TIMEOUT;
        }
        const esp_err_t ended=ls_lora_fsk_end();
        if(err==ESP_OK)err=ended;
    }
    ls_mesh_radio_hold(false);
    if(err==ESP_OK) LS_SNPRINTF(t_rec_watch_runtime_sent_s_u_bytes, result,len,"Sent %s, %u bytes at %d dBm",
        what,(unsigned)n,dbm);
    else snprintf(result,len,"%s",err==ESP_ERR_INVALID_STATE?
        "Replay: LoRa session busy; stop its receiver first":err==ESP_ERR_TIMEOUT?
        "Replay timed out; check radio connection":"Replay failed; check FSK settings and radio connection");
}


/* OOK on the LR2021. The chip sends an OOK packet of fixed-rate chips, so
   the edges become 20 us chips (50 kbps), one packet per burst: a quiet
   stretch of 4 ms or more ends a packet, and the gap is waited out between
   them, so frame repeats keep their spacing. A packet holds 2040 chips,
   about 41 ms, which covers a remote's frame; a longer burst runs on into
   the next packet. */
#include "rec_ook_packet.h"
static esp_err_t lr_ook_edges(const int32_t *pulse,int edges)
{
    static EXT_RAM_BSS_ATTR uint8_t chips[255];
    rec_ook_packet_t packet={0};
    while(packet.edge<(size_t)edges) {
        uint32_t gap_us=0;
        size_t bit=rec_ook_packet(&packet,pulse,(size_t)edges,chips,sizeof(chips),&gap_us);
        if(bit) {
            esp_err_t err=ls_lora_ook_send(chips,(bit+7)/8,50000);
            if(err!=ESP_OK)return err;
            const int64_t end=esp_timer_get_time()+1000000;
            while(!ls_lora_send_done() && esp_timer_get_time()<end) vTaskDelay(1);
            if(!ls_lora_send_done())return ESP_ERR_TIMEOUT;
        }
        if(gap_us) {
            const int64_t until=esp_timer_get_time()+gap_us;
            while(esp_timer_get_time()+1000<until) vTaskDelay(1);
            while(esp_timer_get_time()<until) { }
        }
    }
    return ESP_OK;
}
static bool ook_on_lr(void)
{
    ls_mixrf_status_t mix;ls_mixrf_snapshot(&mix);
    return !mix.cc && ls_lora_chip()==LS_LORA_CHIP_LR20XX;
}
static void send_ook(const char *what,uint32_t freq_hz,const int32_t *pulse,int edges,
                     int dbm,char *result,size_t len)
{
    const int64_t deadline=esp_timer_get_time()+1000000;
    while(!ls_mesh_radio_hold(true) && esp_timer_get_time()<deadline)
        vTaskDelay(pdMS_TO_TICKS(10));
    if(!ls_mesh_radio_held()) {
        ls_mesh_radio_hold(false);
        LS_SNPUTS(t_rec_watch_runtime_replay_mesh_would_not, result, len, "Replay: mesh would not release the radio");
        return;
    }
    /* The FSK session only places the frequency and the power; the burst
       itself goes out as OOK. */
    ls_fsk_cfg_t cfg={0};
    cfg.freq_hz=freq_hz;
    cfg.bitrate=50000;
    cfg.deviation_hz=25000;
    cfg.bandwidth_hz=ls_lora_fsk_bw_snap(100000);
    cfg.sync_word=0x2DD4;
    cfg.preamble_bits=16;
    cfg.payload_bytes=8;
    cfg.power_dbm=(int8_t)dbm;
    esp_err_t err=ls_lora_fsk_begin(&cfg);
    if(err==ESP_OK) {
        err=lr_ook_edges(pulse,edges);
        const esp_err_t ended=ls_lora_fsk_end();
        if(err==ESP_OK)err=ended;
    }
    ls_mesh_radio_hold(false);
    if(err==ESP_OK) LS_SNPRINTF(t_rec_watch_runtime_sent_s_once_on, result,len,"Sent %s once on LR2021 OOK at %d dBm",what,dbm);
    else snprintf(result,len,"%s",err==ESP_ERR_INVALID_STATE?
        "Replay: LoRa session busy; stop its receiver first":err==ESP_ERR_TIMEOUT?
        "Replay timed out; check radio connection":"Replay on LR2021 failed");
}

static void replay_file(const char *path,int dbm,char *result,size_t len)
{
    static EXT_RAM_BSS_ATTR int32_t edges[LS_TX_UNLOCKED?65536:REC_WATCH_EDGES];
    static EXT_RAM_BSS_ATTR char line[SUBGHZ_FILE_LINE_BYTES];
    static EXT_RAM_BSS_ATTR subghz_file_t f;
    if(!subghz_file_load(path,&f,edges,(int)(sizeof(edges)/sizeof(edges[0])),line,sizeof(line))) {
        LS_SNPUTS(t_rec_watch_runtime_replay_not_a_readable, result, len, "Replay: not a readable .sub file");
        return;
    }
    if(!strcmp(f.protocol,"LR2021_IQ")) {iq_replay(path,dbm,result,len);return;}
    if(replay_refused(f.freq_hz,dbm,(size_t)f.edges_total,f.span_us,result,len))
        return;
    /* REC writes an SX1262 or LR2021 FSK capture with a CC1101 preset too, so
       a Flipper can play it. With no CC1101 fitted (both T-Display-P4s) the
       file is still FSK the LoRa chip sends: only a CC1101 that answers
       takes it. */
    ls_mixrf_status_t mix;
    ls_mixrf_snapshot(&mix);
    if(subghz_file_is_cc_fsk(&f) && (mix.cc || !subghz_file_is_fsk(&f))) {
        bool sent=ls_mixrf_replay_fsk(f.freq_hz,edges,f.edges,dbm,&f.cc_fsk);
        snprintf(result,len,"%s",sent?"Sent FSK file once on CC1101":"Replay: CC1101 failed/busy; check MIX-RF");
        return;
    }
    if(subghz_file_is_ook(&f) && ook_on_lr()) {
        const char *name=strrchr(path,'/');
        send_ook(name?name+1:path,f.freq_hz,edges,f.edges,dbm,result,len);
        return;
    }
    if(subghz_file_is_ook(&f)) {
        bool sent=ls_mixrf_replay(f.freq_hz,edges,f.edges,dbm);
        snprintf(result,len,"%s",sent?"Sent file once on CC1101":"Replay: CC1101 failed/busy; check MIX-RF");
        return;
    }
    if(!subghz_file_is_fsk(&f)) {
        LS_SNPUTS(t_rec_watch_runtime_replay_incomplete_or_unsupported, result, len, "Replay: incomplete or unsupported RAW/modulation");
        return;
    }
    const char *name=strrchr(path,'/');
    send_fsk(name?name+1:path,f.freq_hz,f.bitrate,f.deviation_hz,f.sync_word,
             f.preamble_bits?f.preamble_bits:32,edges,f.edges,dbm,result,len);
}

bool rec_watch_request_replay_file(const char *path,int dbm)
{
    if(!path || !path[0] || strlen(path)>=sizeof(s_replay_path) || dbm<-10 || dbm>22)
        return false;
    if(!rec_watch_start()) {
        portENTER_CRITICAL(&s_lock);
        snprintf(s_fsk_error,sizeof(s_fsk_error),"%s",s_starting?
            "Recorder loading; retry when ready":"Replay worker allocation failed; retry later");
        portEXIT_CRITICAL(&s_lock);
        return false;
    }
    portENTER_CRITICAL(&s_lock);
    bool ok=s_status.ready && !s_watch_waiting && !s_iq_busy && !s_replay_busy && !s_replay_path[0] && !s_replay_id && !s_status.enabled && !s_scan_busy && !s_scan_min && !s_learn_busy && !s_learn_secs;
    if(ok){memcpy(s_replay_path,path,strlen(path)+1);s_replay_dbm=dbm;
        snprintf(s_replay_result,sizeof(s_replay_result),"Replay queued");
        snprintf(s_status.export_status,sizeof(s_status.export_status),"Replay queued");}
    if(!ok)snprintf(s_fsk_error,sizeof(s_fsk_error),"%s",
        !s_status.ready?"Recorder loading; retry when ready":
        s_status.enabled?"Stop WATCH before replay":s_scan_busy?"Stop SCAN before replay":
        s_learn_busy?"Stop LEARN before replay":"Wait for the current radio operation");
    portEXIT_CRITICAL(&s_lock);
    return ok;
}
const char *rec_watch_last_result(void){ return s_status.export_status; }

bool rec_watch_request_scan(uint32_t min_hz,uint32_t max_hz,uint32_t seconds,
                            int bins)
{
    /* 150-960 MHz, or as far as the part says it sweeps: the LR2021's two
       inputs reach 1100 MHz and 1500-2500 MHz. */
    if(max_hz<=min_hz || !ls_lora_rx_range_ok(ls_lora_caps(),min_hz,max_hz))return false;
    /* 0 means run until stopped. */
    if(seconds>180)return false;
    if(bins<4 || bins>REC_SCAN_BINS)bins=REC_SCAN_BINS;
    portENTER_CRITICAL(&s_lock);
    bool ok=s_status.ready && !s_scan_busy && !s_learn_busy && !s_watch_waiting && !s_status.enabled && !s_iq_busy && !s_replay_busy && !s_replay_id && !s_replay_path[0];
    if(ok){s_scan_stop_req=false;s_catch_armed=false;s_scan_busy=true;
        s_scan_source=s_source;s_scan_min=min_hz;s_scan_max=max_hz;s_scan_secs=seconds;s_scan_bins=bins;}
    portEXIT_CRITICAL(&s_lock);
    return ok;
}
bool rec_watch_scan_busy(void){ return s_scan_busy; }
int rec_watch_scan_result(rec_scan_bin_t *out,int max)
{
    if(!out || max<=0)return s_scan_n;
    int n=s_scan_n<max?s_scan_n:max;
    for(int i=0;i<n;i++)out[i]=s_scan[i];
    return n;
}
float rec_watch_scan_floor(void){ return s_scan_floor; }

/* A pass's floor is its quarter-way reading, not its lowest. One bad low
   look set the floor before: the LR2021 read one bin at -132 dBm in a
   -110 dBm band (2026-11-07), so every other bin stood "30 dB over the
   floor" and all 32 were detections. A quarter of the band can carry
   signals before this moves. */
static float pass_floor(const float *row,int nb,float *spread)
{
    float s[REC_SCAN_BINS];
    const int n=nb<REC_SCAN_BINS?nb:REC_SCAN_BINS;
    if(spread)*spread=0.0f;
    if(n<=0)return -200.0f;
    for(int i=0;i<n;i++) {
        const float v=row[i];
        int j=i;
        while(j>0 && s[j-1]>v){s[j]=s[j-1];j--;}
        s[j]=v;
    }
    if(spread)*spread=rec_scan_spread(s,n);
    return s[n/4];
}

/* Sweep, and keep what each bin heard at its loudest.

   One pass takes about 200 ms and a button press lasts 60, so a single sweep
   answers "was it transmitting exactly then" - which is almost never the
   question. Repeating for the whole window and keeping per-bin peaks answers
   "did anything speak here while I was listening", which is. */
static void scan_band(uint32_t min_hz,uint32_t max_hz,uint32_t secs,int nb,char *result,size_t len)
{
    s_scan_n=0;
    stage("Taking the radio",0);
    const int64_t deadline=esp_timer_get_time()+1000000;
    while(!s_scan_stop_req && !ls_mesh_radio_hold(true) && esp_timer_get_time()<deadline)
        vTaskDelay(pdMS_TO_TICKS(10));
    if(s_scan_stop_req) {
        ls_mesh_radio_hold(false);
        snprintf(result,len,"%s stopped",s_scan_busy?"Scan":"Learn");
        return;
    }
    if(!ls_mesh_radio_held()) {
        ls_mesh_radio_hold(false);
        LS_SNPUTS(t_rec_watch_runtime_scan_mesh_would_not, result, len, "Scan: mesh would not release the radio");
        return;
    }
    if(ls_lora_scan_begin(min_hz,max_hz)!=ESP_OK) {
        ls_mesh_radio_hold(false);
        LS_SNPUTS(t_rec_watch_runtime_scan_the_receiver_refused, result, len, "Scan: the receiver refused that range");
        return;
    }
    static EXT_RAM_BSS_ATTR float peak[REC_SCAN_BINS];
    static EXT_RAM_BSS_ATTR float row[REC_SCAN_BINS];
    for(int i=0;i<nb;i++)peak[i]=-200.0f;
    const int64_t start=esp_timer_get_time();
    /* Zero means keep going until stopped. Capped anyway, because a sweep
       holds the radio the mesh needs and an instrument left running by
       accident should not take the node down for the rest of the day. */
    const int64_t end=start+(int64_t)(secs?secs:180)*1000000;
    int passes=0;
    s_scan_hits=0;
    s_scan_events=0;
    memset(s_scan_above,0,sizeof(s_scan_above));
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
            float spread=0.0f;
            const float lo=pass_floor(row,nb,&spread);
            /* The floor gets ballistics too, or the whole display shifts
               under the columns every pass; so does the noise spread. */
            if(s_scan_live_n) {
                s_scan_live_floor += (lo-s_scan_live_floor)*0.25f;
                s_scan_live_spread += (spread-s_scan_live_spread)*0.25f;
            } else {
                s_scan_live_floor = lo;
                s_scan_live_spread = spread;
            }
            const float attack=rec_scan_attack_level(s_scan_live_floor,s_scan_live_spread);

            for(int i=0;i<nb;i++) {
                s_scan_live[i].hz=ls_lora_scan_bin_hz(min_hz,max_hz,nb,i);
                if(!s_scan_live_n) { s_scan_live[i].dbm=row[i]; s_scan_live[i].hold=row[i]; continue; }
                /* Instant attack for a signal, an even follow for noise:
                   rec_scan_live.h says why. */
                s_scan_live[i].dbm = rec_scan_follow(s_scan_live[i].dbm,row[i],attack);
                /* And a peak that falls about a dB a pass, so a burst that
                   landed between looks at this bin still leaves a mark. */
                if(row[i] > s_scan_live[i].hold && row[i] >= attack) s_scan_live[i].hold = row[i];
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
            int rising=0; uint32_t rise_hz=0; float rise_dbm=-200.0f;
            for(int i=0;i<nb;i++) {
                const bool above=row[i] >= lo+rec_watch_scan_threshold();
                if(above && !s_scan_above[i]) {
                    rising++;
                    if(row[i]>rise_dbm){rise_dbm=row[i];rise_hz=s_scan_live[i].hz;}
                }
                s_scan_above[i]=above;
                if(!above) continue;
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
            /* Once per pass, for the strongest burst that started in it -
               a new frequency or a known one speaking again. Buzzing only
               for new frequencies made the second press of the same remote
               silent. */
            if(rising && rise_hz) {
                s_scan_events+=rising;
                s_last_hit=rise_hz;
                const rec_scan_on_hit_t mode=rec_watch_scan_on_hit_get();
                /* A floor on the spacing, whatever the threshold is set to.
                   A threshold below the noise makes every pass a detection,
                   and a limit here keeps a bad setting from shaking the
                   device across the bench. 600 ms still separates two
                   presses of a remote. */
                static int64_t last_alert;
                const int64_t now_alert=esp_timer_get_time();
                const bool may_alert=now_alert-last_alert>600000;
                if(mode!=REC_SCAN_ON_HIT_NOTHING && may_alert) {
                    last_alert=now_alert;
                    ls_haptic_play(LS_HAPTIC_ALERT);
                    char peer[17];
                    portENTER_CRITICAL(&s_lock);
                    bool alerts=s_status.alerts;
                    memcpy(peer,s_status.peer,sizeof(peer));
                    portEXIT_CRITICAL(&s_lock);
                    /* A message only for a frequency not heard before this
                       sweep; a buzz for every burst is fine, a DM is not. */
                    if(alerts && peer[0] && fresh_hits) {
                        char text[80];
                        LS_SNPRINTF(t_rec_watch_runtime_subghz_4f_mhz_at, text,sizeof(text),"SubGHz: %.4f MHz at %.0f dBm",
                                 fresh_hz/1e6,(double)fresh_dbm);
                        rec_watch_notify(peer,text);
                    }
                }
                /* And the hand-off: stop looking around, point at it, and
                   let the receiver try to make a capture out of it. */
                if(mode==REC_SCAN_ON_HIT_CATCH) {
                    rec_watch_scan_catch(rise_hz);
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
        snprintf(result,len,"%s",s_scan_stop_req?"Scan stopped":"Scan: no sweep completed");
        return;
    }
    /* The quietest bin THIS sweep measured. Only the first nb entries
       were written; peak[] is a static that outlives the call, so
       reducing over all of REC_SCAN_BINS mixes in whatever a previous
       sweep of a different band left in the tail - which a narrow
       sweep after a wide one reports as a floor tens of dB off. */
    s_scan_floor=pass_floor(peak,nb,NULL);

    /* Strongest first, so the list opens on the thing worth looking at. */
    for(int i=1;i<s_scan_n;i++) {
        rec_scan_bin_t k=s_scan[i]; int j=i-1;
        while(j>=0 && s_scan[j].dbm<k.dbm){s_scan[j+1]=s_scan[j];j--;}
        s_scan[j+1]=k;
    }
    if(s_scan_stop_req && !s_catch_armed) {
        LS_SNPRINTF(t_rec_watch_runtime_scan_stopped_d_found, result,len,"Scan stopped: %d found",s_scan_n);return;
    }
    if(s_scan_n) LS_SNPRINTF(t_rec_watch_runtime_scan_d_pass_s, result,len,"Scan: %d pass%s, %d found, strongest %.0f dBm",
        passes,passes==1?"":"es",s_scan_n,(double)s_scan[0].dbm);
    else LS_SNPRINTF(fmt_rec_watch_runtime_scan_pass_nothing_crossed, result,len,"Scan: %d pass%s, nothing crossed %.0f dB over the floor",
        passes,passes==1?"":"es",(double)rec_watch_scan_threshold());
}

bool rec_watch_request_learn(uint32_t seconds)
{
    if(seconds<5 || seconds>90)return false;
    portENTER_CRITICAL(&s_lock);
    bool ok=s_status.ready && !s_scan_busy && !s_learn_busy && !s_watch_waiting && !s_status.enabled && !s_iq_busy && !s_replay_busy && !s_replay_id && !s_replay_path[0];
    if(ok){s_scan_stop_req=false;s_catch_armed=false;s_learn_busy=true;s_learn_rewatch=false;
        s_learn_secs=seconds;s_learn_hz=rec_get_freq();}
    portEXIT_CRITICAL(&s_lock);
    return ok;
}
bool rec_watch_learn_busy(void){ return s_learn_busy; }
bool rec_watch_learn_rewatch(void)
{
    portENTER_CRITICAL(&s_lock);
    const bool ok=s_learn_busy;
    if(ok)s_learn_rewatch=true;
    portEXIT_CRITICAL(&s_lock);
    return ok;
}
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
    while(!s_scan_stop_req && esp_timer_get_time()<end) {
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
   repeatedly rather than one transmission. False when the radio was never
   put on the rate, so nothing waited. */
static bool try_rate_window(uint32_t hz,uint32_t rate,uint32_t dev,int ms,
                            uint8_t (*seen)[LEARN_BYTES],int *n)
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
    if(s_scan_stop_req || ls_lora_fsk_begin(&cfg)!=ESP_OK)return false;
    const int64_t end=esp_timer_get_time()+(int64_t)ms*1000;
    while(!s_scan_stop_req && esp_timer_get_time()<end && *n<LEARN_KEEP) {
        uint8_t buf[LEARN_BYTES]; float rssi;
        const int got=ls_lora_fsk_poll(buf,sizeof(buf),&rssi);
        if(got>0){memcpy(seen[*n],buf,LEARN_BYTES);(*n)++;}
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    ls_lora_fsk_end();
    return true;
}

/* The most frames that agree among those one rate heard. */
static int rate_agreement(uint8_t (*seen)[LEARN_BYTES],int n,uint8_t *best,int *best_len)
{
    int best_count=0,best_i=0;
    for(int i=0;i<n;i++) {
        int c=0;
        for(int j=0;j<n;j++) if(!memcmp(seen[i],seen[j],LEARN_BYTES))c++;
        if(c>best_count){best_count=c;best_i=i;}
    }
    if(best_count>=2 && best){memcpy(best,seen[best_i],LEARN_BYTES);*best_len=LEARN_BYTES;}
    return n<2?(n?1:0):best_count;
}

/* The sync word is whatever follows the preamble run - found to the bit.
   Whole bytes of 0xAA/0x55 skipped left the word up to seven bits early,
   preamble bits in front (an LR2021 learning 2DD42DD4 beside an SX1262
   reporting A96EA16E for the same remote). The run ends where two bits
   first agree; the word is taken from one bit before that, because the
   first sync bit may have continued the alternation. One preamble bit too
   many still receives - it is always sent before the sync - where one sync
   bit too few would put a payload bit in the word. */
static int frame_bit(const uint8_t *b,int at){return (b[at>>3]>>(7-(at&7)))&1;}
static uint32_t sync_from(const uint8_t *b,int len)
{
    const int bits=len*8;
    int i=1;
    while(i<bits && frame_bit(b,i)!=frame_bit(b,i-1))i++;
    if(i>=9 && i-1+32<=bits) {
        uint32_t w=0;
        for(int k=0;k<32;k++)w=(w<<1)|(uint32_t)frame_bit(b,i-1+k);
        return w;
    }
    /* No run to find: the bytes as they came. */
    int j=0;
    while(j<len && (b[j]==0xAA || b[j]==0x55))j++;
    if(j+4>len)return 0;
    return ((uint32_t)b[j]<<24)|((uint32_t)b[j+1]<<16)|
           ((uint32_t)b[j+2]<<8)|b[j+3];
}

/* LEARN on the LR2021: its I/Q decoder measures the rate from the preamble,
   the deviation from the two tones and reads the sync word, so one press is
   enough for a packet that proves itself and two for one that does not
   (lr_learn_offer). The SX1262 has no I/Q and keeps the search below, which
   needs the same frame twice at each rate it tries. */
static bool learn_run_iq(uint32_t hz,uint32_t secs,char *result,size_t len)
{
    (void)hz;
    s_lrf_learned=false;s_lrf_cands=0;s_lrf_learning=true;
    stage("Listening for one burst",5);
    if(!lr_fsk_iq_session(true)) {
        s_lrf_learning=false;
        snprintf(result,len,"Learn: %s",s_fsk_error[0]?s_fsk_error:"LR2021 stream busy");
        return true;
    }
    const int64_t start=esp_timer_get_time(),end=start+(int64_t)secs*1000000;
    while(!s_scan_stop_req && !s_lrf_learned && esp_timer_get_time()<end) {
        stage(s_lrf_cands?"Heard one burst - press it again":"Listening for one burst",
              5+(int)((esp_timer_get_time()-start)*90/((int64_t)secs*1000000)));
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    lr_fsk_iq_session(false);
    s_lrf_learning=false;
    stage("",0);
    if(s_scan_stop_req){memset(&s_learn,0,sizeof(s_learn));snprintf(result,len,"Learn stopped");return true;}
    if(!s_lrf_learned && s_lrf_cands) {
        snprintf(s_learn.note,sizeof(s_learn.note),"%s","one burst, nothing to confirm it");
        snprintf(result,len,"Learn: one burst at %lu baud, not confirmed - press it twice",
                 (unsigned long)s_lrf_cand_baud);
        return true;
    }
    if(!s_lrf_learned) {
        snprintf(s_learn.note,sizeof(s_learn.note),"%s","nothing heard");
        snprintf(result,len,"Learn: heard no FSK packet - press the remote");
        return true;
    }
    const fsk_pkt_t *p=&s_lrf_learn_pkt;
    const uint32_t w=p->sync&0xFFFFu;
    /* A word sent twice is the whole 32; otherwise the top half is the word
       and the bottom half what follows it, which WATCH on this chip does not
       compare (it matches the bits it decoded). */
    uint32_t sync=(w<<16)|w;
    if(p->sync_repeats<2) sync=(w<<16)|(p->len>=2?((uint32_t)p->data[0]<<8)|p->data[1]:w);
    s_learn.bitrate=p->baud_std?p->baud_std:p->baud;
    s_learn.deviation_hz=(s_lrf_learn_dev>=600.0f && s_lrf_learn_dev<=200000.0f)?(uint32_t)(s_lrf_learn_dev+0.5f):0;
    if(!s_learn.deviation_hz){rec_fsk_mod_t m;rec_watch_fsk_get(&m);s_learn.deviation_hz=m.deviation_hz;}
    s_learn.sync_word=sync;
    s_learn.preamble_bits=(uint16_t)(p->preamble_bits>=8?p->preamble_bits:0);
    /* A packet that proved itself, or two bursts alike, as two frames alike
       are on the SX1262. */
    s_learn.tried=s_learn.agreed=s_lrf_learn_bursts;
    s_learn.confidence=(uint8_t)(lr_learn_proven(p)?95:80);
    s_learn.valid=true;
    snprintf(s_learn.note,sizeof(s_learn.note),"%s, %s deviation, %u-byte packet",
             s_lrf_learn_bursts>1?"two bursts agreed":"one burst",
             s_lrf_learn_dev>=600.0f?"measured":"assumed",(unsigned)p->len);
    LS_SNPRINTF(t_rec_watch_runtime_learn_lu_baud_sync, result,len,"Learn: %lu baud, sync %08lX, %u%% confident",
        (unsigned long)s_learn.bitrate,(unsigned long)s_learn.sync_word,s_learn.confidence);
    return true;
}

static void learn_run(uint32_t hz,uint32_t secs,char *result,size_t len)
{
    static const uint32_t RATE[]={600,1200,2400,4800,9600,19200,38400};
    const int n_rate=(int)(sizeof(RATE)/sizeof(RATE[0]));
    memset(&s_learn,0,sizeof(s_learn));
    if(ls_lora_chip()==LS_LORA_CHIP_LR20XX && ls_lora_rx_range_ok(ls_lora_caps(),hz,hz) &&
       learn_run_iq(hz,secs,result,len))
        return;
    stage("Taking the radio",0);

    const int64_t deadline=esp_timer_get_time()+1000000;
    while(!s_scan_stop_req && !ls_mesh_radio_hold(true) && esp_timer_get_time()<deadline)
        vTaskDelay(pdMS_TO_TICKS(10));
    if(s_scan_stop_req) {
        ls_mesh_radio_hold(false);
        snprintf(result,len,"%s stopped",s_scan_busy?"Scan":"Learn");
        return;
    }
    if(!ls_mesh_radio_held()) {
        ls_mesh_radio_hold(false);
        LS_SNPUTS(t_rec_watch_runtime_learn_mesh_would_not, result, len, "Learn: mesh would not release the radio");
        return;
    }

    /* A quarter of the window on the measurement, the rest on the search. */
    uint32_t dev=measure_deviation(hz,secs/4?secs/4:1);
    bool measured=dev!=0;
    if(!dev) { rec_fsk_mod_t m; rec_watch_fsk_get(&m); dev=m.deviation_hz; }

    /* Round after round of short looks at every rate, the frames each rate
       heard kept across rounds. One long look per rate needed the same
       frame twice inside that rate's three seconds: a remote pressed every
       couple of seconds was never learned. Now any two presses that land on
       the right rate, in any rounds, agree. */
    static EXT_RAM_BSS_ATTR uint8_t seen[7][LEARN_KEEP][LEARN_BYTES];
    int heard[7]={0};
    const int64_t search_end=esp_timer_get_time()+(int64_t)(secs-(secs/4))*1000000;
    const int window_ms=1200;
    int tried=0, round=0;
    while(!s_scan_stop_req && esp_timer_get_time()<search_end) {
        bool listened=false;
        for(int i=0;i<n_rate && !s_scan_stop_req && esp_timer_get_time()<search_end;i++) {
            char what[40];
            snprintf(what,sizeof(what),"Round %d, %lu baud",round+1,(unsigned long)RATE[i]);
            const int64_t left=search_end-esp_timer_get_time();
            stage(what,25+(int)(75-left*75/((int64_t)(secs-(secs/4))*1000000+1)));
            if(heard[i]<LEARN_KEEP && try_rate_window(hz,RATE[i],dev,window_ms,seen[i],&heard[i]))
                listened=true;
            if(!round)tried++;
        }
        round++;
        /* Every rate full: nothing more to hear. */
        bool full=true;
        for(int i=0;i<n_rate;i++) if(heard[i]<LEARN_KEEP) full=false;
        if(full)break;
        /* A round the radio refused every rate must still yield the core. */
        if(!listened)vTaskDelay(pdMS_TO_TICKS(10));
    }
    uint8_t best[LEARN_BYTES]; int best_len=0, best_agreed=0, best_heard=0; uint32_t best_rate=0;
    for(int i=0;i<n_rate;i++) {
        uint8_t got[LEARN_BYTES]; int got_len=0;
        const int agreed=rate_agreement(seen[i],heard[i],got,&got_len);
        if(agreed>best_agreed){best_agreed=agreed;best_rate=RATE[i];best_heard=heard[i];
            if(got_len){memcpy(best,got,LEARN_BYTES);best_len=got_len;}}
    }
    ls_mesh_radio_hold(false);
    stage("",0);

    if(s_scan_stop_req) {
        memset(&s_learn,0,sizeof(s_learn));
        snprintf(result,len,"Learn stopped");return;
    }
    s_learn.tried=(uint8_t)tried;
    s_learn.agreed=(uint8_t)best_agreed;
    s_learn.deviation_hz=dev;
    if(best_agreed>=2 && best_rate) {
        s_learn.bitrate=best_rate;
        s_learn.sync_word=sync_from(best,best_len);
        /* Two 24-byte frames alike at one rate do not happen by chance; what
           lowers it is the rest of what that rate heard disagreeing. */
        int conf=best_agreed>=4?95:best_agreed==3?90:80;
        if(best_agreed*2<best_heard)conf-=20;
        s_learn.confidence=(uint8_t)conf;
        s_learn.valid=s_learn.sync_word!=0;
        LS_SNPRINTF(t_rec_watch_runtime_s_deviation_d_of, s_learn.note,sizeof(s_learn.note),"%s deviation, %d of %d agreed",
                 measured?"measured":"assumed",best_agreed,LEARN_KEEP);
        LS_SNPRINTF(t_rec_watch_runtime_learn_lu_baud_sync, result,len,"Learn: %lu baud, sync %08lX, %u%% confident",
            (unsigned long)best_rate,(unsigned long)s_learn.sync_word,
            s_learn.confidence);
    } else {
        LS_SNPUTS(t_rec_watch_runtime_nothing_repeated_at_any, s_learn.note, sizeof(s_learn.note), "nothing repeated at any rate");
        LS_SNPUTS(t_rec_watch_runtime_learn_heard_nothing_consistent, result, len, "Learn: heard nothing consistent - press it repeatedly");
    }
}

/* One LEARN on the worker. WATCH handed back by the screen that paused it
   is restored here, after the radio is free, whether or not the screen is
   still open. */
static void learn_job(uint32_t hz,uint32_t secs)
{
    char result[112];
    learn_run(hz,secs,result,sizeof(result));
    portENTER_CRITICAL(&s_lock);
    snprintf(s_status.export_status,sizeof(s_status.export_status),"%s",result);
    snprintf(s_status.learn_result,sizeof(s_status.learn_result),"%s",result);
    s_status.learn_generation++;s_learn_busy=false;
    const bool rewatch=s_learn_rewatch && !s_status.enabled;s_learn_rewatch=false;
    portEXIT_CRITICAL(&s_lock);
    if(rewatch)rec_watch_enable_owned(true);
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
        LS_SNPUTS(t_rec_watch_runtime_export_queued_saving_selected, s_status.export_status, sizeof(s_status.export_status), "Export queued; saving selected pattern");
    }
    portEXIT_CRITICAL(&s_lock);return ok;
}

/* Snapshot is separate from export feedback: an export cannot finish a replay. */
bool rec_watch_replay_status(char *out,size_t len)
{
    portENTER_CRITICAL(&s_lock);
    bool busy=s_replay_busy || s_replay_id || s_replay_path[0];
    if(out && len)snprintf(out,len,"%s",s_replay_result);
    portEXIT_CRITICAL(&s_lock);
    return busy;
}
