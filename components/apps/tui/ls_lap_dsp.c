/* API 2.6 services: private, preemptible native DSP computation. No SDK/API
 * context is granted to the callback. Only lap_io joins/deletes its worker;
 * ELF cleanup runs later on that same task, never under executing code. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "ls_lap_dsp.h"
#include "ls_lap_features.h"
#include "ls_lap_usage.h"
#if LAP_WORKER_DSP
#include "ls_lap_service.h"
#include "esp_timer.h"
#include <stdatomic.h>
#include <string.h>
#include <stdlib.h>
#if defined(ESP_PLATFORM) && !defined(LS_LAP_HOST_TEST)
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#define DSP_DEVICE 1
#elif defined(_WIN32)
#include <windows.h>
#define DSP_DEVICE 0
#else
#include <pthread.h>
#include <time.h>
#define DSP_DEVICE 0
#endif
#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR
#endif
typedef struct {
    lap_iq_meta2_t meta;
    unsigned pairs;
    int16_t iq[4096];
} dsp_block;
typedef struct {
    atomic_int id, channel;
    atomic_bool cancel, running, gap;
    atomic_uint head, tail, oh, ot, drops, blocks, overruns, last, worst, state;
    atomic_int error;
    lap_dsp_config_t config;
    dsp_block input[2], current;
    struct { lap_dsp_result_t meta; uint8_t bytes[LAP_DSP_OUTPUT_MAX]; } output[8];
    uint8_t result[LAP_DSP_OUTPUT_MAX];
    int result_bytes;
#if DSP_DEVICE
    SemaphoreHandle_t done;
#endif
} dsp_context;
static EXT_RAM_BSS_ATTR dsp_context contexts[LS_LAP_CONTEXTS];
static atomic_int serial;
static _Thread_local bool computing;
#include "ls_lap_usage.h"
ls_lap_usage_t ls_lap_dsp_usage(unsigned slot) {
    return (ls_lap_usage_t){sizeof(contexts[slot]),atomic_load(&contexts[slot].state)==LAP_DSP_RUNNING};
}
bool ls_lap_dsp_on_worker(void) { return computing; }
static dsp_context *ctx(void) { return &contexts[ls_lap_context_index()]; }
#if !DSP_DEVICE
static unsigned sim_costs[LS_LAP_CONTEXTS];
#define sim_cost sim_costs[ls_lap_context_index()]
static bool sim_failures[LS_LAP_CONTEXTS];
bool ls_lap_dsp_script(const char *c) {
    if(!strncmp(c,"dspfail=",8)) { sim_failures[ls_lap_context_index()]=atoi(c+8)!=0; return true; }
    if(!strncmp(c,"dspook=",7)) { extern int ls_lap_radio_sim_ook; ls_lap_radio_sim_ook=atoi(c+7); return true; }
    if(strncmp(c,"dspcost=",8)) return false;
    sim_cost=(unsigned)strtoul(c+8,NULL,10); return true;
}
#else
bool ls_lap_dsp_script(const char *c) { (void)c; return false; }
#endif
int ls_lap_dsp_attach(int channel, const lap_dsp_config_t *config) {
    lap_dsp_config_t c;
    if(!ls_lap_copy_input(&c,config,sizeof(c)) || c.size!=sizeof(c) || c.version!=1 ||
       !ls_lap_code_ok((const void *)c.process) || c.state_bytes>65536 ||
       (c.state_bytes&&!ls_lap_api_owns(c.state,c.state_bytes))) return LAP_RX_EINVAL;
    if(!c.budget_us) c.budget_us=2000;
    if(c.budget_us<1000 || c.budget_us>10000) return LAP_RX_ERANGE;
    dsp_context *d=ctx();
    if(atomic_load(&d->running) || atomic_load(&d->state)==LAP_DSP_RUNNING) return LAP_RX_EBUSY;
    /* The previous callback has joined before state leaves RUNNING. */
    d->config=c;
    atomic_store(&d->cancel,false); atomic_store(&d->head,0); atomic_store(&d->tail,0);
    atomic_store(&d->gap,true);
    atomic_store(&d->oh,0); atomic_store(&d->ot,0); atomic_store(&d->drops,0); atomic_store(&d->blocks,0);
    atomic_store(&d->overruns,0); atomic_store(&d->last,0); atomic_store(&d->worst,0);
    atomic_store(&d->error,0);
    int ticket=atomic_fetch_add(&serial,1)+1;
    if(ticket<=0) return LAP_RX_ENOMEM;
    atomic_store(&d->channel,channel); atomic_store(&d->id,ticket);
    atomic_store(&d->state,LAP_DSP_RUNNING); return ticket;
}
void ls_lap_dsp_submit(int channel,const int16_t *iq,unsigned n,const lap_iq_meta2_t *m) {
    dsp_context *d=ctx();
    if(atomic_load(&d->channel)!=channel || atomic_load(&d->state)!=LAP_DSP_RUNNING || atomic_load(&d->cancel)) return;
    unsigned h=atomic_load(&d->head);
    if(n>2048 || h-atomic_load_explicit(&d->tail,memory_order_acquire)>=2) { atomic_fetch_add(&d->drops,1); atomic_store(&d->gap,true); return; }
    dsp_block *b=&d->input[h%2]; b->meta=*m; b->pairs=n; memcpy(b->iq,iq,n*4);
    if(atomic_exchange(&d->gap,false)) b->meta.flags|=LAP_IQ_GAP;
    atomic_store_explicit(&d->head,h+1,memory_order_release);
}
void ls_lap_dsp_end(void) { atomic_store(&ctx()->cancel,true); }
void ls_lap_dsp_channel_close(int channel) { if(atomic_load(&ctx()->channel)==channel) ls_lap_dsp_end(); }
void ls_lap_dsp_gap(int channel) {
    dsp_context *d=ctx();
    if(atomic_load(&d->channel)==channel&&atomic_load(&d->state)==LAP_DSP_RUNNING) { atomic_fetch_add(&d->drops,1); atomic_store(&d->gap,true); }
}
static bool valid(int h) { return ls_lap_api_inside() && h>0 && atomic_load(&ctx()->id)==h; }
int lap_rx_dsp_status(int h,lap_dsp_status_t *out) {
    if(!valid(h)||!ls_lap_api_readable(out,sizeof(*out))) return LAP_RX_EINVAL;
    dsp_context *d=ctx(); *out=(lap_dsp_status_t){sizeof(*out),1,atomic_load(&d->state),atomic_load(&d->blocks),atomic_load(&d->drops),atomic_load(&d->overruns),atomic_load(&d->last),atomic_load(&d->worst),atomic_load(&d->head)-atomic_load(&d->tail),atomic_load(&d->error)};
    return 0;
}
int lap_rx_dsp_next(int h,lap_dsp_result_t *out,void *data,uint32_t cap) {
    if(!valid(h)||!ls_lap_api_readable(out,sizeof(*out))||cap>LAP_DSP_OUTPUT_MAX||(cap&&!ls_lap_api_readable(data,cap))) return LAP_RX_EINVAL;
    dsp_context *d=ctx(); unsigned t=atomic_load(&d->ot);
    if(t==atomic_load_explicit(&d->oh,memory_order_acquire)) return 0;
    unsigned n=d->output[t%8].meta.bytes;
    if(n>cap) return LAP_RX_ERANGE;
    *out=d->output[t%8].meta; out->dropped=atomic_load(&d->drops);
    if(n) memcpy(data,d->output[t%8].bytes,n);
    atomic_store_explicit(&d->ot,t+1,memory_order_release); return 1;
}
void lap_rx_dsp_close(int h) { if(valid(h)) ls_lap_dsp_end(); }
static void compute(dsp_context *d) {
    computing=true;
    d->result_bytes=d->config.process(d->current.iq,d->current.pairs,&d->current.meta,d->config.state,d->result,sizeof(d->result));
    computing=false;
}
#if !DSP_DEVICE
/* The host is not a P4 and is descheduled at will: its timeout catches a
   process that never returns, with room for the host, and the scripted
   dspcost (ls_lap_dsp_script) decides overruns. */
#define HOST_SLACK_US 50000u
#endif
#if DSP_DEVICE
static void task(void *arg) { dsp_context *d=arg; compute(d); xSemaphoreGive(d->done); vTaskSuspend(NULL); }
static int execute(dsp_context *d) {
    /* Kernel queue/list storage must remain in internal RAM, including when
       a flash operation disables the PSRAM cache during a timeout tick. */
    d->done=xSemaphoreCreateBinary();
    if(!d->done) return LAP_RX_ENOMEM;
    TaskHandle_t worker=NULL;
    /* Same core as lap_io: deleting a preempted private computation never
       races a callback executing on the other core. No SDK locks are held. */
    if(xTaskCreatePinnedToCoreWithCaps(task,"lap_dsp",6144,d,1,&worker,0,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)!=pdPASS) {
        vSemaphoreDelete(d->done); d->done=NULL; return LAP_RX_ENOMEM;
    }
    TickType_t ticks=pdMS_TO_TICKS((d->config.budget_us+999)/1000);
    bool ok=xSemaphoreTake(d->done,ticks?ticks:1)==pdTRUE;
    vTaskDeleteWithCaps(worker); vSemaphoreDelete(d->done); d->done=NULL; return ok;
}
#elif defined(_WIN32)
static DWORD WINAPI task(void *arg) { compute(arg); return 0; }
static int execute(dsp_context *d) {
    HANDLE worker=CreateThread(NULL,65536,task,d,0,NULL);
    if(!worker) return LAP_RX_ENOMEM;
    bool ok=WaitForSingleObject(worker,(d->config.budget_us+HOST_SLACK_US+999)/1000)==WAIT_OBJECT_0;
    if(!ok) { TerminateThread(worker,1); WaitForSingleObject(worker,INFINITE); }
    CloseHandle(worker); return ok;
}
#else
static void *task(void *arg) { pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS,NULL); compute(arg); return NULL; }
static int execute(dsp_context *d) {
    pthread_t worker; if(pthread_create(&worker,NULL,task,d)) return LAP_RX_ENOMEM;
    struct timespec until; clock_gettime(CLOCK_REALTIME,&until);
    until.tv_nsec+=(long)(d->config.budget_us+HOST_SLACK_US)*1000;
    until.tv_sec+=until.tv_nsec/1000000000; until.tv_nsec%=1000000000;
    bool ok=pthread_timedjoin_np(worker,NULL,&until)==0;
    if(!ok) { pthread_cancel(worker); pthread_join(worker,NULL); } return ok;
}
#endif
bool ls_lap_dsp_service(void) {
    dsp_context *d=ctx();
    if(atomic_load(&d->state)!=LAP_DSP_RUNNING) return false;
    if(atomic_load(&d->cancel)) {
        atomic_fetch_add(&d->drops,atomic_load(&d->head)-atomic_load(&d->tail));
        atomic_store(&d->tail,atomic_load(&d->head)); atomic_store(&d->state,LAP_DSP_STOPPED); return false;
    }
    unsigned t=atomic_load(&d->tail);
    if(t==atomic_load_explicit(&d->head,memory_order_acquire)) return true;
    d->current=d->input[t%2]; atomic_store_explicit(&d->tail,t+1,memory_order_release);
    atomic_store(&d->running,true);
    int64_t start=esp_timer_get_time(); int completion;
#if !DSP_DEVICE
    completion=sim_failures[ls_lap_context_index()]?LAP_RX_ENOMEM:sim_cost<=d->config.budget_us?execute(d):0;
#else
    completion=execute(d);
#endif
    uint32_t used=(uint32_t)(esp_timer_get_time()-start);
#if !DSP_DEVICE
    const uint32_t charged=sim_cost;
    if(sim_cost>used) used=sim_cost;
#else
    const uint32_t charged=used;
#endif
    atomic_store(&d->last,used); if(used>atomic_load(&d->worst)) atomic_store(&d->worst,used);
    atomic_store(&d->running,false);
    if(completion<0) {
        atomic_store(&d->error,completion); atomic_store(&d->state,LAP_DSP_FAILED);
        atomic_fetch_add(&d->drops,1+atomic_load(&d->head)-atomic_load(&d->tail)); atomic_store(&d->tail,atomic_load(&d->head)); return false;
    }
    if(!completion || charged>d->config.budget_us) {
        atomic_fetch_add(&d->overruns,1); atomic_fetch_add(&d->drops,1+atomic_load(&d->head)-atomic_load(&d->tail));
        atomic_store(&d->tail,atomic_load(&d->head)); atomic_store(&d->state,LAP_DSP_OVERRUN); return false;
    }
    atomic_fetch_add(&d->blocks,1);
    int n=d->result_bytes;
    if(n>0 && n<=(int)sizeof(d->result) && !atomic_load(&d->cancel)) {
        unsigned h=atomic_load(&d->oh);
        if(h-atomic_load_explicit(&d->ot,memory_order_acquire)<8) {
            d->output[h%8].meta=(lap_dsp_result_t){sizeof(lap_dsp_result_t),1,(unsigned)n,0,d->current.meta};
            memcpy(d->output[h%8].bytes,d->result,n); atomic_store_explicit(&d->oh,h+1,memory_order_release);
        } else atomic_fetch_add(&d->drops,1);
    } else if(n<0 || n>(int)sizeof(d->result)) atomic_fetch_add(&d->drops,1);
    return true;
}
#else
ls_lap_usage_t ls_lap_dsp_usage(unsigned slot) { (void)slot; return (ls_lap_usage_t){0}; }
bool ls_lap_dsp_on_worker(void) { return false; }
bool ls_lap_dsp_script(const char *command) { (void)command; return false; }
int ls_lap_dsp_attach(int channel, const lap_dsp_config_t *config) { return LAP_RX_EUNSUPPORTED; }
void ls_lap_dsp_submit(int channel, const int16_t *iq, unsigned n, const lap_iq_meta2_t *meta) {}
void ls_lap_dsp_end(void) {}
void ls_lap_dsp_channel_close(int channel) {}
void ls_lap_dsp_gap(int channel) {}
int lap_rx_dsp_status(int h, lap_dsp_status_t *out) { return LAP_RX_EUNSUPPORTED; }
int lap_rx_dsp_next(int h, lap_dsp_result_t *out, void *data, uint32_t cap) { return LAP_RX_EUNSUPPORTED; }
void lap_rx_dsp_close(int h) {}
bool ls_lap_dsp_service(void) { return false; }
#endif
