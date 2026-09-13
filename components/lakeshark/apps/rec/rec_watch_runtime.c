#include "rec_watch.h"
#include "rec_state.h"
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
    uint32_t hz; int edges, peak, reason; uint64_t ms;
    int32_t pulse[REC_WATCH_EDGES];
} capture_t;
static rec_watch_catalog_t *s_catalog;
static QueueHandle_t s_free, s_pending;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static EXT_RAM_BSS_ATTR rec_watch_status_t s_status;
static uint32_t s_pin_id;
static uint32_t s_export_id;
static bool s_pin_value, s_starting;
static uint64_t s_last_submit;
static uint32_t s_boot;
static const char *DIR="/sdcard/subghz";

__attribute__((weak)) bool rec_watch_notify(const char *peer, const char *text)
{ (void)peer; (void)text; return false; }

static void publish(void)
{
    portENTER_CRITICAL(&s_lock);
    s_status.count=0;
    for(int i=0;i<REC_WATCH_SLOTS;i++) if(s_catalog->record[i].event.id) {
        int n=s_status.count++;
        s_status.event[n]=s_catalog->record[i].event;
        memcpy(s_status.preview[n],s_catalog->record[i].pulse,sizeof(s_status.preview[n]));
    }
    s_status.ready=true;
    portEXIT_CRITICAL(&s_lock);
}
static void worker(void *arg)
{
    (void)arg;
    bool restored=rec_watch_restore(DIR,s_catalog);
    portENTER_CRITICAL(&s_lock);
    snprintf(s_status.storage,sizeof(s_status.storage),"%s",restored?"SD archive restored":"RAM; awaiting SD checkpoint");
    portEXIT_CRITICAL(&s_lock);
    publish();
    uint64_t saved=s_catalog->sequence, checkpoint=0, last_alert=0;
    bool attempted_alert=false;
    for(;;) {
        capture_t *cap=NULL;
        if(xQueueReceive(s_pending,&cap,pdMS_TO_TICKS(100))==pdTRUE) {
            bool novel=false;
            int slot=rec_watch_observe(s_catalog,cap->hz,cap->pulse,cap->edges,
                s_boot,cap->ms,cap->peak,cap->reason,&novel);
            uint64_t now=(uint64_t)esp_timer_get_time()/1000;
            char peer[17];
            portENTER_CRITICAL(&s_lock);
            bool enabled=s_status.enabled;
            memcpy(peer,s_status.peer,sizeof(peer));
            if(slot<0)s_status.dropped++; else s_status.received++;
            portEXIT_CRITICAL(&s_lock);
            if(novel && enabled && peer[0]) {
                if((!attempted_alert || now-last_alert>=60000) && now-cap->ms<5000) {
                    char text[80];
                    snprintf(text,sizeof(text),"SubGHz pattern #%lu %.4fMHz %d edges",
                        (unsigned long)s_catalog->record[slot].event.id,cap->hz/1e6,cap->edges);
                    last_alert=now;attempted_alert=true;
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
        portENTER_CRITICAL(&s_lock);
        uint32_t pin=s_pin_id;bool value=s_pin_value;s_pin_id=0;
        uint32_t export_id=s_export_id;s_export_id=0;
        bool enabled=s_status.enabled;
        portEXIT_CRITICAL(&s_lock);
        if(pin) { rec_watch_pin(s_catalog,pin,value);publish(); }
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
        if(s_catalog->sequence!=saved && now-checkpoint >= (enabled?30000:3000)) {
            checkpoint=now;
            uint64_t total=0,free_bytes=UINT64_MAX;
            bool ok=esp_vfs_fat_info("/sdcard",&total,&free_bytes)==ESP_OK;
            if(ok) { mkdir(DIR,0775);ok=rec_watch_store(DIR,s_catalog,free_bytes); }
            if(ok)saved=s_catalog->sequence;
            portENTER_CRITICAL(&s_lock);
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
    capture_t *pool=heap_caps_calloc(2,sizeof(*pool),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    s_free=xQueueCreate(2,sizeof(capture_t*));s_pending=xQueueCreate(2,sizeof(capture_t*));
    bool ok=s_catalog && pool && s_free && s_pending;
    if(ok) {
        for(int i=0;i<2;i++) {capture_t *p=&pool[i];xQueueSend(s_free,&p,0);}
        s_boot=esp_random();
        ok=xTaskCreatePinnedToCoreWithCaps(worker,"subghz_io",6144,NULL,1,NULL,0,
            MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)==pdPASS;
    }
    if(!ok) {
        if(s_free)vQueueDelete(s_free);
        if(s_pending)vQueueDelete(s_pending);
        s_free=s_pending=NULL;heap_caps_free(pool);heap_caps_free(s_catalog);s_catalog=NULL;
        portENTER_CRITICAL(&s_lock);
        s_starting=false;
        snprintf(s_status.storage,sizeof(s_status.storage),"Monitor allocation failed");
        portEXIT_CRITICAL(&s_lock);
    }
    return ok;
}
bool rec_watch_enable(bool on)
{
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
void rec_watch_snapshot(rec_watch_status_t *out)
{
    if(!out)return;
    portENTER_CRITICAL(&s_lock);*out=s_status;portEXIT_CRITICAL(&s_lock);
}
void rec_watch_submit(uint32_t hz,const int32_t *pulse,int edges,int peak,int reason)
{
    if(!rec_watch_enabled() || !pulse || edges<6 || edges>REC_WATCH_EDGES)return;
    uint64_t now=(uint64_t)esp_timer_get_time()/1000;
    capture_t *cap=NULL;
    if(now-s_last_submit<250 || xQueueReceive(s_free,&cap,0)!=pdTRUE) {
        portENTER_CRITICAL(&s_lock);s_status.dropped++;portEXIT_CRITICAL(&s_lock);return;
    }
    s_last_submit=now;
    cap->hz=hz;cap->edges=edges;cap->peak=peak;cap->reason=reason;cap->ms=now;
    memcpy(cap->pulse,pulse,(size_t)edges*sizeof(*pulse));
    xQueueSend(s_pending,&cap,0);
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
