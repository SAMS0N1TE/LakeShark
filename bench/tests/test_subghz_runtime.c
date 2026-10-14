#include "ls_test.h"
#include <stdint.h>
#include <sys/stat.h>
#ifdef _WIN32
#define mkdir(path,mode) mkdir(path)
#endif
uint32_t esp_random(void);
#define vTaskDelay subghz_delay
#define xTaskGetCurrentTaskHandle subghz_current_task
#define xTaskCreateStaticPinnedToCore subghz_create_task
#define vTaskDelete subghz_delete_task
#define vTaskSuspend subghz_suspend_task
#define eTaskGetState subghz_task_state
#include "../../components/lakeshark/apps/rec/rec_watch_runtime.c"

int subghz_task=1,subghz_lease,subghz_hold_attempts,subghz_cancel_hold;
static int scan_begins,scan_ends,fsk_begins,fsk_ends,polls,cancel_pass,cancel_poll;
static uint32_t frequency=433920000u;
static bool refuse_tune;
static void reset(void)
{
    memset(&s_status,0,sizeof(s_status));s_status.ready=true;
    s_scan_busy=s_learn_busy=s_scan_stop_req=s_catch_armed=false;
    s_scan_min=s_learn_secs=0;s_scan_n=0;s_scan_live_n=0;
    s_watch_pending=s_watch_waiting=false;s_source=s_scan_source=REC_SOURCE_SX1262;
    s_replay_busy=false;s_replay_id=0;s_replay_path[0]=0;
    s_fsk_run=false;s_fsk_loaded=true;s_fsk.bitrate=2400;s_fsk.deviation_hz=10000;
    s_worker_task=(TaskHandle_t)2;subghz_task=1;subghz_lease=0;
    subghz_hold_attempts=subghz_cancel_hold=0;
    scan_begins=scan_ends=fsk_begins=fsk_ends=polls=0;cancel_pass=cancel_poll=0;
    refuse_tune=false;frequency=433920000u;s_gate_db=12;s_on_hit_loaded=0;
}
TaskHandle_t subghz_current_task(void){return (TaskHandle_t)(uintptr_t)subghz_task;}
void subghz_delay(TickType_t ticks)
{
    extern void ls_shim_time_advance(int64_t);
    ls_shim_time_advance((int64_t)ticks*1000);
    if(s_watch_pending) {
        int caller=subghz_task;subghz_task=2;
        s_watch_result=rec_watch_enable_owned(s_watch_on);s_watch_pending=false;
        subghz_task=caller;
    }
    if(!s_fsk_run)s_fsk_exited=true;
}
TaskHandle_t subghz_create_task(TaskFunction_t task,const char *name,uint32_t depth,
    void *arg,UBaseType_t priority,StackType_t *stack,StaticTask_t *tcb,BaseType_t core)
{(void)task;(void)name;(void)depth;(void)arg;(void)priority;(void)stack;(void)tcb;(void)core;return (TaskHandle_t)3;}
void subghz_delete_task(TaskHandle_t task){(void)task;}
void subghz_suspend_task(TaskHandle_t task){(void)task;}
eTaskState subghz_task_state(TaskHandle_t task){(void)task;return s_fsk_exited?eSuspended:eRunning;}
uint32_t rec_get_freq(void){return frequency;}
bool rec_set_freq(uint32_t hz){if(refuse_tune || s_status.enabled)return false;frequency=hz;return true;}
uint32_t ls_lora_caps(void){return LS_LORA_CAP_RX_WIDE|LS_LORA_CAP_BAND_1G5_2G5;}
esp_err_t ls_lora_scan_begin(uint32_t lo,uint32_t hi){(void)lo;(void)hi;scan_begins++;return ESP_OK;}
esp_err_t ls_lora_scan_end(void){scan_ends++;return ESP_OK;}
int ls_lora_scan_pass(float *row,int n,bool *done)
{for(int i=0;i<n;i++)row[i]=-110;*done=true;if(cancel_pass)rec_watch_scan_stop();return n;}
uint32_t ls_lora_scan_bin_hz(uint32_t lo,uint32_t hi,int n,int i){return lo+(uint64_t)(hi-lo)*i/(n-1);}
uint32_t ls_lora_fsk_bw_snap(uint32_t hz){return hz;}
esp_err_t ls_lora_fsk_begin(const ls_fsk_cfg_t *cfg){(void)cfg;fsk_begins++;return ESP_OK;}
esp_err_t ls_lora_fsk_end(void){fsk_ends++;return ESP_OK;}
int ls_lora_fsk_poll(uint8_t *b,size_t n,float *r){memset(b,0,n);(void)r;polls++;if(cancel_poll)rec_watch_scan_stop();return 0;}
bool ls_haptic_play(ls_haptic_effect_t effect){(void)effect;return true;}
int settings_get_subghz_gate_db(void){return 12;}
int settings_get_subghz_on_hit(void){return 0;}
void settings_get_subghz_fsk(uint32_t *rate,uint32_t *dev,uint32_t *sync,int *pre,int *bw)
{*rate=2400;*dev=10000;*sync=0;*pre=32;*bw=59;}
bool ls_mixrf_capture(bool on,uint32_t hz){(void)on;(void)hz;return true;}
void ls_mixrf_snapshot(ls_mixrf_status_t *out){memset(out,0,sizeof(*out));}

LS_CASE(scan_admission_covers_queued_active_and_stopping_work)
{
    reset();LS_CHECK(rec_watch_request_scan(433000000,434000000,30,8));
    LS_CHECK(rec_watch_scan_busy());s_scan_min=0;
    LS_CHECK(!rec_watch_request_scan(433000000,434000000,30,32));
    LS_CHECK(!rec_watch_request_learn(30));rec_watch_scan_stop();
    LS_CHECK(!rec_watch_request_scan(433000000,434000000,30,32));
    char result[112];scan_band(433000000,434000000,30,8,result,sizeof(result));
    LS_EQ_INT(scan_begins,0);LS_EQ_INT(subghz_lease,0);LS_CHECK(strstr(result,"stopped"));
    LS_CHECK(rec_watch_scan_busy());
}
LS_CASE(scan_stop_survives_acquisition_and_pass_cancellation)
{
    reset();s_scan_busy=true;subghz_cancel_hold=1;
    char result[112];scan_band(433000000,434000000,30,8,result,sizeof(result));
    LS_EQ_INT(scan_begins,0);LS_CHECK(strstr(result,"stopped"));
    reset();s_scan_busy=true;cancel_pass=1;
    scan_band(433000000,434000000,30,8,result,sizeof(result));
    LS_EQ_INT(scan_ends,1);LS_EQ_INT(subghz_lease,0);LS_CHECK(strstr(result,"stopped"));
}
LS_CASE(learn_cancels_acquisition_measurement_and_rate_poll)
{
    char result[112];reset();LS_CHECK(rec_watch_request_learn(30));
    LS_CHECK(rec_watch_learn_busy());LS_CHECK(!rec_watch_request_scan(433000000,434000000,30,8));
    subghz_cancel_hold=1;learn_run(frequency,30,result,sizeof(result));
    LS_EQ_INT(scan_begins,0);LS_CHECK(strstr(result,"stopped"));
    reset();s_learn_busy=true;cancel_pass=1;learn_run(frequency,30,result,sizeof(result));
    LS_EQ_INT(scan_ends,1);LS_EQ_INT(fsk_begins,0);LS_EQ_INT(subghz_lease,0);
    reset();s_learn_busy=true;cancel_poll=1;learn_run(frequency,5,result,sizeof(result));
    LS_EQ_INT(fsk_begins,1);LS_EQ_INT(fsk_ends,1);LS_EQ_INT(polls,1);
    LS_EQ_INT(subghz_lease,0);LS_CHECK(!s_learn.valid);LS_CHECK(strstr(result,"stopped"));
}
LS_CASE(worker_catch_and_ui_stop_release_the_same_watch_lease)
{
    reset();subghz_task=2;s_scan_busy=true;s_catch_armed=true;
    LS_CHECK(rec_watch_enable(true));s_scan_busy=false;s_catch_armed=false;
    LS_EQ_INT(subghz_lease,2);subghz_task=1;
    LS_CHECK(rec_watch_enable(false));LS_EQ_INT(subghz_lease,0);
    LS_CHECK(rec_watch_enable(true));LS_EQ_INT(subghz_lease,2);
    LS_CHECK(rec_watch_enable(false));LS_EQ_INT(subghz_lease,0);
}
LS_CASE(catch_does_not_arm_after_tune_refusal_or_stop)
{
    reset();s_scan_busy=true;refuse_tune=true;
    LS_CHECK(!rec_watch_scan_catch(2483000000u));LS_CHECK(!s_catch_armed);
    refuse_tune=false;rec_watch_scan_stop();
    LS_CHECK(!rec_watch_scan_catch(2483000000u));LS_CHECK(!s_catch_armed);
}

void settings_set_subghz_gate_db(int db){(void)db;}
void settings_set_subghz_on_hit(int mode){(void)mode;}
void settings_set_subghz_fsk(uint32_t rate,uint32_t dev,uint32_t sync,int pre,int bw)
{(void)rate;(void)dev;(void)sync;(void)pre;(void)bw;}
ls_rsel_radio_t ls_rsel_saved(ls_rsel_job_t job){(void)job;return LS_RSEL_LORA;}
bool ls_mixrf_start(void){return true;}
uint32_t esp_random(void){return 1;}

LS_CASE(source_changes_refused_for_queued_and_running_radio_operations)
{
    reset();LS_CHECK(rec_watch_request_scan(433000000,434000000,30,8));
    LS_CHECK(!rec_watch_select_source(REC_SOURCE_CC1101));
    LS_EQ_INT(s_scan_source,REC_SOURCE_SX1262);
    s_scan_min=0;LS_CHECK(!rec_watch_select_source(REC_SOURCE_RTL));
    reset();LS_CHECK(rec_watch_request_learn(30));
    LS_CHECK(!rec_watch_select_source(REC_SOURCE_CC1101));
    s_learn_secs=0;LS_CHECK(!rec_watch_select_source(REC_SOURCE_RTL));
    reset();s_replay_id=1;LS_CHECK(!rec_watch_select_source(REC_SOURCE_RTL));
    s_replay_id=0;strcpy(s_replay_path,"/sdcard/test.sub");
    LS_CHECK(!rec_watch_select_source(REC_SOURCE_RTL));
    s_replay_path[0]=0;s_replay_busy=true;
    LS_CHECK(!rec_watch_select_source(REC_SOURCE_RTL));
    s_replay_busy=false;LS_CHECK(rec_watch_select_source(REC_SOURCE_RTL));
}
LS_CASE(replay_policy_allows_fixed_bounds_and_refuses_outside_them)
{
    reset();char result[112];
    LS_CHECK(!replay_refused(433920000,0,6,1800,result,sizeof(result)));
    LS_CHECK(replay_refused(406050000,0,6,1800,result,sizeof(result)));
    LS_CHECK(strstr(result,"refused"));
    LS_EQ_INT(fsk_begins,0);LS_EQ_INT(subghz_hold_attempts,0);
}

LS_CASE(iq_recording_excludes_other_rec_radio_requests)
{
    reset();s_iq_busy=true;
    LS_CHECK(!rec_watch_request_scan(433000000,434000000,30,8));
    LS_CHECK(!rec_watch_request_learn(30));
    LS_CHECK(!rec_watch_request_replay(1,0));
    LS_CHECK(!rec_watch_select_source(REC_SOURCE_RTL));
    LS_CHECK(!rec_watch_enable_owned(true));
    s_iq_busy=false;
}
LS_CASE(iq_consumer_trigger_and_bound)
{
    s_iq_used=s_iq_warm=0;s_iq_floor=s_iq_power=0;s_iq_triggered=false;
    s_iq_rise=s_iq_ring_at=s_iq_ring_count=0;
    atomic_store(&s_iq_full,false);
    uint8_t quiet[4]={0},signal[4]={0,32,0,0};
    for(int i=0;i<2500;i++)iq_consume(NULL,quiet,4,false);
    LS_EQ_UINT(s_iq_used,0);
    for(unsigned i=0;i<REC_LR_IQ_SAMPLES+20;i++)iq_consume(NULL,signal,4,false);
    LS_CHECK(atomic_load(&s_iq_full));LS_EQ_UINT(s_iq_used,REC_LR_IQ_BYTES);
    LS_CHECK(!memcmp(s_iq_data,quiet,4));
    LS_CHECK(!memcmp(s_iq_data+REC_LR_IQ_BYTES-4,signal,4));
}

static int iq_sends;
esp_err_t ls_lora_iq_send_rate_reason(const uint8_t *data,size_t bytes,uint32_t hz,int dbm,uint32_t rate,const char **reason)
{
    *reason=NULL;
    LS_CHECK(ls_mesh_radio_held());LS_EQ_UINT(bytes,306);LS_EQ_UINT(rate,REC_LR_IQ_TX_RATE);LS_EQ_UINT(hz,433920000);LS_EQ_INT(dbm,-9);
    LS_EQ_INT((int8_t)data[0],0);iq_sends++;return ESP_OK;
}
LS_CASE(iq_replay_requires_matching_sidecar_and_tx_gate_before_radio)
{
    reset();iq_sends=0;char result[112];
    const char *path="test-runtime-iq.sub";unlink(path);unlink("test-runtime-iq.json");
    uint8_t data[4000]={0};
    for(unsigned j=500;j<1000;j++)data[j*4+1]=32;
    LS_CHECK(rec_lr_iq_save(path,433920000,data,sizeof(data)));
    iq_replay(path,-9,result,sizeof(result));LS_EQ_INT(iq_sends,0);LS_EQ_INT(subghz_hold_attempts,0);
    LS_CHECK(strstr(result,"sidecar"));
    rec_sidecar_t sc={.freq_hz=433920000,.sample_rate=125000};strcpy(sc.radio,"LR2021");
    LS_EQ_INT(rec_sidecar_write(".","test-runtime-iq",&sc),0);
    iq_replay(path,11,result,sizeof(result));LS_EQ_INT(iq_sends,0);LS_EQ_INT(subghz_hold_attempts,0);
    iq_replay(path,-9,result,sizeof(result));LS_EQ_INT(iq_sends,1);LS_EQ_INT(subghz_lease,0);
    LS_CHECK(strstr(result,"Sent LR2021 I/Q"));
    unlink(path);unlink("test-runtime-iq.json");
    sc.freq_hz=406050000;LS_CHECK(rec_lr_iq_save(path,sc.freq_hz,data,sizeof(data)));
    LS_EQ_INT(rec_sidecar_write(".","test-runtime-iq",&sc),0);
    int held=subghz_hold_attempts;
    iq_replay(path,-9,result,sizeof(result));LS_EQ_INT(iq_sends,1);LS_EQ_INT(subghz_hold_attempts,held);
    LS_CHECK(strstr(result,"distress"));unlink(path);unlink("test-runtime-iq.json");
}
