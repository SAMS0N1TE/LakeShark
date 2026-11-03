/* API 2.6 HackRF TX / desktop host. Kept outside desk/ so non-panel builds
 * also export the service and negotiate absence with older viewers. */
#include "desk/ls_desk_host.h"
#include "ls_vt.h"
#include "esp_timer.h"
#include "../../../app-sdk/include/lakeshark_app.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif
#if defined(ESP_PLATFORM) && !defined(LS_LAP_HOST_TEST)
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
static StaticSemaphore_t mutex_storage;
static SemaphoreHandle_t mutex;
static atomic_int initialized;
static void take(void) {
    if(atomic_load_explicit(&initialized,memory_order_acquire)!=2) {
        int expected=0;
        if(atomic_compare_exchange_strong(&initialized,&expected,1)) {
            mutex=xSemaphoreCreateMutexStatic(&mutex_storage);
            atomic_store_explicit(&initialized,2,memory_order_release);
        } else while(atomic_load_explicit(&initialized,memory_order_acquire)!=2) vTaskDelay(1);
    }
    xSemaphoreTake(mutex,portMAX_DELAY);
}
static void give(void) { xSemaphoreGive(mutex); }
#else
static atomic_flag lock=ATOMIC_FLAG_INIT;
static void take(void) { while (atomic_flag_test_and_set_explicit(&lock,memory_order_acquire)) {} }
static void give(void) { atomic_flag_clear_explicit(&lock,memory_order_release); }
#endif
static int cancel_notice;
static uint32_t features, sequence;
static unsigned scale_milli;
static EXT_RAM_BSS_ATTR struct {
    int id, op, result;
    char name[256];
    uint8_t data[LS_DESK_HOST_MAX];
    size_t length, offset;
    int64_t deadline;
    int ready, sent;
} request;
void ls_desk_host_connect(uint32_t f, unsigned scale) {
    take();
    if (request.id&&!request.result) request.result=LAP_SYS_ECANCEL;
    cancel_notice=0; features=f&3u; scale_milli=scale>=250&&scale<=8000?scale:0;
    give();
}
void ls_desk_host_update(uint32_t f, unsigned scale) { take(); features=f&3u; scale_milli=scale>=250&&scale<=8000?scale:0; give(); }
void ls_desk_host_scale(unsigned scale) { take(); scale_milli=scale>=250&&scale<=8000?scale:0; give(); }
void ls_desk_host_disconnect(void) { ls_desk_host_connect(0,0); }
uint32_t ls_desk_host_features(unsigned *scale) {
    take(); uint32_t f=features; if (scale) *scale=scale_milli; give(); return f;
}
int ls_desk_host_begin(int op, const char *name) {
    take(); int e=0;
    if (!(features&(unsigned)op)) e=LAP_SYS_EUNSUPPORTED;
    else if (request.id) e=LAP_SYS_EBUSY;
    else if (!name||strlen(name)>=sizeof(request.name)) e=-1;
    if (!e) {
        memset(&request,0,sizeof(request));
        sequence=sequence%0x07ffffff+1;
        request.id=(int)sequence; request.op=op;
        strcpy(request.name,name); request.ready=op==LS_DESK_URL;
        request.deadline=esp_timer_get_time()+120000000;
        e=request.id;
    }
    give(); return e;
}
int ls_desk_host_payload(int id, const void *bytes, size_t n) {
    take(); int e=LAP_SYS_ECANCEL;
    if (request.id==id&&!request.result) {
        if (n>LS_DESK_HOST_MAX) e=LAP_SYS_ELIMIT;
        else { if(n) memcpy(request.data,bytes,n); request.length=n; request.ready=1; e=0; }
    }
    give(); return e;
}
int ls_desk_host_status(int id, int consume) {
    take(); int e=-1;
    if (id>0&&request.id==id) {
        if (!request.result&&esp_timer_get_time()>request.deadline) request.result=LAP_SYS_ECANCEL;
        e=request.result;
        if (e&&consume) { if(e<0&&request.sent) cancel_notice=request.id; memset(&request,0,sizeof(request)); }
    }
    give(); return e;
}
void ls_desk_host_reply(int id, int result) {
    take();
    if (id==request.id&&!request.result&&result) request.result=result==1?1:result<0?result:-8;
    give();
}
void ls_desk_host_cancel(int id) { take(); if(id==request.id) { request.result=LAP_SYS_ECANCEL; cancel_notice=id; } give(); }
int ls_desk_host_line(char *line, size_t cap) {
    take(); int n=0;
    if(cancel_notice) {
        n=snprintf(line,cap,"term: deskcancel %d",cancel_notice);
        if(n>0&&(size_t)n<cap) cancel_notice=0; else n=0;
        give(); return n;
    }
    if (request.id&&!request.result&&request.ready&&!request.sent) {
        size_t bytes=request.length-request.offset; if(bytes>96) bytes=96;
        char name[513], data[193];
        ls_vt_hex_encode((const uint8_t *)request.name,strlen(request.name),name,sizeof(name));
        ls_vt_hex_encode(request.data+request.offset,bytes,data,sizeof(data));
        n=snprintf(line,cap,"term: desk1 %d %d %u %u %s %s",request.id,request.op,
                   (unsigned)request.offset,(unsigned)request.length,name,bytes?data:"-");
        if(n>0&&(size_t)n<cap) { request.offset+=bytes; request.sent=request.offset==request.length; }
        else n=0;
    }
    give(); return n;
}
