/* CATCH owns an RTL endpoint and, only while useful, a background socket
   lease. Two workers keep USB reception independent of SPI retunes and SD. */
#include "rec_catch.h"
#include "rec_watch.h"
#include "subghz_file.h"
#include "ls_sub_fsk.h"
#include "ls_lora.h"
#include "rtl-sdr.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Defined by REC's weak no-mesh fallback when MeshCore is not linked. */
extern bool ls_mesh_radio_hold_bg(bool);
extern bool ls_mesh_radio_wanted(void);
#define CAP_EDGES 2048
#define STACK_BYTES 8192
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_rtl_task, s_lr_task;
static StaticTask_t s_rtl_tcb, s_lr_tcb;
static EXT_RAM_BSS_ATTR catch_detector_t s_detector;
static EXT_RAM_BSS_ATTR catch_status_t s_status;
static EXT_RAM_BSS_ATTR catch_result_t s_result[CATCH_RESULTS];
static EXT_RAM_BSS_ATTR int32_t s_edges[CATCH_RESULTS][CAP_EDGES];
static EXT_RAM_BSS_ATTR uint8_t s_iq[32768], s_bits[256];
static EXT_RAM_BSS_ATTR lr433_work_t s_decode;
static EXT_RAM_BSS_ATTR lr433_msg_t s_messages[8];
static EXT_RAM_BSS_ATTR int32_t s_pulses[CAP_EDGES];
static EXT_RAM_BSS_ATTR catch_burst_t s_pending;
static EXT_RAM_BSS_ATTR catch_sources_t s_sources;
static bool s_has_pending;
static int s_head, s_save_slot=-1;
static uint32_t s_save_id;
static bool s_run, s_rtl_done=true, s_lr_done=true, s_starting;
static catch_mod_t s_mod;
static lr433_in_t s_kind;
static uint32_t s_rate, s_lead, s_dev;
static int s_lead_bits, s_slot=-1;
static bool s_lease;
static uint64_t s_rtl_epoch, s_usb_drops;
static unsigned s_full_allowance=1;
static uint64_t s_status_due;
static uint64_t s_trace_until, s_trace_second;
static unsigned s_trace_lines, s_trace_suppressed;
#define TRACE_QUEUE 8
static TaskHandle_t s_trace_task;
static StaticTask_t s_trace_tcb;
typedef struct {catch_trace_t event;uint64_t period, gap;} trace_line_t;
static EXT_RAM_BSS_ATTR trace_line_t s_trace_queue[TRACE_QUEUE];
static unsigned s_trace_head, s_trace_count;

static bool running(void) { return __atomic_load_n(&s_run,__ATOMIC_ACQUIRE); }
static void lock(void) { xSemaphoreTake(s_lock,portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }
/* Console backpressure must never block the IQ/coarse worker. Queue overflow
   suppresses diagnostics only; event strings are static detector literals. */
static void trace_candidate(const catch_trace_t *t,void *ctx)
{
    (void)ctx;
    uint64_t now=esp_timer_get_time();
    lock();
    if(now>=s_trace_until) {unlock();return;}
    uint64_t second=now/1000000;
    if(second!=s_trace_second) {s_trace_second=second;s_trace_lines=0;}
    if(s_trace_lines++>=40 || s_trace_count==TRACE_QUEUE) {
        s_trace_suppressed++;unlock();return;
    }
    unsigned slot=(s_trace_head+s_trace_count++)%TRACE_QUEUE;
    s_trace_queue[slot]=(trace_line_t){*t,s_status.block_period_us,s_status.block_gap_us};
    unlock();
    if(s_trace_task)xTaskNotifyGive(s_trace_task);
}
static bool trace_drain(void)
{
    trace_line_t line;
    lock();
    if((uint64_t)esp_timer_get_time()>=s_trace_until)s_trace_count=0;
    if(!s_trace_count) {unlock();return false;}
    line=s_trace_queue[s_trace_head];s_trace_head=(s_trace_head+1)%TRACE_QUEUE;s_trace_count--;
    unsigned suppressed=s_trace_suppressed;s_trace_suppressed=0;
    unlock();
    if(suppressed)printf("catch trace suppressed %u lines\n",suppressed);
    const catch_trace_t *t=&line.event;
    printf("catch trace block=%llu us id=%lu coarse=%.1f dB/floor bin=%u freq=%lu FFT=%s state=%s reason=%s period=%llu us gap=%llu us\n",
        (unsigned long long)t->block_us,(unsigned long)t->id,(double)t->coarse_db,
        t->bin,(unsigned long)t->frequency,t->fft,t->state,t->reason,
        (unsigned long long)line.period,(unsigned long long)line.gap);
    return true;
}
static void trace_worker(void *arg)
{
    (void)arg;
    for(;;) {
        ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
        while(trace_drain()) {}
    }
}

/* Called with the result lock. Keep physical slots stable for socket captures
   and pending saves while hiding rows identified during later spur learning. */
static int result_slot(int index)
{
    for(int j=0;j<CATCH_RESULTS;j++) {
        int slot=(s_head-1-j+CATCH_RESULTS)%CATCH_RESULTS;
        if(s_result[slot].burst.id && !s_result[slot].burst.spur && index--==0)return slot;
    }
    return -1;
}
static void hide_learned_spurs(void)
{
    for(int j=0;j<CATCH_RESULTS;j++) {
        catch_burst_t *b=&s_result[j].burst;
        if(b->id && !b->spur && catch_detector_is_spur(&s_detector,b->frequency,b->rtl_dbfs)) {
            b->spur=true;if(s_status.count)s_status.count--;
        }
    }
}

static void detection(const catch_burst_t *b,bool start,void *ctx)
{
    (void)ctx;
    if(b->spur)return;
    /* Chip inspection is independent of the result lock and repeat policy. */
    bool has_stream=ls_lora_chip()==LS_LORA_CHIP_LR20XX;
    lock();
    if(start) {
        /* Callback retries and overlapping detector lobes update one row. */
        for(int j=0;j<CATCH_RESULTS;j++) if(s_result[j].burst.id && !s_result[j].burst.spur &&
            (s_result[j].burst.id==b->id ||
             (llabs((long long)s_result[j].burst.frequency-b->frequency)<=15000 &&
              b->start_us<=s_result[j].burst.stop_us+CATCH_MERGE_US &&
              b->stop_us+CATCH_MERGE_US>=s_result[j].burst.start_us))) {
            if(b->stop_us>s_result[j].burst.stop_us)s_result[j].burst.stop_us=b->stop_us;
            if(b->rtl_dbfs>s_result[j].burst.rtl_dbfs)s_result[j].burst.rtl_dbfs=b->rtl_dbfs;
            unlock();return;
        }
        int i=s_head; s_head=(s_head+1)%CATCH_RESULTS;
        if(s_result[i].burst.id && !s_result[i].burst.spur)s_status.dropped++;
        else s_status.count++;
        memset(&s_result[i],0,sizeof(s_result[i])); s_result[i].burst=*b;
        s_result[i].burst.detected_us=esp_timer_get_time();
        s_result[i].lr_rssi=NAN;
        snprintf(s_result[i].decode_status,sizeof(s_result[i].decode_status),"waiting for LR2021");
        s_status.detections++;
        uint64_t elapsed=b->start_us>=s_rtl_epoch?b->start_us-s_rtl_epoch:0;
        bool eligible=catch_source_observe(&s_sources,&s_result[i].burst,s_status.center,elapsed);
        const char *why=!has_stream?"LR2021 absent (SX1262 has no stream)":
            b->frequency<400000000u || b->frequency>1100000000u?
            "LR2021 sensitivity unavailable in this band":NULL;
        if(why) {
            snprintf(s_result[i].decode_status,sizeof(s_result[i].decode_status),"%s",why);
            snprintf(s_status.lr_status,sizeof(s_status.lr_status),"%s",why);
            unlock();return;
        }
        if(!eligible) {
            snprintf(s_result[i].decode_status,sizeof(s_result[i].decode_status),"%s",
                elapsed<CATCH_HANDOFF_WARM_US?"spur warm-up: no handoff":"waiting for similar repeat");
            unlock();return;
        }
        if(s_has_pending) {
            if(!catch_handoff_prefer(&s_result[i].burst,&s_pending)) {
                snprintf(s_result[i].decode_status,sizeof(s_result[i].decode_status),"stronger burst pending");
                unlock();return;
            }
            s_status.dropped++;
            for(int j=0;j<CATCH_RESULTS;j++) if(s_result[j].burst.id==s_pending.id)
                snprintf(s_result[j].decode_status,sizeof(s_result[j].decode_status),"handoff queue full");
        }
        s_pending=s_result[i].burst;s_has_pending=true;
    } else for(int i=0;i<CATCH_RESULTS;i++) if(s_result[i].burst.id==b->id) {
        s_result[i].burst.stop_us=b->stop_us;s_result[i].burst.rtl_dbfs=b->rtl_dbfs;break;
    }
    unlock();
}
/* Drain without duty sleeps while data is ready. Diagnostics do not own the radio;
   all reads/start/stop still go through its endpoint session. */
static ls_radio_err_t rtl_block(ls_radio_session_t *session)
{
    uint64_t begin=esp_timer_get_time();
    size_t n=0;
    ls_radio_err_t err=ls_radio_iq_read(session,s_iq,sizeof(s_iq),0,&n);
    uint64_t read_end=esp_timer_get_time();
    uint64_t dropped=rtlsdr_stream_dropped()-s_usb_drops;
    uint32_t backlog=rtlsdr_stream_avail();
    if(dropped)catch_detector_gap(&s_detector);
    uint64_t optional_before=s_detector.fft_us+s_detector.spectrum_us;
    bool snapshot=read_end>=s_status_due && read_end>=s_detector.expensive_ready_us;
    if(err==LS_RADIO_OK && n && running()) {
        lock();bool tracing=(uint64_t)esp_timer_get_time()<s_trace_until;unlock();
        s_detector.trace=tracing?trace_candidate:NULL;
        uint64_t end=esp_timer_get_time(),lag=(uint64_t)backlog*500000/CATCH_RATE;
        lock();
        s_status.block_period_us=(uint64_t)n*500000/CATCH_RATE;
        s_status.block_gap_us=s_status.last_block_us?read_end-s_status.last_block_us:0;
        s_status.last_block_us=read_end;
        if(s_status.block_gap_us>s_status.max_block_gap_us)s_status.max_block_gap_us=s_status.block_gap_us;
        s_status.processed_bytes+=n;
        unlock();
        /* Drain backlog first; debt only suppresses optional refinement. */
        s_full_allowance=backlog>sizeof(s_iq) || snapshot?0:1;
        catch_detector_allow(&s_detector,s_full_allowance);
        catch_detector_iq(&s_detector,s_iq,n,end>lag?end-lag:0,detection,NULL);
        lock();s_status.blocks++;unlock();
    }
    s_usb_drops+=dropped;
    lock();
    s_status.usb_drop_bytes+=dropped;
    s_status.lost_samples=s_status.usb_drop_bytes/2;
    s_status.full_allowance=s_full_allowance;
    s_status.detector_drops=s_detector.dropped;
    s_status.fft_frames=s_detector.frames;
    s_status.floor_dbfs=s_detector.reported_floor;
    s_status.threshold_dbfs=s_detector.reported_floor+CATCH_MARGIN_DB;
    s_status.iq_rms=s_detector.iq_rms;s_status.clips=s_detector.clips;
    s_status.coarse_frames=s_detector.coarse_frames;
    s_status.checks_us=s_detector.checks_us;s_status.fft_us=s_detector.fft_us;
    s_status.spectrum_us=s_detector.spectrum_us;
    s_status.read_us+=read_end-begin;
    unlock();
    uint64_t scan_begin=esp_timer_get_time();
    if(snapshot) {
        lock();
        s_status.peak_count=s_detector.peak_count;
        memcpy(s_status.peaks,s_detector.peaks,sizeof(s_status.peaks));
        memcpy(s_status.recent,s_detector.recent,sizeof(s_status.recent));
        memcpy(s_status.reasons,s_detector.reasons,sizeof(s_status.reasons));
        memset(s_status.spurs,0,sizeof(s_status.spurs));
        s_status.spur_count=catch_detector_spurs(&s_detector,s_status.spurs,CATCH_SPUR_GROUPS);
        hide_learned_spurs();
        if(s_has_pending && catch_detector_is_spur(&s_detector,s_pending.frequency,s_pending.rtl_dbfs)) {
            for(int j=0;j<CATCH_RESULTS;j++)if(s_result[j].burst.id==s_pending.id)
                snprintf(s_result[j].decode_status,sizeof(s_result[j].decode_status),"learned spur: no handoff");
            s_has_pending=false;
        }
        unlock();
        uint64_t scan_end=esp_timer_get_time();
        s_status_due=scan_end+100000;
        s_detector.expensive_ready_us=(s_detector.expensive_ready_us>scan_end?
            s_detector.expensive_ready_us:scan_end)+2*(scan_end-scan_begin);
        lock();s_status.scan_us+=scan_end-scan_begin;unlock();
    }
    uint64_t active=esp_timer_get_time()-begin;
    uint64_t optional=s_detector.fft_us+s_detector.spectrum_us-optional_before;
    lock();
    if(n && optional*3>(uint64_t)n*500000/CATCH_RATE)s_status.over_budget++;
    s_status.active_us+=active;
    s_status.elapsed_us=esp_timer_get_time()-s_rtl_epoch;
    /* RTL U8 has no sequence numbers or device overrun counter. Show the
       unexplained rate deficit separately; do not invent an exact loss count.
       Allow the posted transfer pool plus a read block to be in flight. */
    uint64_t expected=s_status.elapsed_us*CATCH_RATE/1000000;
    uint64_t accounted=(s_status.processed_bytes+s_status.usb_drop_bytes+backlog)/2;
    uint64_t in_flight=(15u*16384u+sizeof(s_iq))/2;
    s_status.deficit_samples=expected>accounted+in_flight?expected-accounted-in_flight:0;
    unlock();
    /* The only receive sleep is an empty nonblocking read. */
    if(!n && (err==LS_RADIO_OK || err==LS_RADIO_ERR_TIMEOUT))vTaskDelay(1);
    return err==LS_RADIO_ERR_TIMEOUT?LS_RADIO_OK:err;
}
static uint64_t detector_clock(void *ctx) {(void)ctx;return esp_timer_get_time();}
static void rtl_worker(void *arg)
{
    (void)arg;
    for(;;) {
        ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
        ls_radio_session_t *session=NULL;
        lock();uint32_t center=s_status.center;unlock();
        ls_radio_requirements_t req={.required_caps=LS_RADIO_RX_IQ_U8,
            .min_hz=center-1200000u,.max_hz=center+1200000u,
            .sample_rate_hz=CATCH_RATE,.iq_format=LS_RADIO_IQ_FORMAT_U8_INTERLEAVED,
            .preferred_endpoint_id=LS_RADIO_ENDPOINT_RTL_USB};
        ls_radio_err_t err=ls_radio_acquire("CATCH",&req,&session);
        ls_radio_iq_config_t cfg={.center_hz=center,.sample_rate_hz=CATCH_RATE,
            .bandwidth_hz=CATCH_RATE,.gain_mode=LS_RADIO_GAIN_MANUAL,.gain_tenths_db=400},actual;
        if(err==LS_RADIO_OK) err=ls_radio_iq_configure(session,&cfg,&actual);
        if(err==LS_RADIO_OK && (actual.center_hz!=center || actual.sample_rate_hz!=CATCH_RATE)) err=LS_RADIO_ERR_UNSUPPORTED;
        if(err==LS_RADIO_OK) {
            lock();s_status.actual_center=actual.center_hz;s_status.sample_rate=actual.sample_rate_hz;
            s_status.gain_mode=actual.gain_mode;s_status.gain_tenths_db=actual.gain_tenths_db;unlock();
        }
        if(err==LS_RADIO_OK && running()) err=ls_radio_iq_start(session);
        lock();snprintf(s_status.rtl_status,sizeof(s_status.rtl_status),"RTL: %s",ls_radio_err_name(err));unlock();
        catch_detector_init(&s_detector,center);s_detector.clock_us=detector_clock;
        s_rtl_epoch=esp_timer_get_time();s_usb_drops=rtlsdr_stream_dropped();s_full_allowance=1;s_status_due=s_rtl_epoch;
        while(err==LS_RADIO_OK && running()) err=rtl_block(session);
        if(session) {ls_radio_iq_stop(session);ls_radio_release(session);}
        if(err!=LS_RADIO_OK) {
            lock();snprintf(s_status.rtl_status,sizeof(s_status.rtl_status),"RTL: %s",ls_radio_err_name(err));unlock();
            __atomic_store_n(&s_run,false,__ATOMIC_RELEASE);
        }
        __atomic_store_n(&s_rtl_done,true,__ATOMIC_RELEASE);
    }
}
static uint64_t handoff_now(void *ctx) {(void)ctx;return esp_timer_get_time();}
static void handoff_end(void *ctx)
{
    (void)ctx;
    if(s_mod==CATCH_FSK) ls_lora_fsk_end(); else ls_lora_ook_end();
    if(s_lease) ls_mesh_radio_hold_bg(false);
    lock();
    if(s_slot>=0 && !s_result[s_slot].captures)
        snprintf(s_result[s_slot].decode_status,sizeof(s_result[s_slot].decode_status),"no repeat captured during hold");
    snprintf(s_status.lr_status,sizeof(s_status.lr_status),"waiting for a burst");
    unlock();
    s_lease=false;s_slot=-1;
}
static bool handoff_begin(void *ctx,const catch_burst_t *b)
{
    (void)ctx;
    const char *why=NULL;
    if(ls_lora_chip()!=LS_LORA_CHIP_LR20XX) why="LR2021 absent (SX1262 has no stream)";
    else if(b->frequency<400000000u || b->frequency>1100000000u) why="LR2021 sensitivity unavailable in this band";
    else {
        /* Mesh acknowledges a background lease on its own worker. A single
           request followed by release never gives it a chance to pause. */
        bool held=ls_mesh_radio_hold_bg(true);
        int64_t deadline=esp_timer_get_time()+25000;
        while(!held && !ls_mesh_radio_wanted() && running() && esp_timer_get_time()<deadline) {
            vTaskDelay(1);held=ls_mesh_radio_hold_bg(true);
        }
        if(!held) {ls_mesh_radio_hold_bg(false);why="socket busy: MESH / foreground lease";}
    }
    if(why) {lock();snprintf(s_status.lr_status,sizeof(s_status.lr_status),"%s",why);unlock();return false;}
    s_lease=true;s_mod=b->modulation;
    bool ert=b->frequency>=911000000u && b->frequency<=914000000u && s_mod==CATCH_OOK;
    s_kind=ert?LR433_IN_ERT:s_mod==CATCH_FSK?LR433_IN_FSK:LR433_IN_OOK;
    s_rate=ert?32768:s_mod==CATCH_FSK?(b->frequency>900000000u?68966:76800):16000;
    s_lead=ert?0x2665:s_mod==CATCH_FSK?0x0F0F:7;
    s_lead_bits=ert?14:s_mod==CATCH_FSK?16:4;
    s_dev=b->deviation?b->deviation:35000;
    esp_err_t err;
    if(s_mod==CATCH_FSK) {
        ls_fsk_cfg_t cfg={.freq_hz=b->frequency,.bitrate=s_rate,.deviation_hz=s_dev,
            .bandwidth_hz=ls_lora_fsk_bw_snap(s_rate+2*s_dev+50000),
            .sync_word=0x0F0F0000,.sync_bits=16,.payload_bytes=128};
        err=ls_lora_fsk_begin(&cfg);
    } else {
        ls_ook_cfg_t cfg={.freq_hz=b->frequency,.bitrate=s_rate,
            .bandwidth_hz=ert?2222222:250000,.pattern=ert?0x2999:0x000E,
            .pattern_bits=(uint8_t)s_lead_bits,.frame_bytes=252,.gain_step=0,.boost=-1};
        err=ls_lora_ook_begin(&cfg);
    }
    if(err!=ESP_OK) {ls_mesh_radio_hold_bg(false);s_lease=false;}
    lock();snprintf(s_status.lr_status,sizeof(s_status.lr_status),err==ESP_OK?"LR2021 listening; rate is a guess":"LR2021 session refused: %s",esp_err_to_name(err));unlock();
    return err==ESP_OK;
}
static const catch_handoff_ops_t HANDOFF={handoff_now,handoff_begin,handoff_end,NULL};

/* Run lengths include the consumed detector/sync pattern. Rounded boundaries
   avoid accumulating a microsecond error on every sampled bit. */
static int edges_from_bits(const lr433_capture_t *c)
{
    int n=0,prev=-1,run=0;uint64_t elapsed=0;
    for(int i=0;i<c->lead_bits+c->bits;i++) {
        int bit=i<c->lead_bits?(c->lead>>(c->lead_bits-1-i))&1:
            (c->data[(i-c->lead_bits)/8]>>(7-(i-c->lead_bits)%8))&1;
        if(prev>=0 && bit!=prev) {
            uint64_t end=(uint64_t)(i)*1000000/c->rate;
            if(n==CAP_EDGES) return 0;
            s_pulses[n++]=(prev?1:-1)*(int32_t)(end-elapsed);elapsed=end;run=0;
        }
        prev=bit;run++;
    }
    if(run && n<CAP_EDGES) s_pulses[n++]=(prev?1:-1)*(int32_t)((uint64_t)(c->lead_bits+c->bits)*1000000/c->rate-elapsed);
    return n;
}
static void capture(uint32_t id,uint32_t frequency,float rssi,int bytes)
{
    lr433_capture_t cap={.kind=s_kind,.rate=s_rate,.lead=s_lead,
        .lead_bits=(uint8_t)s_lead_bits,.data=s_bits,.bits=bytes*8};
    int decoded=lr433_decode(&cap,&s_decode,s_messages,8);
    int n=edges_from_bits(&cap);
    lock();
    if(s_slot>=0 && s_result[s_slot].burst.id==id) {
        catch_result_t *r=&s_result[s_slot];r->lr_rssi=rssi;r->captures++;r->edges=n;
        memcpy(s_edges[s_slot],s_pulses,(size_t)n*sizeof(int32_t));
        r->decoded=decoded>0;if(decoded) r->message=s_messages[0];
        snprintf(r->decode_status,sizeof(r->decode_status),decoded?"decoded":"raw: sampled edges");
        s_status.captures++;if(decoded) s_status.decoded++;
    }
    unlock();
    if(n>=6) {
        if(s_mod==CATCH_FSK) {
            rec_fsk_mod_t mod={.bitrate=s_rate,.deviation_hz=s_dev,.bandwidth_khz=250};
            rec_watch_submit_mod(REC_SOURCE_SX1262,frequency,s_pulses,n,isfinite(rssi)?(int)rssi:0,0,&mod);
        } else rec_watch_submit_from(REC_SOURCE_SX1262,frequency,s_pulses,n,isfinite(rssi)?(int)rssi:0,0);
    }
}
static void save_capture(void)
{
    catch_result_t result={0};int n=0;
    lock();int slot=s_save_slot;
    if(slot>=0 && s_result[slot].burst.id==s_save_id) {
        result=s_result[slot];n=result.edges;memcpy(s_pulses,s_edges[slot],(size_t)n*sizeof(int32_t));
    }
    s_save_slot=-1;unlock();
    if(slot<0) return;
    char path[112];bool ok=false;
    snprintf(path,sizeof(path),"/sdcard/lakeshark/rec/catch_%llu_%lu.sub",
        (unsigned long long)result.burst.detected_us,(unsigned long)result.burst.id);
    if(n>0) {
        mkdir("/sdcard/lakeshark",0775);mkdir("/sdcard/lakeshark/rec",0775);
        subghz_file_t file={.freq_hz=result.burst.frequency};
        snprintf(file.preset,sizeof(file.preset),"%s",result.burst.modulation==CATCH_FSK?"FuriHalSubGhzPresetCustom":"FuriHalSubGhzPresetOok650Async");
        if(result.burst.modulation==CATCH_FSK) {file.bitrate=result.burst.frequency>900000000u?68966:76800;file.deviation_hz=result.burst.deviation;}
        char preset_text[512];
        ls_sub_mod_t mod={file.bitrate,file.deviation_hz};
        size_t preset_len=ls_sub_preset_text(&mod,preset_text,sizeof(preset_text));
        if(preset_len && preset_len<sizeof(preset_text))
            ok=subghz_file_save(path,&file,s_pulses,n,preset_text);
    }
    lock();snprintf(s_status.save_status,sizeof(s_status.save_status),"%s",ok?path:"Save failed: no capture, file exists or SD unavailable");unlock();
}
static void lr_worker(void *arg)
{
    (void)arg;
    for(;;) {
        ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
        catch_hold_t hold={0};uint32_t listening_id=0;
        while(running()) {
            bool held_spur=false;
            lock();
            for(unsigned i=0;i<s_status.spur_count && i<CATCH_SPUR_GROUPS;i++)if(
                llabs((long long)s_status.spurs[i].frequency-hold.frequency)<15000 &&
                hold.dbfs<s_status.spurs[i].dbfs+10)held_spur=true;
            unlock();
            if(hold.listening && (held_spur || ls_mesh_radio_wanted() || catch_handoff_expired(&hold,handoff_now(NULL)))) catch_handoff_stop(&hold,&HANDOFF);
            catch_burst_t b;bool pending;
            lock();pending=s_has_pending;b=s_pending;s_has_pending=false;unlock();
            if(pending) {
                uint32_t latency=0;bool same=hold.listening && llabs((long long)hold.frequency-b.frequency)<=15000;
                bool preempt=hold.listening && !same;
                bool accepted=catch_handoff(&hold,&b,&HANDOFF,&latency);
                lock();
                int found=-1;
                for(int i=0;i<CATCH_RESULTS;i++) if(s_result[i].burst.id==b.id) {found=i;break;}
                if(accepted && found>=0) {
                    s_slot=found;listening_id=b.id;s_result[found].latency_us=latency;
                    snprintf(s_result[found].decode_status,sizeof(s_result[found].decode_status),"listening for repeat");
                    if(!same) {s_status.handoffs++;if(preempt)s_status.preemptions++;if(latency>s_status.max_latency_us)s_status.max_latency_us=latency;}
                } else if(found>=0) snprintf(s_result[found].decode_status,sizeof(s_result[found].decode_status),"%s",hold.listening?"LR2021 holding another channel":s_status.lr_status);
                unlock();
            }
            if(hold.listening) {
                float rssi=NAN;
                int n=s_mod==CATCH_FSK?ls_lora_fsk_poll(s_bits,sizeof(s_bits),&rssi):ls_lora_ook_poll(s_bits,sizeof(s_bits),&rssi);
                if(n>0) {
                    /* Raw socket noise cannot prove a repeating source or keep
                       an unproven channel leased forever. RTL duration/frequency
                       evidence controls preference and hold renewal. */
                    capture(listening_id,hold.frequency,rssi,n);
                }
                else if(n<0) {catch_handoff_stop(&hold,&HANDOFF);lock();snprintf(s_status.lr_status,sizeof(s_status.lr_status),"LR2021 receive fault");unlock();}
            }
            save_capture();vTaskDelay(1);
        }
        catch_handoff_stop(&hold,&HANDOFF);
        for(;;) {
            save_capture();lock();bool saved=s_save_slot<0;
            if(saved)__atomic_store_n(&s_lr_done,true,__ATOMIC_RELEASE);
            unlock();if(saved)break;
        }
    }
}
ls_radio_err_t rec_catch_start(uint32_t center)
{
    if(center<25200000u || center>1764800000u) return LS_RADIO_ERR_INVALID;
    bool expected=false;
    if(!__atomic_compare_exchange_n(&s_starting,&expected,true,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return LS_RADIO_ERR_BUSY;
    ls_radio_err_t err=LS_RADIO_OK;
    if(running() || !__atomic_load_n(&s_rtl_done,__ATOMIC_ACQUIRE) || !__atomic_load_n(&s_lr_done,__ATOMIC_ACQUIRE)) err=LS_RADIO_ERR_BUSY;
    if(err==LS_RADIO_OK && !s_lock) s_lock=xSemaphoreCreateMutex();
    if(!s_lock) err=LS_RADIO_ERR_NO_MEMORY;
    if(err==LS_RADIO_OK && !s_rtl_task) {
        StackType_t *stack=heap_caps_malloc(STACK_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(stack) s_rtl_task=xTaskCreateStaticPinnedToCore(rtl_worker,"catch_rtl",STACK_BYTES,NULL,1,stack,&s_rtl_tcb,1);
        if(!s_rtl_task) {heap_caps_free(stack);err=LS_RADIO_ERR_NO_MEMORY;}
    }
    if(err==LS_RADIO_OK && !s_lr_task) {
        StackType_t *stack=heap_caps_malloc(STACK_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(stack) s_lr_task=xTaskCreateStaticPinnedToCore(lr_worker,"catch_lr",STACK_BYTES,NULL,1,stack,&s_lr_tcb,0);
        if(!s_lr_task) {heap_caps_free(stack);err=LS_RADIO_ERR_NO_MEMORY;}
    }
    if(err==LS_RADIO_OK && !s_trace_task) {
        StackType_t *stack=heap_caps_malloc(STACK_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(stack)s_trace_task=xTaskCreateStaticPinnedToCore(trace_worker,"catch_trace",STACK_BYTES,NULL,1,stack,&s_trace_tcb,0);
        if(!s_trace_task) {heap_caps_free(stack);err=LS_RADIO_ERR_NO_MEMORY;}
    }
    if(err==LS_RADIO_OK) {
        lock();memset(&s_status,0,sizeof(s_status));memset(s_result,0,sizeof(s_result));
        memset(&s_sources,0,sizeof(s_sources));
        s_rtl_epoch=esp_timer_get_time();s_status.center=center;s_head=0;s_has_pending=false;s_save_slot=-1;
        s_trace_head=s_trace_count=s_trace_lines=s_trace_suppressed=0;s_trace_until=0;
        snprintf(s_status.lr_status,sizeof(s_status.lr_status),"waiting for a burst");unlock();
        __atomic_store_n(&s_rtl_done,false,__ATOMIC_RELEASE);__atomic_store_n(&s_lr_done,false,__ATOMIC_RELEASE);
        __atomic_store_n(&s_run,true,__ATOMIC_RELEASE);
        xTaskNotifyGive(s_lr_task);xTaskNotifyGive(s_rtl_task);
    }
    __atomic_store_n(&s_starting,false,__ATOMIC_RELEASE);return err;
}
void rec_catch_stop(void)
{
    __atomic_store_n(&s_run,false,__ATOMIC_RELEASE);
    if(s_lock) {lock();s_trace_until=0;s_trace_count=0;unlock();}
}
void rec_catch_status(catch_status_t *out)
{
    memset(out,0,sizeof(*out));if(s_lock){lock();*out=s_status;unlock();}
    out->running=running();out->stopping=!out->running && (!__atomic_load_n(&s_rtl_done,__ATOMIC_ACQUIRE)||!__atomic_load_n(&s_lr_done,__ATOMIC_ACQUIRE));
}
bool rec_catch_result(int index,catch_result_t *out)
{
    if(!s_lock || index<0) return false;
    lock();int slot=result_slot(index);bool ok=slot>=0;if(ok)*out=s_result[slot];unlock();return ok;
}
bool rec_catch_save(int index)
{
    if(!s_lock || !s_lr_task || index<0) return false;
    bool expected=false;
    if(!__atomic_compare_exchange_n(&s_starting,&expected,true,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return false;
    lock();bool ok=index<s_status.count && s_save_slot<0;
    int slot=result_slot(index);
    if(ok) ok=slot>=0 && s_result[slot].edges>0;
    if(ok){s_save_slot=slot;s_save_id=s_result[slot].burst.id;
        if(!running())__atomic_store_n(&s_lr_done,false,__ATOMIC_RELEASE);
    }
    unlock();
    if(ok && !running()) xTaskNotifyGive(s_lr_task);
    __atomic_store_n(&s_starting,false,__ATOMIC_RELEASE);return ok;
}
int rec_catch_command(int argc,char **argv)
{
    if(argc==3 && !strcmp(argv[1],"trace")) {
        char *end;unsigned long seconds=strtoul(argv[2],&end,10);
        if(!argv[2][0] || *end || seconds>300 || !s_lock)return 1;
        lock();s_trace_until=seconds?(uint64_t)esp_timer_get_time()+seconds*1000000ull:0;
        s_trace_head=s_trace_count=s_trace_lines=s_trace_suppressed=0;unlock();
        printf("catch trace %lu seconds (40 lines/s; suppressed lines counted)\n",seconds);return 0;
    }
    if(argc==3 && !strcmp(argv[1],"start")) {
        char *end;double mhz=strtod(argv[2],&end);
        if(*end || !isfinite(mhz) || mhz<25.2 || mhz>1764.8) return 1;
        ls_radio_err_t err=rec_catch_start((uint32_t)(mhz*1e6+0.5));printf("catch: %s\n",ls_radio_err_name(err));return err!=LS_RADIO_OK;
    }
    if(argc==2 && !strcmp(argv[1],"stop")) {rec_catch_stop();return 0;}
    if(argc==2 && !strcmp(argv[1],"list")) {
        /* Indexes shift while reception inserts rows. Freeze the identities,
           then fetch each row separately with short locks and small locals. */
        uint32_t ids[CATCH_RESULTS];int count=0;
        if(s_lock) {
            lock();count=s_status.count;
            for(int i=0;i<count;i++)ids[i]=s_result[result_slot(i)].burst.id;
            unlock();
        }
        catch_result_t r;
        for(int i=0;i<count;i++) {
            bool found=false;lock();
            for(int j=0;j<CATCH_RESULTS;j++)if(s_result[j].burst.id==ids[i] && !s_result[j].burst.spur) {r=s_result[j];found=true;break;}
            unlock();if(!found)continue;
            printf("%llu ms %.6f MHz RTL %.1f dBFS LR %.1f dBm %s %s %s\n",
            (unsigned long long)(r.burst.start_us/1000),r.burst.frequency/1e6,(double)r.burst.rtl_dbfs,(double)r.lr_rssi,
            r.decoded?r.message.model:"raw",r.decoded?r.message.id:"",r.decode_status);
            if(r.decoded) printf("  channel %d battery %d temp %.1f C humidity %.1f %% pressure %.1f kPa wind %.1f m/s rain %.1f mm consumption %lld\n",
                r.message.channel,r.message.battery_ok,(double)r.message.temp_c,(double)r.message.humidity,
                (double)r.message.kpa,(double)r.message.wind_ms,(double)r.message.rain_mm,(long long)r.message.consumption);
        }
        return 0;
    }
    if(argc==2 && !strcmp(argv[1],"stats")) {
        catch_status_t *st=heap_caps_malloc(sizeof(*st),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(!st) {puts("catch: diagnostic snapshot allocation failed");return 1;}
        rec_catch_status(st);
        printf("catch: %s %.6f MHz detected %lu captures %lu decoded %lu dropped %lu handoffs %lu max latency %lu us\n%s; %s\n",
            st->running?"running":st->stopping?"stopping":"stopped",st->center/1e6,(unsigned long)st->detections,
            (unsigned long)st->captures,(unsigned long)st->decoded,(unsigned long)st->dropped,
            (unsigned long)st->handoffs,(unsigned long)st->max_latency_us,st->rtl_status,st->lr_status);
        printf("blocks %lu dropped blocks %lu USB lost bytes %llu detector drops %lu FFT %lu active %llu/%llu us (%.1f%%)\n",
            (unsigned long)st->blocks,(unsigned long)st->block_drops,(unsigned long long)st->usb_drop_bytes,
            (unsigned long)st->detector_drops,(unsigned long)st->fft_frames,
            (unsigned long long)st->active_us,(unsigned long long)st->elapsed_us,
            st->elapsed_us?100.0*(double)st->active_us/st->elapsed_us:0);
        printf("processed %.1f blocks/s %.3f MB/s vs expected %.1f blocks/s %.3f MB/s; block period %llu us max coarse block gap %llu us\n",
            st->elapsed_us?st->blocks*1e6/st->elapsed_us:0,
            st->elapsed_us?(double)st->processed_bytes/st->elapsed_us:0,
            st->processed_bytes?CATCH_RATE*2.0*st->blocks/st->processed_bytes:0,CATCH_RATE*2/1e6,
            (unsigned long long)st->block_period_us,(unsigned long long)st->max_block_gap_us);
        printf("lost samples %llu (%.1f samples/s, transport reported); unaccounted samples %llu (rate deficit estimate; RTL has no sequence counter)\n",
            (unsigned long long)st->lost_samples,st->elapsed_us?st->lost_samples*1e6/st->elapsed_us:0,
            (unsigned long long)st->deficit_samples);
        printf("stages read %llu us checks %llu us (%llu us/check) FFT %llu us (%llu us/frame) spectrum %llu us scan/status %llu us; hard FFT budget %u/s preemptions %lu\n",
            (unsigned long long)st->read_us,(unsigned long long)st->checks_us,
            (unsigned long long)(st->coarse_frames?st->checks_us/st->coarse_frames:0),
            (unsigned long long)st->fft_us,(unsigned long long)(st->fft_frames?st->fft_us/st->fft_frames:0),
            (unsigned long long)st->spectrum_us,(unsigned long long)st->scan_us,
            CATCH_FULL_PER_SECOND,(unsigned long)st->preemptions);
        printf("floor median %.1f dBFS threshold median %.1f dBFS (per-bin +%.1f dB); warm %u ms duration %u..%u ms; quiet %u blocks after %u ms, positive timeout %u ms\n",
            (double)st->floor_dbfs,(double)st->threshold_dbfs,(double)CATCH_MARGIN_DB,
            CATCH_WARM_US/1000,CATCH_MIN_US/1000,CATCH_MAX_US/1000,CATCH_QUIET_BLOCKS,CATCH_MERGE_US/1000,CATCH_OBSERVATION_US/1000);
        printf("handoff warm %u ms repeat tolerance 15000 Hz; DC/image guard %u bins (%.3f kHz)\n",
            CATCH_HANDOFF_WARM_US/1000,CATCH_DC_BINS,(double)CATCH_DC_BINS*CATCH_RATE/CATCH_BINS/1000);
        printf("RTL actual centre %lu Hz rate %lu sps gain %s %.1f dB IQ RMS %.5f clips %llu (sampled)\n",
            (unsigned long)st->actual_center,(unsigned long)st->sample_rate,
            st->gain_mode==LS_RADIO_GAIN_MANUAL?"manual":"auto",st->gain_tenths_db/10.0,
            (double)st->iq_rms,(unsigned long long)st->clips);
        printf("checks %lu full allowance %lu over budget %lu; rejected short %lu long %lu wide %lu margin %lu mask %lu merged %lu\n",
            (unsigned long)st->coarse_frames,(unsigned long)st->full_allowance,(unsigned long)st->over_budget,
            (unsigned long)st->reasons[CATCH_TOO_SHORT],(unsigned long)st->reasons[CATCH_TOO_LONG],
            (unsigned long)st->reasons[CATCH_TOO_WIDE],(unsigned long)st->reasons[CATCH_UNDER_MARGIN],
            (unsigned long)st->reasons[CATCH_MASKED],(unsigned long)st->reasons[CATCH_MERGED]);
        printf("learned spurs %lu suppressed %lu; top",(unsigned long)st->spur_count,(unsigned long)st->reasons[CATCH_SPUR]);
        for(unsigned i=0;i<st->spur_count && i<4;i++)printf(" %lu Hz%s",(unsigned long)st->spurs[i].frequency,st->spurs[i].comb?" comb":"");
        puts("");
        uint64_t now=esp_timer_get_time();int strongest=-1;
        for(unsigned i=0;i<CATCH_RECENT_SECONDS;i++)if(st->recent[i].frequency && now>=st->recent[i].peak_us &&
            now-st->recent[i].peak_us<=CATCH_PEAK_WINDOW_US &&
            (strongest<0 || st->recent[i].snr_db>st->recent[strongest].snr_db))strongest=(int)i;
        if(strongest>=0) {
            catch_peak_t *p=&st->recent[strongest];
            printf("peak last 10 s SNR %.1f dB %lu Hz at %llu ms age %llu ms (%s)\n",
                (double)p->snr_db,(unsigned long)p->frequency,(unsigned long long)(p->peak_us/1000),
                (unsigned long long)((now-p->peak_us)/1000),catch_reason_name(p->reason));
        } else puts("peak last 10 s: none above floor+6 dB");
        heap_caps_free(st);return 0;
    }
    if(argc==2 && !strcmp(argv[1],"spurs")) {
        catch_status_t *st=heap_caps_malloc(sizeof(*st),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(!st)return 1;
        rec_catch_status(st);
        printf("learned spurs %lu (capacity %u; +10 dB override, quiet decay 3 s)\n",(unsigned long)st->spur_count,CATCH_SPUR_GROUPS);
        for(unsigned i=0;i<st->spur_count && i<CATCH_SPUR_GROUPS;i++)
            printf("spur %lu Hz %.1f dBFS %s\n",(unsigned long)st->spurs[i].frequency,(double)st->spurs[i].dbfs,st->spurs[i].comb?"comb family":"recurrent / persistent");
        heap_caps_free(st);return 0;
    }
    if(argc==2 && !strcmp(argv[1],"peaks")) {
        catch_status_t *st=heap_caps_malloc(sizeof(*st),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(!st) {puts("catch: diagnostic snapshot allocation failed");return 1;}
        rec_catch_status(st);
        /* A snapshot and local index sort keep console locks short. */
        unsigned order[CATCH_PEAKS];
        for(unsigned i=0;i<st->peak_count;i++)order[i]=i;
        for(unsigned i=0;i<st->peak_count;i++)for(unsigned j=i+1;j<st->peak_count;j++)
            if(st->peaks[order[j]].snr_db>st->peaks[order[i]].snr_db) {
                unsigned t=order[i];order[i]=order[j];order[j]=t;
            }
        for(unsigned i=0;i<st->peak_count;i++) {
            catch_peak_t *p=&st->peaks[order[i]];
            printf("peak %llu..%llu ms %lu Hz SNR %.1f dB %.1f dBFS %s\n",
                (unsigned long long)(p->first_us/1000),(unsigned long long)(p->last_us/1000),
                (unsigned long)p->frequency,(double)p->snr_db,(double)p->dbfs,catch_reason_name(p->reason));
        }
        heap_caps_free(st);return 0;
    }
    puts("catch start <MHz> | stop | list | stats | peaks | spurs | trace <0..300 seconds>");return 1;
}
