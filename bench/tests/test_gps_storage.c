/* Production recorder and ring I/O with one-shot stdio faults. */
#include "ls_test.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#include <direct.h>
#else
#include <unistd.h>
#endif
#include <stdlib.h>
#include "freertos/task.h"
#include "ls_gps.h"
#include "ls_nvs_safe.h"
#include "nvs.h"
static int close_fault, flush_fault, write_fault, sync_fault;
static int sync_io(int fd)
{
    if(sync_fault && !--sync_fault) return -1;
#ifdef _WIN32
    return _commit(fd);
#else
    return fsync(fd);
#endif
}
static int close_io(FILE *f)
{
    int rc=fclose(f);
    if(close_fault && !--close_fault) return EOF;
    return rc;
}
static int flush_io(FILE *f)
{
    if(flush_fault && !--flush_fault) return EOF;
    return fflush(f);
}
static size_t write_io(const void *p,size_t s,size_t n,FILE *f)
{
    if(write_fault && !--write_fault) return 0;
    return fwrite(p,s,n,f);
}
static int mkdir_io(const char *p,int mode)
{
#ifdef _WIN32
    (void)mode;return _mkdir(p);
#else
    return mkdir(p,mode);
#endif
}
static int access_io(const char *p)
{
#ifdef _WIN32
    return _access(p,0);
#else
    return access(p,0);
#endif
}
#define _commit sync_io
#define fsync sync_io
#define fclose close_io
#define fflush flush_io
#define fwrite write_io
#define mkdir mkdir_io
#define TRACK_PATH "gps-storage/track.log"
#define TRACK_DIR "gps-storage"
#include "../../components/lakeshark/core/ls_rlog.c"
#include "../../components/lakeshark/core/ls_track_log.c"
#undef _commit
#undef fsync
#undef fclose
#undef fflush
#undef fwrite
#undef mkdir
static uint8_t saved_intent;
static bool nvs_dispatched;
static int starts;
static bool feed_fix;
static esp_err_t nvs_error;
esp_err_t ls_nvs_run(ls_nvs_fn_t fn,void *ctx,unsigned stack)
{ (void)stack;nvs_dispatched=true;esp_err_t e=fn(ctx);nvs_dispatched=false;return e; }
esp_err_t nvs_open(const char *name,int mode,nvs_handle_t *h)
{ LS_CHECK(nvs_dispatched);LS_CHECK(!strcmp(name,"ls_track"));(void)mode;*h=1;return nvs_error; }
esp_err_t nvs_get_u8(nvs_handle_t h,const char *k,uint8_t *v)
{ (void)h;(void)k;*v=saved_intent;return ESP_OK; }
esp_err_t nvs_set_u8(nvs_handle_t h,const char *k,uint8_t v)
{ (void)h;(void)k;saved_intent=v;return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h;return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }
bool ls_gps_running(void) { return true; }
esp_err_t ls_gps_start(void) { return ESP_OK; }
void ls_gps_get(ls_gps_state_t *g)
{
    memset(g,0,sizeof(*g));
    if(feed_fix) {
        g->fix=true;g->lat_deg=43;g->lon_deg=-71;g->last_fix_us=esp_timer_get_time();
        s_stop=true;
    }
}
void ls_task_retire_self(void) { abort(); }
BaseType_t xTaskCreate(TaskFunction_t fn,const char *name,uint32_t stack,void *arg,UBaseType_t prio,TaskHandle_t *out)
{ (void)fn;(void)name;(void)stack;(void)arg;(void)prio;starts++;*out=(TaskHandle_t)1;return pdPASS; }
static void clean(void)
{
    close_fault=flush_fault=write_fault=sync_fault=0;
    remove(TRACK_PATH);remove(TRACK_PATH ".undo");remove("gps-storage/out.gpx");remove("gps-storage/out.gpx.part");
    memset(&s_log,0,sizeof(s_log));s_task=NULL;save_time(NULL);
    saved_intent=0;nvs_error=ESP_OK;starts=0;feed_fix=false;s_requested=false;
    mkdir_io(TRACK_DIR,0777);
}
LS_CASE(recording_intent_survives_restart_and_explicit_stop)
{
    clean();LS_EQ_INT(ESP_OK,ls_track_rec_start());LS_EQ_INT(1,saved_intent);LS_CHECK(ls_track_rec_requested());
    s_task=NULL;ls_track_restore(true);LS_EQ_INT(2,starts);
    s_task=NULL;ls_track_rec_stop();LS_EQ_INT(0,saved_intent);LS_CHECK(!ls_track_rec_requested());
    ls_track_restore(true);LS_EQ_INT(2,starts);
    saved_intent=1;ls_track_restore(false);
    LS_EQ_INT(ESP_ERR_NOT_FOUND,ls_track_rec_error());LS_EQ_INT(1,saved_intent);
    LS_CHECK(ls_track_rec_requested());
    ls_track_rec_stop();LS_CHECK(!ls_track_rec_requested());
    saved_intent=1;ls_track_restore(true);LS_EQ_INT(3,starts);
    s_task=NULL;nvs_error=ESP_FAIL;
    LS_EQ_INT(ESP_FAIL,ls_track_rec_start());LS_EQ_INT(3,starts);
    clean();
}
LS_CASE(final_append_close_failure_restores_a_full_ring)
{
    clean();LS_CHECK(ls_rlog_open(&s_log,TRACK_PATH,sizeof(ls_track_pt_t),1));
    ls_track_pt_t a={.t=1},b={.t=2},out;
    LS_CHECK(ls_rlog_append(&s_log,&a));
    /* Undo closes journal and input, then append closes output. */
    close_fault=3;LS_CHECK(!ls_rlog_append(&s_log,&b));
    LS_EQ_INT(1,s_log.count);LS_EQ_INT(0,s_log.head);
    LS_EQ_INT(1,ls_rlog_read_at(&s_log,0,&out));LS_EQ_INT(1,out.t);
    ls_rlog_close(&s_log);LS_CHECK(ls_rlog_open(&s_log,TRACK_PATH,sizeof(a),1));
    LS_EQ_INT(1,ls_rlog_read_at(&s_log,0,&out));LS_EQ_INT(1,out.t);
    clean();
}
LS_CASE(append_flush_and_header_failure_do_not_advance_metadata)
{
    clean();LS_CHECK(ls_rlog_open(&s_log,TRACK_PATH,sizeof(ls_track_pt_t),4));
    ls_track_pt_t a={.t=3};
    flush_fault=2;LS_CHECK(!ls_rlog_append(&s_log,&a));LS_EQ_INT(0,s_log.count);
    sync_fault=2;LS_CHECK(!ls_rlog_append(&s_log,&a));LS_EQ_INT(0,s_log.count);
    write_fault=4;LS_CHECK(!ls_rlog_append(&s_log,&a));LS_EQ_INT(0,s_log.count);
    LS_CHECK(ls_rlog_append(&s_log,&a));LS_EQ_INT(1,s_log.count);clean();
}
LS_CASE(unfinished_append_is_rolled_back_on_open)
{
    clean();LS_CHECK(ls_rlog_open(&s_log,TRACK_PATH,sizeof(ls_track_pt_t),1));
    ls_track_pt_t a={.t=7},b={.t=8},out;
    LS_CHECK(ls_rlog_append(&s_log,&a));LS_CHECK(undo(&s_log,true));
    FILE *f=fopen(TRACK_PATH,"r+b");fseek(f,sizeof(ls_rlog_header_t),SEEK_SET);
    fwrite(&b,sizeof(b),1,f);fclose(f);
    ls_rlog_close(&s_log);LS_CHECK(ls_rlog_open(&s_log,TRACK_PATH,sizeof(a),1));
    LS_EQ_INT(1,ls_rlog_read_at(&s_log,0,&out));LS_EQ_INT(7,out.t);clean();
}
LS_CASE(export_fails_on_each_write_flush_or_close_and_removes_partial_file)
{
    clean();LS_CHECK(ls_rlog_open(&s_log,TRACK_PATH,sizeof(ls_track_pt_t),4));
    ls_track_pt_t a={.t=7,.lat_e7=430000000,.lon_e7=-710000000};
    LS_CHECK(ls_rlog_append(&s_log,&a));
    char path[64]="";
    for(int i=1;i<=5;i++) {
        write_fault=i;
        LS_EQ_INT(LS_TRACK_EXPORT_NO_WRITE,ls_track_export("gps-storage/out.gpx",path,sizeof(path)));
        LS_CHECK(!path[0]);LS_CHECK(access_io("gps-storage/out.gpx")!=0);
        LS_CHECK(access_io("gps-storage/out.gpx.part")!=0);
    }
    flush_fault=1;LS_EQ_INT(LS_TRACK_EXPORT_NO_WRITE,ls_track_export("gps-storage/out.gpx",path,sizeof(path)));
    sync_fault=1;LS_EQ_INT(LS_TRACK_EXPORT_NO_WRITE,ls_track_export("gps-storage/out.gpx",path,sizeof(path)));
    close_fault=2;LS_EQ_INT(LS_TRACK_EXPORT_NO_WRITE,ls_track_export("gps-storage/out.gpx",path,sizeof(path)));
    LS_EQ_INT(1,ls_track_export("gps-storage/out.gpx",path,sizeof(path)));
    LS_CHECK(!strcmp(path,"gps-storage/out.gpx"));clean();
}

LS_CASE(clear_close_failure_preserves_points_and_last_point)
{
    clean();LS_CHECK(ls_rlog_open(&s_log,TRACK_PATH,sizeof(ls_track_pt_t),4));
    ls_track_pt_t a={.t=7},out;
    LS_CHECK(ls_rlog_append(&s_log,&a));save_time(&a);
    close_fault=3;LS_CHECK(!ls_track_clear());LS_EQ_INT(1,s_log.count);
    uint32_t t;bool epoch;LS_CHECK(ls_track_last_time(&t,&epoch));LS_EQ_INT(7,t);
    LS_EQ_INT(1,ls_rlog_read_at(&s_log,0,&out));LS_EQ_INT(7,out.t);
    LS_CHECK(ls_track_clear());LS_EQ_INT(0,s_log.count);LS_CHECK(!ls_track_last_time(&t,&epoch));
    clean();
}

LS_CASE(recorder_failure_keeps_last_saved_point_and_recording_intent)
{
    clean();LS_CHECK(ls_rlog_open(&s_log,TRACK_PATH,sizeof(ls_track_pt_t),4));
    ls_track_pt_t a={.t=7};LS_CHECK(ls_rlog_append(&s_log,&a));save_time(&a);
    int64_t old=esp_timer_get_time();ls_shim_time_set(1000000);
    s_have_last=false;s_stop=false;s_record_error=ESP_OK;s_task=(TaskHandle_t)1;
    feed_fix=true;saved_intent=1;s_requested=true;close_fault=3;
    track_task(NULL);
    LS_EQ_INT(ESP_FAIL,ls_track_rec_error());LS_CHECK(!ls_track_rec_running());
    LS_EQ_INT(1,saved_intent);LS_CHECK(ls_track_rec_requested());LS_EQ_INT(1,s_log.count);
    uint32_t t;bool epoch;LS_CHECK(ls_track_last_time(&t,&epoch));LS_EQ_INT(7,t);
    ls_shim_time_set(old);clean();
}

LS_CASE(failed_export_preserves_an_existing_destination)
{
    clean();LS_CHECK(ls_rlog_open(&s_log,TRACK_PATH,sizeof(ls_track_pt_t),4));
    ls_track_pt_t a={.t=7};LS_CHECK(ls_rlog_append(&s_log,&a));
    FILE *f=fopen("gps-storage/out.gpx","wb");fputs("prior export",f);fclose(f);
    write_fault=1;
    LS_EQ_INT(LS_TRACK_EXPORT_NO_WRITE,ls_track_export("gps-storage/out.gpx",NULL,0));
    char text[20]={0};f=fopen("gps-storage/out.gpx","rb");LS_CHECK(f!=NULL);
    fread(text,1,sizeof(text)-1,f);fclose(f);LS_CHECK(!strcmp(text,"prior export"));clean();
}
