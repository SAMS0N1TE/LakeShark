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
/* The fake radio: an occupied B5 carrier pair, and a dongle whose crystal runs
 * fake_ppm fast, so the offsets the fake sync reports follow what the
 * correction in force leaves. */
static bool plateau_mode,cells_mode;
static int fake_ppm,fake_applied,fake_gain=1,window_calls,first_span,last_span,set_ppm_calls,spans[16],span_n;
static uint32_t tuned_hz,tuned_log[300];
static unsigned tuned_count;
/* rail_bytes: I/Q bytes on a rail in the tune at 881.5 MHz, of the 8192 each
 * tune folds in; clip_from: the sweep (1-based) it starts at. sweep_gain is
 * what the last sweep was asked for. */
static uint32_t rail_bytes;
static int clip_from,sweep_gain,configured_gain,stop_after,observations;
static bool stop_on_delay;
static cell_status_t observed;   /* what the last WATCH pass reported */
static void reset_job(void)
{
    memset(&state,0,sizeof(state));memset(&baseline,0,sizeof(baseline));
    full_scan=false;no_mib=false;cell_n=0;cell_ppm_reset(&ppm_acc);applied_ppm=0;ppm_stale=false;
    plateau_mode=cells_mode=false;fake_ppm=10;fake_applied=0;window_calls=first_span=last_span=0;
    set_ppm_calls=0;tuned_hz=0;tuned_count=0;fake_gain=1;span_n=0;ppm_verify=false;
    sd_ready=false;baseline_reads=0;stopped=false;receiver_epoch=job_epoch=0;baseline_saved=false;
    notifications=sweeps=reads=lose_fix=slow_mode=0;clock_us=1000000;fix_mode=1;activity_kind=0;
    rail_bytes=0;clip_from=1;sweep_gain=configured_gain=0;gain_tenths=CELL_GAIN;state.floor=CELL_MISSING;
    stop_after=observations=0;stop_on_delay=false;memset(&observed,0,sizeof(observed));
}
/* Another job on the same worker, keeping the baseline it holds. */
static void next_job(void)
{
    stopped=false;stop_on_delay=false;sweeps=0;observations=0;
}
static void run_job(int job,bool local)
{
    command=job;manual=local;state.band=0;state.busy=true;
    notifications=0;
    if(!setjmp(end_job))task(NULL);
}
uint32_t ulTaskNotifyTake(BaseType_t clear,TickType_t wait)
{(void)clear;(void)wait;if(notifications++)longjmp(end_job,1);return 1;}
void vTaskDelay(TickType_t ticks){clock_us+=(int64_t)ticks*1000;if(stop_on_delay)stopped=true;}
int64_t esp_timer_get_time(void){return clock_us;}
void ls_gps_get(ls_gps_state_t *g)
{memset(g,0,sizeof(*g));g->fix=fix_mode && !(lose_fix && sweeps);g->last_fix_us=clock_us;g->lat_deg=43;g->lon_deg=-71;g->hdop=1;}
bool ls_imu_read(ls_imu_sample_t *s){memset(s,0,sizeof(*s));s->az=1;return true;}
bool ls_board_hw_antenna_is_external(void){return false;}
bool ls_sdcard_mounted(void){return sd_ready;}
FILE *cell_test_fopen(const char *path,const char *mode)
{if(!strcmp(mode,"rb") && strstr(path,".bin"))baseline_reads++;return NULL;}
void cell_report_observe(const cell_status_t *s){observed=*s;observations++;}
void cell_report_activity(const cell_status_t *s,char kind){(void)s;activity_kind=kind;}
ls_radio_err_t ls_sweep_run_rails(const ls_sweep_plan_t *p,int gain,uint32_t dwell,bool fast,int8_t *out,
    ls_sweep_progress_fn progress_fn,ls_sweep_cancel_fn cancel_fn,void *arg,const char *endpoint,ls_sweep_rails_t *rails)
{
    (void)dwell;(void)fast;(void)cancel_fn;
    if(strcmp(endpoint,LS_RADIO_ENDPOINT_RTL_USB))abort();
    memset(out,-90,p->n_bins);sweeps++;sweep_gain=gain;
    /* WATCH repeats until stopped: by default inside its first sweep (which
     * is then discarded), or with stop_after in the wait after that pass. */
    if(command==CELL_WATCH && !stop_after)stopped=true;
    if(command==CELL_WATCH && stop_after && sweeps>=stop_after)stop_on_delay=true;
    if(rails) {
        memset(rails,0,sizeof(*rails));
        rails->samples=p->n_tunes*8192u;rails->worst_samples=8192;rails->worst_hz=ls_sweep_tune_center(p,0);
        if(rail_bytes && sweeps>=clip_from) {
            rails->rails=rails->worst_rails=rail_bytes;rails->worst_hz=881000000;
        }
    }
    if(plateau_mode) {
        /* 10 MHz carrier 877-886 and a 5 MHz one 886.75-891.25, on a -85 floor. */
        for(uint32_t i=0;i<p->n_bins;i++) {
            uint64_t f=ls_sweep_out_hz(p,i);
            out[i]=(f>=877000000 && f<886000000)||(f>=886750000 && f<891250000)?-55:-85;
        }
    }
    if(progress_fn)for(uint32_t t=1;t<=p->n_tunes;t++)progress_fn(t,p->n_tunes,arg);
    return LS_RADIO_OK;
}
ls_radio_err_t ls_radio_acquire(const char *owner,const ls_radio_requirements_t *r,ls_radio_session_t **s)
{(void)owner;if(strcmp(r->preferred_endpoint_id,LS_RADIO_ENDPOINT_RTL_USB))abort();*s=(void*)1;return LS_RADIO_OK;}
ls_radio_err_t ls_radio_iq_configure(ls_radio_session_t *s,const ls_radio_iq_config_t *c,ls_radio_iq_config_t *a)
{(void)s;*a=*c;configured_gain=c->gain_tenths_db;tuned_hz=(uint32_t)c->center_hz;if(tuned_count<300)tuned_log[tuned_count++]=tuned_hz;return LS_RADIO_OK;}
ls_radio_err_t ls_radio_iq_retune(ls_radio_session_t *s,uint64_t hz,bool fast,uint64_t *actual)
{(void)s;(void)fast;tuned_hz=(uint32_t)hz;*actual=hz;if(tuned_count<300)tuned_log[tuned_count++]=tuned_hz;return LS_RADIO_OK;}
ls_radio_err_t ls_radio_iq_set_ppm(ls_radio_session_t *s,int ppm){(void)s;fake_applied=ppm;set_ppm_calls++;return LS_RADIO_OK;}
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
/* Two sectors on 881.5 and one on 889.0, each at the offset the dongle error
 * leaves after the correction in force. */
bool lte_sync_find_cells(const uint8_t *iq,size_t n,void *ws,lte_sync_set_t *set,lte_sync_yield_fn yield,void *arg,int centre,int span)
{
    (void)iq;(void)n;(void)ws;(void)yield;(void)arg;
    set->count=0;
    if(!cells_mode)return false;
    window_calls++;if(window_calls==1)first_span=span;
    last_span=span;if(span_n<16)spans[span_n++]=span;
    /* fake_gain -1 is a correction that moves the carriers the wrong way. */
    int cfo=(int)(-(double)(fake_ppm-fake_gain*fake_applied)*tuned_hz/1e6);cfo=cfo/500*500;
    if(abs(cfo-centre)>span)return false;
    if(tuned_hz==881500000) {
        set->cell[0].r=(lte_sync_result_t){301,5,4,cfo,.4f,.8f};set->cell[0].frame_start=7000;
        set->cell[1].r=(lte_sync_result_t){302,4,3,cfo,.3f,.6f};set->cell[1].frame_start=3000;
        set->count=2;
    } else if(tuned_hz==889000000) {
        set->cell[0].r=(lte_sync_result_t){17,4,3,cfo,.3f,.7f};set->cell[0].frame_start=1000;
        set->count=1;
    }
    return set->count>0;
}
size_t lte_mib_workspace_size(void){return 1;}
bool lte_mib_crs_power(const uint8_t *iq,size_t n,int pci,int frame,int cfo,void *ws,float *db)
{(void)iq;(void)n;(void)frame;(void)cfo;(void)ws;*db=-40.f-pci/100.f;return true;}
bool lte_mib_confirm(const uint8_t *iq,size_t n,int pci,int frame,int cfo,void *ws,lte_mib_result_t *r,lte_sync_yield_fn y,void *arg)
{
    (void)iq;(void)n;(void)frame;(void)cfo;(void)ws;(void)y;(void)arg;
    memset(r,0,sizeof(*r));
    if(pci==302)return false;
    r->n_rb=50;r->antenna_ports=2;r->sfn=446;r->frames=2;return true;
}
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
    reset_job();full_scan=true;slow_mode=1;run_job(CELL_LTE,false);
    LS_CHECK(state.lte_attempted>0 && state.lte_discarded==state.lte_attempted && !state.lte_usable);
    LS_CHECK(activity_kind=='F' && strstr(state.message,"no usable"));
    reset_job();full_scan=true;run_job(CELL_LTE,false);
    LS_CHECK(state.lte_usable==state.lte_attempted && !state.lte_discarded && activity_kind=='C');
    /* The whole 100 kHz raster: 869.7 .. 893.3 MHz. */
    LS_EQ_INT(state.lte_attempted,237);LS_EQ_INT(sweeps,0);
}
LS_CASE(gated_search_sweeps_first_and_tunes_only_occupied_carriers)
{
    reset_job();plateau_mode=true;run_job(CELL_LTE,false);
    LS_EQ_INT(sweeps,1);LS_EQ_INT(state.plateaus,2);
    LS_CHECK(state.candidates>=2 && state.candidates<=5);
    LS_EQ_INT(state.lte_attempted,state.candidates);
    LS_CHECK(state.lte_attempted<237/10);
    LS_CHECK(activity_kind=='C');
    bool a=false,b=false;
    for(unsigned i=0;i<tuned_count;i++) {
        a|=tuned_log[i]==881500000;b|=tuned_log[i]==889000000;
        LS_CHECK(tuned_log[i]>=869700000 && tuned_log[i]<=893300000);
    }
    LS_CHECK(a && b);
    /* The spectrum and the marks are the occupancy pass. */
    LS_EQ_INT(state.spectrum_count,96);
    unsigned occupied=0;for(int i=0;i<96;i++)occupied+=state.mark[i]&1;
    LS_CHECK(occupied>20 && occupied<60);
    LS_CHECK(strstr(state.message,"occupied carriers"));
    /* An empty band needs no sync at all and is still a complete search. */
    reset_job();run_job(CELL_LTE,false);
    LS_EQ_INT(sweeps,1);LS_EQ_INT(state.lte_attempted,0);LS_CHECK(activity_kind=='C');
    LS_CHECK(strstr(state.message,"SCAN: FULL"));
}
LS_CASE(cells_give_the_dongle_clock_and_narrow_the_search)
{
    reset_job();plateau_mode=cells_mode=true;run_job(CELL_LTE,false);
    /* 881.5 holds two sectors, 889.0 one; the first sync had no clock to go on. */
    LS_EQ_INT(cell_n,3);LS_EQ_INT(first_span,40000);
    cell_ppm_est_t e;LS_CHECK(cell_ppm_estimate(&ppm_acc,&e));
    LS_CHECK(e.n==3 && e.agree==3 && fabsf(e.ppm-10.f)<.7f);
    /* Two agreeing cells apply the correction to the tuner... */
    LS_CHECK(set_ppm_calls>=1 && fake_applied==applied_ppm && abs(applied_ppm-10)<=1);
    LS_CHECK(state.ppm_known && state.ppm_applied && state.ppm_cells==3 && fabsf(state.ppm-10.f)<.7f);
    /* The first capture after the correction still searches everything, to prove it;
     * once a cell has landed where it should, the window narrows: the MIB captures. */
    LS_EQ_INT(span_n,4);LS_EQ_INT(spans[0],40000);LS_EQ_INT(spans[1],40000);
    LS_EQ_INT(spans[2],5000);LS_EQ_INT(spans[3],5000);
    LS_CHECK(!ppm_verify);
    /* RSRP-like power and the MIB of each cell, on the RTL path. */
    cell_lte_t row[CELL_MAX_CELLS];LS_EQ_INT(cell_monitor_cells(row,CELL_MAX_CELLS),3);
    LS_EQ_INT(row[0].pci,301);LS_EQ_INT(row[0].earfcn,2525);LS_EQ_INT(row[0].rsrp10,-430);
    LS_EQ_INT(row[0].n_rb,50);LS_EQ_INT(row[0].ports,2);LS_EQ_INT(row[0].mib,CELL_MIB_OK);
    LS_EQ_INT(row[1].pci,302);LS_EQ_INT(row[1].mib,CELL_MIB_FAILED);LS_EQ_INT(row[1].n_rb,0);
    LS_EQ_INT(row[2].pci,17);LS_EQ_INT(row[2].earfcn,2600);
    LS_EQ_INT(state.mib_tried,3);LS_EQ_INT(state.mib_done,2);
    LS_EQ_INT(state.last.pci,17);
}
LS_CASE(mib_can_be_switched_off_and_the_table_keeps_more_than_eight_rows)
{
    reset_job();plateau_mode=cells_mode=true;no_mib=true;run_job(CELL_LTE,false);
    LS_EQ_INT(state.mib_tried,0);
    cell_lte_t row[CELL_MAX_CELLS];LS_EQ_INT(cell_monitor_cells(row,CELL_MAX_CELLS),3);
    LS_EQ_INT(row[0].mib,CELL_MIB_NONE);LS_EQ_INT(row[0].n_rb,0);
    for(unsigned i=0;i<20;i++){cell_lte_t c={.hz=881500000,.pci=(uint16_t)(100+i)};store_cell(&c);}
    LS_EQ_INT(cell_n,23);
    for(unsigned i=0;i<20;i++){cell_lte_t c={.hz=881500000,.pci=(uint16_t)(100+i)};store_cell(&c);}
    LS_EQ_INT(cell_n,23);   /* a cell heard again replaces its row */
    for(unsigned i=0;i<20;i++){cell_lte_t c={.hz=889000000,.pci=(uint16_t)(300+i)};store_cell(&c);}
    LS_EQ_INT(cell_n,CELL_MAX_CELLS);
}
LS_CASE(clock_correction_can_be_cleared_and_a_replug_forgets_it)
{
    reset_job();plateau_mode=cells_mode=true;run_job(CELL_LTE,false);
    LS_CHECK(applied_ppm!=0);
    run_job(CELL_PPM_ZERO,false);
    LS_EQ_INT(applied_ppm,0);LS_EQ_INT(fake_applied,0);LS_CHECK(!state.ppm_known);
    LS_CHECK(strstr(state.message,"cleared"));
    reset_job();plateau_mode=cells_mode=true;run_job(CELL_LTE,false);
    ls_radio_endpoint_event_t event={.kind=LS_RADIO_ENDPOINT_DETACHED,.capabilities=LS_RADIO_RX_IQ_U8};
    strcpy(event.endpoint_id,LS_RADIO_ENDPOINT_RTL_USB);receiver_event(&event,NULL);
    LS_CHECK(!state.ppm_known && ppm_stale);
    stopped=false;receiver_epoch=job_epoch=0;window_calls=0;
    run_job(CELL_LTE,false);   /* the next search starts the clock over */
    LS_EQ_INT(first_span,40000);
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
LS_CASE(a_known_clock_narrows_the_search_from_the_first_point)
{
    reset_job();plateau_mode=cells_mode=true;
    /* Two cells of an earlier search: 12 ppm fast, already corrected. */
    cell_ppm_add(&ppm_acc,739000000,11,-9000,0);cell_ppm_add(&ppm_acc,751000000,12,-9100,0);
    applied_ppm=fake_applied=12;fake_ppm=12;
    run_job(CELL_LTE,false);
    LS_EQ_INT(first_span,5000);LS_EQ_INT(cell_n,3);
    for(int i=0;i<span_n;i++)LS_EQ_INT(spans[i],5000);
    LS_CHECK(state.ppm_known && state.ppm_applied && state.ppm_value==12);
}
LS_CASE(a_correction_that_moves_the_carriers_the_wrong_way_is_taken_back)
{
    reset_job();plateau_mode=cells_mode=true;fake_gain=-1;
    run_job(CELL_LTE,false);
    /* Applied after the first capture, disproved by the second. */
    LS_CHECK(set_ppm_calls>=2);
    LS_EQ_INT(applied_ppm,0);LS_EQ_INT(fake_applied,0);
    LS_CHECK(!state.ppm_known && !state.ppm_applied && !ppm_verify);
    LS_EQ_INT(cell_n,3);                       /* the cells themselves are still reported */
    LS_EQ_INT(spans[1],40000);                 /* and the proving capture was not narrowed */
}
LS_CASE(a_survey_pass_publishes_its_floor_and_its_carriers)
{
    reset_job();plateau_mode=true;stop_after=1;run_job(CELL_WATCH,true);
    LS_EQ_INT(sweeps,1);LS_EQ_INT(observations,1);
    LS_EQ_INT(observed.floor,-85);LS_EQ_INT(observed.plateaus,2);LS_EQ_INT(observed.spectrum_count,96);
    unsigned occupied=0;for(int i=0;i<96;i++)occupied+=observed.mark[i]&1;
    LS_CHECK(occupied>20 && occupied<60);
    LS_CHECK(!observed.overload && !state.overload);
    LS_EQ_INT(state.floor,-85);
    /* The LTE pre-pass publishes the same floor from the same reading. */
    reset_job();plateau_mode=true;run_job(CELL_LTE,false);
    LS_EQ_INT(state.floor,-85);LS_EQ_INT(state.plateaus,2);
}
LS_CASE(a_clipped_pass_is_never_learned)
{
    /* Clipped from the first pass: LEARN stops there, with nothing learned,
     * and says where and what to do. */
    reset_job();rail_bytes=200;run_job(CELL_LEARN,true);
    LS_EQ_INT(sweeps,1);LS_EQ_INT(baseline.passes,0);
    LS_CHECK(!state.baseline && !baseline_saved);
    LS_CHECK(state.overload);LS_EQ_UINT(state.overload_hz,881000000);
    LS_EQ_UINT(state.rails,200);LS_EQ_UINT(state.rail_samples,8192);
    LS_CHECK(strstr(state.message,"RTL overloaded at 881 MHz - lower gain")!=NULL);
    /* The pass is still drawn, so the operator sees where. */
    LS_EQ_INT(state.spectrum_count,96);LS_EQ_INT(state.floor,-90);
    /* Clipping that starts on the second pass loses the first one too. */
    reset_job();rail_bytes=200;clip_from=2;run_job(CELL_LEARN,true);
    LS_EQ_INT(sweeps,2);LS_EQ_INT(baseline.passes,1);LS_CHECK(!state.baseline);
    /* One byte in a thousand is still a clean pass. */
    reset_job();rail_bytes=8;run_job(CELL_LEARN,true);
    LS_EQ_INT(sweeps,3);LS_CHECK(state.baseline && !state.overload);
}
LS_CASE(a_clipped_pass_is_never_compared_and_the_warning_clears_with_a_clean_one)
{
    reset_job();run_job(CELL_LEARN,true);LS_CHECK(state.baseline);
    /* Watching with the receiver clipping: shown, not compared, no streak. */
    next_job();rail_bytes=200;stop_after=1;plateau_mode=true;run_job(CELL_WATCH,true);
    LS_EQ_INT(observations,1);
    LS_CHECK(!observed.compared && observed.overload);LS_EQ_UINT(observed.changed,0);
    LS_CHECK(observed.rows>0);LS_EQ_INT(observed.row[0].delta,0);
    LS_CHECK(strstr(state.message,"overloaded at 881 MHz")!=NULL);
    /* The same carriers 35 dB over the baseline, unclipped, are compared. */
    next_job();rail_bytes=0;stop_after=1;plateau_mode=true;run_job(CELL_WATCH,true);
    LS_CHECK(observed.compared && !observed.overload && !state.overload);
    LS_EQ_INT(observed.row[0].delta,35);
    LS_CHECK(!strstr(state.message,"overloaded"));
}
LS_CASE(the_chosen_gain_runs_the_sweeps_and_keys_the_baseline)
{
    reset_job();
    LS_CHECK(!cell_monitor_set_gain(300));LS_CHECK(!cell_monitor_set_gain(0));
    LS_CHECK(cell_monitor_set_gain(197));
    cell_status_t s;cell_monitor_get(&s);LS_EQ_INT(s.gain,197);
    run_job(CELL_LEARN,true);
    LS_EQ_INT(sweep_gain,197);LS_EQ_UINT(baseline.gain,197);LS_CHECK(state.baseline);
    /* Back at the usual gain, that baseline is not this receiver's reading. */
    LS_CHECK(cell_monitor_set_gain(CELL_GAIN));
    next_job();stop_after=1;run_job(CELL_WATCH,true);
    LS_EQ_INT(sweep_gain,CELL_GAIN);LS_CHECK(!observed.compared && !state.baseline);
    LS_CHECK(strstr(state.message,"No baseline")!=NULL);
    /* The LTE search tunes at the chosen gain too: its pre-pass and its captures. */
    reset_job();plateau_mode=true;LS_CHECK(cell_monitor_set_gain(144));run_job(CELL_LTE,false);
    LS_EQ_INT(sweep_gain,144);LS_EQ_INT(configured_gain,144);
    reset_job();full_scan=true;LS_CHECK(cell_monitor_set_gain(372));run_job(CELL_LTE,false);
    LS_EQ_INT(configured_gain,372);LS_EQ_INT(sweeps,0);
}
