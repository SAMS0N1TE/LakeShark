/* LS_TEST_SOURCES: ${APP}/cell/cell_core.c ${FW}/components/lakeshark/dsp/ls_sweep_core.c */
/* LS_TEST_INCLUDE: ${APP}/cell ${FW}/components/lakeshark/dsp ${FW}/components/lakeshark/radio ${FW}/components/lakeshark/board */
#include "ls_test.h"
#include <setjmp.h>
#include <unistd.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir(path,mode) _mkdir(path)
#define fsync(fd) _commit(fd)
#endif
#define esp_timer_get_time cell_test_time
#define vTaskDelay cell_test_delay
#define ulTaskNotifyTake cell_test_notify
#define fopen cell_test_fopen
FILE *cell_test_fopen(const char *path,const char *mode);
#include "../../components/lakeshark/apps/cell/cell_monitor.c"
static jmp_buf end_job;
static int notifications,sweeps,fix_mode,lose_fix,slow_mode,reads;
static int64_t clock_us;
static char activity_kind;
static bool sd_ready;
static int baseline_reads;
static void reset_job(void)
{
    memset(&state,0,sizeof(state));memset(&baseline,0,sizeof(baseline));
    sd_ready=false;baseline_reads=0;stopped=false;receiver_epoch=job_epoch=0;baseline_saved=false;
    notifications=sweeps=reads=lose_fix=slow_mode=0;clock_us=1000000;fix_mode=1;activity_kind=0;
}
static void run_job(int job,bool local)
{
    command=job;manual=local;state.band=0;state.busy=true;
    notifications=0;
    if(!setjmp(end_job))task(NULL);
}
uint32_t ulTaskNotifyTake(BaseType_t clear,TickType_t wait)
{(void)clear;(void)wait;if(notifications++)longjmp(end_job,1);return 1;}
void vTaskDelay(TickType_t ticks){clock_us+=(int64_t)ticks*1000;}
int64_t esp_timer_get_time(void){return clock_us;}
void ls_gps_get(ls_gps_state_t *g)
{memset(g,0,sizeof(*g));g->fix=fix_mode && !(lose_fix && sweeps);g->last_fix_us=clock_us;g->lat_deg=43;g->lon_deg=-71;g->hdop=1;}
bool ls_imu_read(ls_imu_sample_t *s){memset(s,0,sizeof(*s));s->az=1;return true;}
bool ls_board_hw_antenna_is_external(void){return false;}
bool ls_sdcard_mounted(void){return sd_ready;}
FILE *cell_test_fopen(const char *path,const char *mode)
{if(!strcmp(mode,"rb") && strstr(path,".bin"))baseline_reads++;return NULL;}
void cell_report_observe(const cell_status_t *s){(void)s;}
void cell_report_activity(const cell_status_t *s,char kind){(void)s;activity_kind=kind;}
ls_radio_err_t ls_sweep_run_endpoint(const ls_sweep_plan_t *p,int gain,uint32_t dwell,bool fast,int8_t *out,
    ls_sweep_progress_fn progress_fn,ls_sweep_cancel_fn cancel_fn,void *arg,const char *endpoint)
{
    (void)gain;(void)dwell;(void)fast;(void)progress_fn;(void)cancel_fn;(void)arg;
    if(strcmp(endpoint,LS_RADIO_ENDPOINT_RTL_USB))abort();
    memset(out,-90,p->n_bins);sweeps++;if(command==CELL_WATCH)stopped=true;return LS_RADIO_OK;
}
ls_radio_err_t ls_radio_acquire(const char *owner,const ls_radio_requirements_t *r,ls_radio_session_t **s)
{(void)owner;if(strcmp(r->preferred_endpoint_id,LS_RADIO_ENDPOINT_RTL_USB))abort();*s=(void*)1;return LS_RADIO_OK;}
ls_radio_err_t ls_radio_iq_configure(ls_radio_session_t *s,const ls_radio_iq_config_t *c,ls_radio_iq_config_t *a)
{(void)s;*a=*c;return LS_RADIO_OK;}
ls_radio_err_t ls_radio_iq_start(ls_radio_session_t *s){(void)s;return LS_RADIO_OK;}
ls_radio_err_t ls_radio_iq_stop(ls_radio_session_t *s){(void)s;return LS_RADIO_OK;}
void ls_radio_release(ls_radio_session_t *s){(void)s;}
ls_radio_err_t ls_radio_iq_read(ls_radio_session_t *s,void *b,size_t n,uint32_t timeout,size_t *got)
{(void)s;(void)b;(void)timeout;*got=n;clock_us+=slow_mode?10000:100;reads++;return LS_RADIO_OK;}
uint64_t rtlsdr_stream_dropped(void){return 0;}
const char *ls_radio_err_name(ls_radio_err_t e){(void)e;return "fake error";}
size_t lte_sync_workspace_size(void){return 1;}
bool lte_sync_find(const uint8_t *iq,size_t n,void *ws,lte_sync_result_t *r,lte_sync_yield_fn yield,void *arg)
{(void)iq;(void)n;(void)ws;(void)r;(void)yield;(void)arg;return false;}
LS_CASE(gps_auto_without_fix_preserves_local_evidence)
{
    for(int job=CELL_WATCH;job<=CELL_LOAD;job++) {
        reset_job();fix_mode=0;baseline.magic=123;baseline.passes=3;
        run_job(job,false);
        LS_EQ_INT(sweeps,0);LS_EQ_INT(baseline.magic,123);
        LS_CHECK(!state.baseline && !state.compared && strstr(state.message,"NO FIX"));
    }
}
LS_CASE(fix_loss_during_learn_never_completes_baseline)
{
    reset_job();lose_fix=1;run_job(CELL_LEARN,false);
    LS_CHECK(baseline.passes<3 && !state.baseline && !baseline_saved);
}
LS_CASE(local_learn_and_watch_keep_fresh_ram_baseline)
{
    reset_job();run_job(CELL_LEARN,true);
    LS_CHECK(state.baseline && !state.baseline_saved && sweeps==3);
    uint32_t sum=baseline.checksum;
    run_job(CELL_WATCH,true);LS_EQ_UINT(baseline.checksum,sum);
    LS_CHECK(!baseline_saved);
}
LS_CASE(unrelated_receiver_events_preserve_baseline_and_job)
{
    reset_job();state.baseline=true;state.compared=true;
    ls_radio_endpoint_event_t event={.kind=LS_RADIO_ENDPOINT_DETACHED,.capabilities=LS_RADIO_RX_IQ_U8};
    strcpy(event.endpoint_id,LS_RADIO_ENDPOINT_HACKRF_USB);receiver_event(&event,NULL);
    LS_CHECK(state.baseline && state.compared && !state.needs_relearn && receiver_epoch==0);
    strcpy(event.endpoint_id,LS_RADIO_ENDPOINT_RTL_USB);receiver_event(&event,NULL);
    LS_CHECK(!state.baseline && state.needs_relearn && state.receiver_missing && cancel(NULL));
    strcpy(event.endpoint_id,LS_RADIO_ENDPOINT_HACKRF_USB);event.kind=LS_RADIO_ENDPOINT_ATTACHED;receiver_event(&event,NULL);
    LS_CHECK(state.receiver_missing);
}
LS_CASE(all_discarded_lte_is_failed_coverage)
{
    reset_job();slow_mode=1;run_job(CELL_LTE,false);
    LS_CHECK(state.lte_attempted>0 && state.lte_discarded==state.lte_attempted && !state.lte_usable);
    LS_CHECK(activity_kind=='F' && strstr(state.message,"no usable"));
    reset_job();run_job(CELL_LTE,false);
    LS_CHECK(state.lte_usable==state.lte_attempted && !state.lte_discarded && activity_kind=='C');
}


int ls_radio_endpoint_subscribe(ls_radio_endpoint_event_fn fn,void *arg){(void)fn;(void)arg;return 0;}

LS_CASE(gps_watch_does_not_replace_fresh_ram_with_older_sd)
{
    reset_job();run_job(CELL_LEARN,false);
    LS_CHECK(state.baseline && !state.baseline_saved);
    uint32_t sum=baseline.checksum;sd_ready=true;
    run_job(CELL_WATCH,false);
    LS_EQ_INT(baseline_reads,0);LS_EQ_UINT(baseline.checksum,sum);
}
