#include "ls_test.h"
#include <math.h>
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

int subghz_task=1,subghz_lease,subghz_hold_attempts,subghz_cancel_hold,subghz_lr_stream;
static void (*on_delay)(void);
static int scan_begins,scan_ends,fsk_begins,fsk_ends,polls,cancel_pass,cancel_poll;
static uint32_t frequency=433920000u;
static bool refuse_tune,refuse_fsk,frame_every_poll;
/* A sweep's rows: flat by default, or what a case draws for pass p. */
static void (*scan_row)(float *row,int n,int pass);
static int scan_passes;
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
    subghz_lr_stream=0;on_delay=NULL;
    refuse_tune=refuse_fsk=frame_every_poll=false;s_learn_rewatch=false;scan_row=NULL;scan_passes=0;frequency=433920000u;s_gate_db=12;s_on_hit_loaded=0;
}
TaskHandle_t subghz_current_task(void){return (TaskHandle_t)(uintptr_t)subghz_task;}
void subghz_delay(TickType_t ticks)
{
    extern void ls_shim_time_advance(int64_t);
    ls_shim_time_advance((int64_t)ticks*1000);
    if(on_delay)on_delay();
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
const char *ls_lora_chip_name(void){return "LR2021";}
esp_err_t ls_lora_scan_begin(uint32_t lo,uint32_t hi){(void)lo;(void)hi;scan_begins++;return ESP_OK;}
esp_err_t ls_lora_scan_end(void){scan_ends++;return ESP_OK;}
int ls_lora_scan_pass(float *row,int n,bool *done)
{
    if(scan_row)scan_row(row,n,scan_passes++);
    else for(int i=0;i<n;i++)row[i]=-110;
    *done=true;if(cancel_pass)rec_watch_scan_stop();return n;
}
uint32_t ls_lora_scan_bin_hz(uint32_t lo,uint32_t hi,int n,int i){return lo+(uint64_t)(hi-lo)*i/(n-1);}
uint32_t ls_lora_fsk_bw_snap(uint32_t hz){return hz;}
esp_err_t ls_lora_fsk_begin(const ls_fsk_cfg_t *cfg){(void)cfg;fsk_begins++;return refuse_fsk?ESP_FAIL:ESP_OK;}
esp_err_t ls_lora_fsk_end(void){fsk_ends++;return ESP_OK;}
int ls_lora_fsk_poll(uint8_t *b,size_t n,float *r)
{
    memset(b,0,n);(void)r;polls++;if(cancel_poll)rec_watch_scan_stop();
    if(!frame_every_poll)return 0;
    /* The same frame each time: preamble, then 2DD42DD4. */
    static const uint8_t frame[]={0xAA,0xAA,0x2D,0xD4,0x2D,0xD4,0x12,0x34};
    memcpy(b,frame,sizeof(frame)<n?sizeof(frame):n);return (int)n;
}
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
/* The SX1262 board's 902-928 MHz sweep with nothing on the air
   (2026-11-12): -101 dBm to 919 MHz, a step to about -62, -29 and -24 dBm
   in the top two bins, every pass - and a -132 dBm look in one bin, the
   LR2021's of 2026-11-07. The old pass-floor detector reported 11
   frequencies ~450 times each in 25 s. Through scan_band itself: the steady
   shape is floor, and one keyfob press at 915 MHz is the only find. */
static void sx915_row(float *row,int n,int pass)
{
    static const float top[11]={-74,-70,-62,-61,-63,-62,-62,-62,-62,-29,-24};
    for(int i=0;i<n;i++) {
        const float wiggle=(float)(((pass*7+i*13)%5)-2)*0.5f;   /* +/-1 dB */
        row[i]=(i<21?-101.0f:top[i-21])+wiggle;
    }
    if(pass==40)row[5]=-132.0f;
    if(pass==150 || pass==151)row[16]=-60.0f;
    if(pass>=300)rec_watch_scan_stop();
}
LS_CASE(a_steady_band_shape_is_floor_and_only_the_keyfob_is_found)
{
    const float gates[2]={12.0f,25.0f};
    for(int g=0;g<2;g++) {
        reset();s_gate_db=gates[g];s_scan_busy=true;scan_row=sx915_row;
        char result[112];
        scan_band(902000000,928000000,0,32,result,sizeof(result));
        LS_EQ_INT(s_scan_n,1);
        LS_EQ_UINT(s_scan[0].hz,ls_lora_scan_bin_hz(902000000,928000000,32,16));
        LS_EQ_INT(s_scan_events,1);
        LS_NEAR(s_scan[0].floor,-101.0f,1.5f);
        /* The live row carries each bin's own floor for the chart. */
        LS_NEAR(s_scan_live[31].floor,-24.0f,1.5f);
        LS_NEAR(s_scan_live[3].floor,-101.0f,1.5f);
    }
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
/* The rate search always yields the worker's core: a round where the radio
   refuses every rate waits, and once every rate is full the search ends
   instead of spinning to its deadline. */
LS_CASE(learn_search_waits_on_refusal_and_ends_when_every_rate_is_full)
{
    char result[112];reset();s_learn_busy=true;refuse_fsk=true;
    int64_t t0=esp_timer_get_time();
    learn_run(frequency,8,result,sizeof(result));
    LS_CHECK(esp_timer_get_time()-t0>=6000000);
    LS_EQ_INT(polls,0);LS_CHECK(fsk_begins>=7);LS_EQ_INT(subghz_lease,0);
    LS_CHECK(strstr(result,"nothing consistent"));
    reset();s_learn_busy=true;frame_every_poll=true;
    t0=esp_timer_get_time();
    learn_run(frequency,30,result,sizeof(result));
    LS_CHECK(esp_timer_get_time()-t0<20000000);
    LS_EQ_INT(fsk_begins,7);LS_EQ_INT(fsk_ends,7);LS_EQ_INT(polls,7*LEARN_KEEP);
    LS_CHECK(s_learn.valid);LS_EQ_UINT(s_learn.bitrate,600);
    LS_CHECK(s_learn.sync_word!=0);LS_EQ_INT(subghz_lease,0);
}
/* A WATCH handed back while LEARN runs comes on when the job ends, even
   stopped; without the hand-back it stays off. */
LS_CASE(learn_job_restores_a_handed_back_watch)
{
    reset();subghz_task=2;
    LS_CHECK(!rec_watch_learn_rewatch());
    LS_CHECK(rec_watch_request_learn(5));s_learn_secs=0;
    LS_CHECK(rec_watch_learn_rewatch());
    cancel_poll=1;learn_job(frequency,5);
    LS_CHECK(!rec_watch_learn_busy());LS_CHECK(s_status.enabled);
    LS_EQ_INT(subghz_lease,2);
    LS_CHECK(rec_watch_enable(false));LS_EQ_INT(subghz_lease,0);
    reset();subghz_task=2;
    LS_CHECK(rec_watch_request_learn(5));s_learn_secs=0;
    cancel_poll=1;learn_job(frequency,5);
    LS_CHECK(!s_status.enabled);LS_EQ_INT(subghz_lease,0);
    subghz_task=1;
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
bool settings_get_subghz_lr_fsk(void){return false;}
void settings_set_subghz_lr_fsk(bool fsk){(void)fsk;}
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
    LS_CHECK(replay_refused(150000000,0,6,1800,result,sizeof(result)));
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
    sc.freq_hz=150000000;LS_CHECK(rec_lr_iq_save(path,sc.freq_hz,data,sizeof(data)));
    LS_EQ_INT(rec_sidecar_write(".","test-runtime-iq",&sc),0);
    int held=subghz_hold_attempts;
    iq_replay(path,-9,result,sizeof(result));LS_EQ_INT(iq_sends,1);LS_EQ_INT(subghz_hold_attempts,held);
    LS_CHECK(strstr(result,"outside"));unlink(path);unlink("test-runtime-iq.json");
}

/* ---- FSK on the LR2021, from its I/Q stream ---------------------------- */

static uint32_t lrf_seed;
static int lrf_noise(int span)
{
    lrf_seed=lrf_seed*1664525u+1013904223u;
    return (int)((lrf_seed>>16)%(uint32_t)(2*span+1))-span;
}
static int lrf_decodes,lrf_wait;
static void lrf_begin(void)
{
    reset();
    memset(&s_lrf,0,sizeof(s_lrf));s_lrf.floor=8;s_lrf.floor_acc=8<<6;
    atomic_store(&s_lrf_full[0],0);atomic_store(&s_lrf_full[1],0);
    s_lrf_task=NULL;s_lrf_learning=s_lrf_learned=false;
    s_fsk.sync_word=0xD391D391u;
    lrf_seed=1;lrf_decodes=lrf_wait=0;
}
/* The worker's pass over the buffers that are waiting. */
static void lrf_work(void)
{
    for(int b=0;b<2;b++)
        if(atomic_load(&s_lrf_full[b])) {
            lr_fsk_decode(s_lrf_buf[b],s_lrf_len[b],s_lrf_peakv[b],s_lrf_airv[b],s_lrf_tailed[b]);
            atomic_store(&s_lrf_full[b],0);lrf_decodes++;
        }
    lrf_wait=0;
}
/* One sample through the consumer. The worker takes 200 ms over a buffer,
   so the gate has had every chance to reopen by the time it is free. */
static void lrf_put(int i,int q)
{
    const uint8_t b[4]={(uint8_t)i,(uint8_t)(i>>8),(uint8_t)q,(uint8_t)(q>>8)};
    lr_fsk_consume(NULL,b,4,false);
    if(!atomic_load(&s_lrf_full[0]) && !atomic_load(&s_lrf_full[1])){lrf_wait=0;return;}
    if(++lrf_wait>=25000)lrf_work();
}
static void lrf_quiet(int samples){for(int k=0;k<samples;k++)lrf_put(lrf_noise(3),lrf_noise(3));}
/* 2-FSK at 9600 bd, 10 kHz deviation: `frames` of preamble, D391 D391, two
   bytes and their CRC, the carrier dropped for `gap` samples after each. */
static float lrf_phase;
static double lrf_at;
static void lrf_bit(int bit)
{
    for(lrf_at+=REC_LR_IQ_RATE/9600.0;lrf_at>=1.0;lrf_at-=1.0) {
        lrf_phase+=(bit?1.0f:-1.0f)*2.0f*3.14159265f*10000.0f/(float)REC_LR_IQ_RATE;
        lrf_put((int)(2000.0f*cosf(lrf_phase))+lrf_noise(3),(int)(2000.0f*sinf(lrf_phase))+lrf_noise(3));
    }
}
static void lrf_frames(int frames,int gap)
{
    for(int f=0;f<frames;f++) {
        uint8_t body[4]={0x10,(uint8_t)f};
        const uint16_t crc=fsk_pkt_crc16(body,2,0x8005,0xFFFF);
        body[2]=(uint8_t)(crc>>8);body[3]=(uint8_t)crc;
        for(int k=0;k<32;k++)lrf_bit(!(k&1));
        for(int r=0;r<2;r++)for(int k=15;k>=0;k--)lrf_bit((0xD391>>k)&1);
        for(int n=0;n<4;n++)for(int k=7;k>=0;k--)lrf_bit((body[n]>>k)&1);
        lrf_quiet(gap);
    }
}

/* A carrier that stays up is decoded once, not once a second for as long as
   it lasts, and the gate is back for the packet that follows it. */
LS_CASE(lr_fsk_gate_gives_a_steady_carrier_to_the_decoder_once)
{
    lrf_begin();
    lrf_quiet(4000);
    for(int k=0;k<8*(int)LRF_PAIRS;k++)lrf_put(2000+lrf_noise(3),lrf_noise(3));
    lrf_work();
    LS_EQ_INT(lrf_decodes,1);
    LS_CHECK(!s_lrf.on);
    lrf_quiet(4000);
    s_lrf_learning=true;
    lrf_frames(1,4000);lrf_work();
    LS_EQ_INT(lrf_decodes,2);
    LS_CHECK(s_lrf_learned);
    LS_EQ_UINT(s_lrf_learn_pkt.sync,0xD391);
    s_lrf_learning=s_lrf_learned=false;
}
/* A remote held down sends its frame over and over with gaps too short to
   shut the gate, for longer than the buffer. The floor relearned on one
   frame falls again in the gap after it, so the gate is not left deaf:
   every second of it still reaches the decoder. */
LS_CASE(lr_fsk_gate_keeps_taking_frames_repeated_for_seconds)
{
    lrf_begin();
    lrf_quiet(4000);
    lrf_frames(300,560);     /* 4.4 s, 4.5 ms apart */
    lrf_quiet(4000);lrf_work();
    LS_CHECK(lrf_decodes>=4);
}

/* ---- LEARN on the LR2021 ---------------------------------------------- */

/* A frame with a sync word no chip defaults to, sent once, and no CRC: the
   remote LEARN cannot prove from one burst. `roll` changes its payload. */
static void lrf_plain(uint16_t sync,int roll)
{
    const uint8_t body[6]={0x5A,0xC3,(uint8_t)(0x17+roll),(uint8_t)(0xE1^roll),0x3C,0x99};
    for(int k=0;k<32;k++)lrf_bit(!(k&1));
    for(int k=15;k>=0;k--)lrf_bit((sync>>k)&1);
    for(int n=0;n<6;n++)for(int k=7;k>=0;k--)lrf_bit((body[n]>>k)&1);
    lrf_quiet(4000);lrf_work();
}
/* An FM set keyed up on a tone: no packet, but a run of even half-cycles
   the decoder reads as a preamble. */
static void lrf_tone(float hz)
{
    float ph=0;
    for(int k=0;k<2500;k++) {
        const float f=3000.0f*sinf(2.0f*3.14159265f*hz*(float)k/(float)REC_LR_IQ_RATE)+(float)lrf_noise(400);
        ph+=2.0f*3.14159265f*f/(float)REC_LR_IQ_RATE;
        lrf_put((int)(2000.0f*cosf(ph))+lrf_noise(3),(int)(2000.0f*sinf(ph))+lrf_noise(3));
    }
    lrf_quiet(4000);lrf_work();
}
static void lrf_learning(void)
{
    lrf_begin();
    s_lrf_cands=0;s_lrf_learning=true;
    lrf_quiet(4000);
}

LS_CASE(lr_learn_takes_a_packet_that_proves_itself_from_one_burst)
{
    lrf_learning();
    lrf_frames(1,4000);lrf_work();
    LS_CHECK(s_lrf_learned);
    LS_EQ_INT(s_lrf_learn_bursts,1);
    LS_CHECK(s_lrf_learn_pkt.crc_kind!=FSK_CRC_NONE);
    s_lrf_learning=s_lrf_learned=false;
}
/* No CRC and no known sync word: one burst is only a candidate. A second
   with the same word at the same rate confirms it, whatever its payload;
   a burst of something else in between neither confirms nor displaces it. */
LS_CASE(lr_learn_wants_a_second_burst_for_a_packet_it_cannot_prove)
{
    lrf_learning();
    lrf_plain(0xCB89,0);
    LS_CHECK(!s_lrf_learned);
    LS_EQ_UINT(s_lrf_cands,1);
    LS_EQ_UINT(s_lrf_cand_baud,9600);
    lrf_plain(0xC6B5,0);
    LS_CHECK(!s_lrf_learned);
    LS_EQ_UINT(s_lrf_cands,2);
    lrf_plain(0xCB89,7);
    LS_CHECK(s_lrf_learned);
    LS_EQ_INT(s_lrf_learn_bursts,2);
    LS_EQ_UINT(s_lrf_learn_pkt.sync,0xCB89);
    LS_CHECK(s_lrf_learn_pkt.crc_kind==FSK_CRC_NONE && !s_lrf_learn_pkt.sync_known);
    s_lrf_learning=s_lrf_learned=false;
}
/* A remote repeating its frame with no gap the gate can close in is one
   burst, however many packets the capture holds: they do not confirm each
   other, and a later capture still does. */
static void lrf_plain_body(uint16_t sync,int roll)
{
    const uint8_t body[6]={0x5A,0xC3,(uint8_t)(0x17+roll),(uint8_t)(0xE1^roll),0x3C,0x99};
    for(int k=0;k<32;k++)lrf_bit(!(k&1));
    for(int k=15;k>=0;k--)lrf_bit((sync>>k)&1);
    for(int n=0;n<6;n++)for(int k=7;k>=0;k--)lrf_bit((body[n]>>k)&1);
}
LS_CASE(lr_learn_is_not_confirmed_by_repeats_within_one_capture)
{
    lrf_learning();
    for(int r=0;r<3;r++){lrf_plain_body(0xCB89,r);lrf_quiet(300);}   /* 2.4 ms apart */
    lrf_quiet(4000);lrf_work();
    LS_CHECK(!s_lrf_learned);
    LS_CHECK(s_lrf_cands>=1);
    lrf_plain(0xCB89,5);
    LS_CHECK(s_lrf_learned);
    LS_EQ_INT(s_lrf_learn_bursts,2);
    s_lrf_learning=s_lrf_learned=false;
}
/* The same tone keyed up again and again is never a packet, however often
   it repeats: what follows its "preamble" is the end of the burst or the
   tone carrying on. */
LS_CASE(lr_learn_is_not_taught_by_a_tone_keyed_up_repeatedly)
{
    for(int hz=2500;hz<=4000;hz+=500) {
        lrf_learning();
        for(int i=0;i<20;i++) {
            lrf_seed=77u+(uint32_t)i;
            lrf_tone((float)hz);
        }
        LS_CHECK(!s_lrf_learned);
        LS_EQ_UINT(s_lrf_cands,0);
    }
    s_lrf_learning=false;
}

static int press,presses;
static bool press_proven;
static void press_remote(void)
{
    s_lrf_task=NULL;             /* no worker on the bench: lrf_work stands in */
    if(press>=presses)return;
    if(!press++)lrf_quiet(4000);
    if(press_proven)lrf_frames(1,4000);else lrf_plain(0xCB89,press);
    lrf_work();
}
/* The whole LEARN: one press of a remote it cannot prove teaches nothing
   and says to press again; two teach it at the confidence two agreeing
   frames get on the SX1262; a packet with a CRC is still learned at once. */
LS_CASE(lr_learn_applies_nothing_from_one_unproven_burst)
{
    char result[112];rec_learn_t got;
    reset();subghz_lr_stream=1;s_learn_busy=true;lrf_seed=1;
    press=0;presses=1;press_proven=false;on_delay=press_remote;
    memset(&s_learn,0,sizeof(s_learn));
    LS_CHECK(learn_run_iq(frequency,5,result,sizeof(result)));
    LS_CHECK(!rec_watch_learn_result(&got));
    LS_CHECK(strstr(result,"not confirmed") && strstr(result,"9600"));
    LS_EQ_INT(subghz_lease,0);

    reset();subghz_lr_stream=1;s_learn_busy=true;lrf_seed=1;
    press=0;presses=2;press_proven=false;on_delay=press_remote;
    memset(&s_learn,0,sizeof(s_learn));
    LS_CHECK(learn_run_iq(frequency,5,result,sizeof(result)));
    LS_CHECK(rec_watch_learn_result(&got));
    LS_EQ_UINT(got.bitrate,9600);LS_EQ_UINT(got.sync_word>>16,0xCB89);
    LS_NEAR((float)got.deviation_hz,10000.0f,1000.0f);
    LS_EQ_INT(got.confidence,80);LS_EQ_INT(got.agreed,2);
    LS_CHECK(strstr(got.note,"two bursts agreed"));

    reset();subghz_lr_stream=1;s_learn_busy=true;lrf_seed=1;
    press=0;presses=1;press_proven=true;on_delay=press_remote;
    memset(&s_learn,0,sizeof(s_learn));
    LS_CHECK(learn_run_iq(frequency,5,result,sizeof(result)));
    LS_CHECK(rec_watch_learn_result(&got));
    LS_EQ_UINT(got.sync_word,0xD391D391u);
    LS_EQ_INT(got.confidence,95);LS_EQ_INT(got.agreed,1);
    LS_CHECK(strstr(got.note,"one burst"));
    reset();
}

/* ---- The gate's captures as the LR2021 gives them ---------------------- */

/* Three kinds of air. BENCH is the +/-3 counts the cases above use. AIR is
   the chip's own noise as bench/tests/data/fsk_rec.iq16le has it (46 counts
   of |I|+|Q|, band-limited) under a signal 14 dB over it; that noise never
   gives the gate its 5 ms of quiet, so the capture runs the full second.
   AGC is that recording's strong signal: the gain steps down over the first
   4 ms of the burst and the noise sits 18 dB lower until 7 ms after it. */
enum { LRF_BENCH, LRF_AIR, LRF_AGC };
static float lrf_ni,lrf_nq;
static float lrf_gauss(void)
{
    float a=0;
    for(int i=0;i<4;i++) {
        lrf_seed=lrf_seed*1664525u+1013904223u;
        a+=(float)(lrf_seed>>8)/16777216.0f-0.5f;
    }
    return a*1.7320508f;
}
static void lrf_air(int model,float amp,float ph,float sigma)
{
    if(model==LRF_BENCH) {
        lrf_put((int)(amp*cosf(ph))+lrf_noise(3),(int)(amp*sinf(ph))+lrf_noise(3));
        return;
    }
    lrf_ni=0.86f*lrf_ni+0.51f*lrf_gauss();
    lrf_nq=0.86f*lrf_nq+0.51f*lrf_gauss();
    lrf_put((int)lrintf(amp*cosf(ph)+sigma*lrf_ni),(int)lrintf(amp*sinf(ph)+sigma*lrf_nq));
}
/* The carrier `k` samples into a burst, and the noise `k` samples after it. */
static float lrf_amp(int model,int k)
{
    if(model==LRF_BENCH)return 2000.0f;
    if(model==LRF_AIR)return 200.0f;
    return !k?1050.0f:k<60?3800.0f:k<210?272.0f:k<460?93.0f:163.0f;
}
static float lrf_sigma(int model,int k)
{
    if(model!=LRF_AGC)return 28.0f;
    return k<560?3.6f:k<870?14.0f:28.0f;
}
static void lrf_hush(int model,int samples,bool after)
{
    for(int k=0;k<samples;k++)lrf_air(model,0,0,after?lrf_sigma(model,k):28.0f);
}
/* `ms` of 2-FSK at `baud`: preamble, D391 D391, a body and its CRC, as long
   a frame as fits (64 bytes at most) and then the same again 3 ms apart. */
static uint8_t lrf_body[64];
static int lrf_body_n;
static void lrf_burst(int model,int baud,int ms)
{
    const float dev=baud<=2400?5000.0f:10000.0f;
    const int want=ms*baud/1000;
    int n=(want-64)/8;
    if(n>62)n=62;
    if(n<4)n=4;
    for(int i=0;i<n-2;i++)lrf_body[i]=(uint8_t)(0x31+i*7+baud/600);
    const uint16_t crc=fsk_pkt_crc16(lrf_body,(size_t)n-2,0x8005,0xFFFF);
    lrf_body[n-2]=(uint8_t)(crc>>8);lrf_body[n-1]=(uint8_t)crc;
    lrf_body_n=n-2;
    static uint8_t bits[64+64*8];
    int nb=0;
    for(int k=0;k<32;k++)bits[nb++]=(uint8_t)!(k&1);
    for(int r=0;r<2;r++)for(int k=15;k>=0;k--)bits[nb++]=(uint8_t)((0xD391>>k)&1);
    for(int i=0;i<n;i++)for(int k=7;k>=0;k--)bits[nb++]=(uint8_t)((lrf_body[i]>>k)&1);
    const int total=ms*(int)REC_LR_IQ_RATE/1000;
    const double spb=(double)REC_LR_IQ_RATE/baud;
    float ph=0;
    int k=0;
    do {
        const int len=(int)(nb*spb);
        for(int s=0;s<len;s++,k++) {
            ph+=(bits[(int)(s/spb)]?1.0f:-1.0f)*2.0f*3.14159265f*dev/(float)REC_LR_IQ_RATE;
            lrf_air(model,lrf_amp(model,k),ph,model==LRF_AGC?3.6f:28.0f);
        }
        if(k+375+len>total)break;
        for(int s=0;s<375;s++,k++)lrf_air(model,0,0,model==LRF_AGC?3.6f:28.0f);
    } while(1);
}
/* One burst through the gate and the decoder: was its frame read whole? */
static bool lrf_heard(int model,int baud,int ms,uint32_t seed)
{
    lrf_begin();
    lrf_seed=seed;lrf_ni=lrf_nq=0;
    s_lrf_cands=0;s_lrf_learning=true;
    lrf_hush(model,6000,false);lrf_work();
    s_lrf_learned=false;s_lrf_cands=0;
    lrf_burst(model,baud,ms);
    lrf_hush(model,1000,true);
    for(int k=0;k<200 && s_lrf.on;k++)lrf_hush(model,1000,false);
    lrf_work();
    const fsk_pkt_t *p=&s_lrf_learn_pkt;
    const bool ok=s_lrf_learned && p->sync==0xD391 && p->crc_kind==FSK_CRC_CC1101 &&
        p->baud_std==(uint32_t)baud && p->len==lrf_body_n && !memcmp(p->data,lrf_body,(size_t)lrf_body_n);
    s_lrf_learning=s_lrf_learned=false;
    return ok;
}
/* The gate leaves 2 ms before a burst and 5 ms after it, so the decoder's
   own floor is inside any burst over 28 ms. */
LS_CASE(lr_fsk_decodes_short_and_long_bursts_through_the_gate)
{
    static const int BAUD[]={1200,2400,4800,9600},MS[]={20,35,50,80,120,200,300,500,900};
    static const char *const AIR[]={"bench","air","gain step"};
    for(int m=LRF_BENCH;m<=LRF_AGC;m++)
        for(int b=0;b<4;b++)
            for(int j=0;j<9;j++)
                for(uint32_t seed=1;seed<=3;seed++)
                    LS_CHECK_MSG(lrf_heard(m,BAUD[b],MS[j],seed),"%s: %d bd, %d ms, seed %u",
                                 AIR[m],BAUD[b],MS[j],(unsigned)seed);
}
/* The recording itself: ten 12.5 ms bursts of a 4800 bd frame with no CRC,
   each cut out by the gate and read, the second confirming the first. */
LS_CASE(lr_fsk_reads_every_burst_of_a_real_capture)
{
    static int16_t iq[2*REC_LR_IQ_SAMPLES];
    char path[512];
    snprintf(path,sizeof(path),"%s",__FILE__);
    char *slash=strrchr(path,'/');
    LS_CHECK(slash!=NULL);
    if(!slash)return;
    strcpy(slash+1,"data/fsk_rec.iq16le");
    FILE *f=fopen(path,"rb");
    LS_CHECK(f!=NULL);
    if(!f)return;
    const size_t n=fread(iq,4,REC_LR_IQ_SAMPLES,f);
    fclose(f);
    LS_EQ_UINT(n,REC_LR_IQ_SAMPLES);
    lrf_begin();
    s_lrf_cands=0;s_lrf_learning=true;
    int read=0;
    for(size_t k=n-3500;k<n;k++)lrf_put(iq[2*k],iq[2*k+1]);     /* its closing noise first */
    for(size_t k=0;k<n;k++) {
        const int was=s_lrf.buf;
        lrf_put(iq[2*k],iq[2*k+1]);
        if(s_lrf.buf==was)continue;
        const uint32_t cands=s_lrf_cands;
        s_lrf_learned=false;
        lrf_work();
        /* Unproven: the first is a candidate and each one after agrees. */
        LS_CHECK(s_lrf_learned==(read>0));
        if(s_lrf_learned) {
            LS_EQ_UINT(s_lrf_learn_pkt.baud_std,4800);
            LS_EQ_INT(s_lrf_learn_bursts,2);
        }
        if(s_lrf_learned || s_lrf_cands>cands)read++;
    }
    LS_EQ_INT(lrf_decodes,10);
    LS_EQ_INT(read,10);
    LS_EQ_UINT(s_lrf_cands,1);
    s_lrf_learning=s_lrf_learned=false;
}
/* Noise is no packet: 20 s of the chip's own, then its floor stepping back
   up as it does after a burst, which opens the gate on nothing. */
LS_CASE(lr_fsk_noise_through_the_gate_is_no_packet)
{
    for(uint32_t seed=1;seed<=3;seed++) {
        lrf_learning();
        lrf_seed=seed;lrf_ni=lrf_nq=0;
        lrf_hush(LRF_AIR,20*(int)LRF_PAIRS,false);
        for(int i=0;i<20;i++) {
            lrf_hush(LRF_AGC,870,true);
            lrf_hush(LRF_AGC,(int)LRF_PAIRS+4000,false);
        }
        lrf_work();
        LS_CHECK(lrf_decodes>0);
        LS_CHECK(!s_lrf_learned);
        LS_EQ_UINT(s_lrf_cands,0);
    }
    s_lrf_learning=false;
}
