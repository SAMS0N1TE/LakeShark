#include "ls_test.h"
#include "rec_lr_iq.h"
#include "rec_lr_ook.h"
#include "rec_ook_packet.h"
#include "subghz_file.h"
#include "rec_sidecar.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <sys/stat.h>
#include "ls_lora_priv.h"
#include "ls_lora_lr20xx.h"
#include "lr_wf_session.h"
#include "esp_attr.h"
#include "freertos/task.h"
#define LS_TEST_REAL_BOARD_HW
#include "lora_fakes.h"

static bool antenna_allowed=true,stream_active,lease,stop_on_delay,noise_capture;
static unsigned hold_attempts,consumer_starts,antenna_checks;
static bool mesh_listening,mesh_paused,background_owner,field_owner;
bool ls_mesh_radio_wanted(void) { return lease; }
bool ls_mesh_radio_bg(void) { return background_owner; }
bool ls_field_owned(void) { return field_owner; }
bool ls_board_hw_antenna_is_external(void) { return !antenna_allowed; }
bool ls_board_hw_antenna_tx_allowed(void) { return true; }
static bool antenna_moved;
esp_err_t ls_board_hw_antenna_tx_begin(void)
{
    LS_CHECK(ls_lora_hw_locked_by_me());antenna_checks++;
    if (antenna_allowed) antenna_moved = true;
    return antenna_allowed ? ESP_OK : ESP_FAIL;
}
void ls_board_hw_antenna_tx_end(void) { LS_CHECK(ls_lora_hw_locked_by_me()); antenna_moved = false; }
bool ls_mesh_radio_held(void) { return lease && (!mesh_listening || mesh_paused) && ls_lora_send_done(); }
bool ls_mesh_radio_hold(bool on)
{
    lease=on;mesh_paused=on && mesh_listening;if(on)hold_attempts++;
    return !on || ls_mesh_radio_held();
}
static bool test_stream_active(void) { return stream_active; }
static const char *rec_dir(void) { return "/sdcard/rec"; }
static bool rec_watch_start(void) { return true; }
static esp_err_t esp_vfs_fat_info(const char *path,uint64_t *total,uint64_t *free_bytes)
{ (void)path;*total=*free_bytes=1000000;return ESP_OK; }
esp_err_t lr_wf_start_consumer(uint32_t hz,lr_wf_consumer_t consume,void *ctx)
{
    (void)hz;consumer_starts++;
    if(!noise_capture)return ESP_FAIL;
    const uint8_t quiet[4]={20,0,0,0};
    for(unsigned j=0;j<5000;j++)consume(ctx,quiet,4,false);
    return ESP_OK;
}
void lr_wf_get_stats(lr_wf_stats *out) { *out=(lr_wf_stats){0}; }
void lr_wf_stop_stats(lr_wf_stats *out) { *out=(lr_wf_stats){0}; }
static portMUX_TYPE s_lock=portMUX_INITIALIZER_UNLOCKED;
static bool s_iq_busy,s_iq_pending,s_watch_waiting,s_scan_busy,s_learn_busy,s_replay_busy;
static uint32_t s_scan_min,s_learn_secs,s_replay_id;
static char s_replay_path[160];
static struct { bool enabled; } s_status;
static void iq_delay(TickType_t ticks);
#define vTaskDelay iq_delay
#define lr20xx_stream_source2_active test_stream_active
#ifdef _WIN32
#define mkdir(path,mode) mkdir(path)
#endif
#include "../../components/lakeshark/apps/rec/rec_lr_iq_runtime.inc"
#undef lr20xx_stream_source2_active
#undef vTaskDelay
static void iq_delay(TickType_t ticks)
{
    ls_shim_time_advance((int64_t)ticks*1000);
    if(stop_on_delay)rec_watch_iq_stop();
}

static uint8_t iq[REC_LR_IQ_BYTES],copy[REC_LR_IQ_BYTES],tx[REC_LR_IQ_BYTES/2];
static void pair(size_t n,int i,int q)
{iq[n*4]=(uint8_t)i;iq[n*4+1]=(uint8_t)(i>>8);iq[n*4+2]=(uint8_t)q;iq[n*4+3]=(uint8_t)(q>>8);}

LS_CASE(lriq_file_roundtrip_and_truncation)
{
    const char *path="test-lriq.sub";unlink(path);
    for(size_t i=0;i<sizeof(iq);i++)iq[i]=(uint8_t)(i*37);
    LS_CHECK(rec_lr_iq_save(path,433920000,iq,sizeof(iq)));
    LS_CHECK(!rec_lr_iq_save(path,915000000,iq,4));
    uint32_t hz=0;size_t bytes=0;
    LS_CHECK(rec_lr_iq_load(path,&hz,copy,sizeof(copy),&bytes));
    LS_EQ_UINT(hz,433920000);LS_EQ_UINT(bytes,sizeof(iq));LS_CHECK(!memcmp(iq,copy,bytes));
    subghz_file_t f;char line[256];int32_t edges[6];
    LS_CHECK(subghz_file_load(path,&f,edges,6,line,sizeof(line)));
    LS_CHECK(subghz_file_is_iq(&f));LS_CHECK(!subghz_file_is_ook(&f));
    LS_CHECK(!subghz_file_is_fsk(&f));
    LS_CHECK(!rec_lr_iq_load(path,&hz,copy,4,&bytes));
    FILE *out=fopen(path,"wb");LS_CHECK(out!=NULL);
    fputs("Filetype: LakeShark SubGhz IQ File\nVersion: 1\nFrequency: 433920000\nProtocol: LR2021_IQ\nSampleRate: 125000\nSamples: 2\nIQ_Data: 00000000\n",out);fclose(out);
    LS_CHECK(!rec_lr_iq_load(path,&hz,copy,sizeof(copy),&bytes));
    LS_CHECK(subghz_file_load(path,&f,edges,6,line,sizeof(line)));LS_CHECK(!subghz_file_is_iq(&f));
    unlink(path);
}

LS_CASE(lriq_sidecar_and_tx_gate)
{
    rec_sidecar_t in={.freq_hz=433920000,.sample_rate=125000,.gain_tenths=0},out;
    strcpy(in.radio,"LR2021");char json[REC_SIDECAR_JSON_MAX];
    LS_CHECK(rec_sidecar_format(json,sizeof(json),&in)>0);LS_CHECK(rec_sidecar_parse(json,&out));
    LS_CHECK(!strcmp(out.radio,"LR2021"));LS_EQ_UINT(out.sample_rate,125000);LS_EQ_UINT(out.freq_hz,433920000);
    LS_CHECK(!subghz_tx_radio_refusal(433920000,10));
    LS_CHECK(subghz_tx_radio_refusal(406050000,0));LS_CHECK(subghz_tx_radio_refusal(1500000000,0));
    LS_CHECK(subghz_tx_radio_refusal(433920000,11));
}

LS_CASE(lriq_conversion_offset_gate_rate_and_global_level)
{
    for(unsigned rate=9600;rate<=19200;rate*=2) {
        /* Ten clustered OOK pulses, with alternating full/half on levels,
           now intentionally regenerate to one constant level. */
        const size_t n=25000;
        for(size_t i=0;i<n;i++) {
            unsigned block=i/1250;
            double amp=block%2?(block%4==1?4000:2000):20;
            double phase=2*3.141592653589793*30000*i/125000+0.4;
            pair(i,(int)lround(amp*cos(phase)),(int)lround(amp*sin(phase)));
        }
        size_t bytes=rec_lr_iq_convert(iq,n*4,tx,sizeof(tx),rate);
        LS_EQ_UINT(bytes,2*n*rate/125000);
        LS_EQ_UINT(rec_lr_iq_convert(iq,n*4,copy,sizeof(copy),rate),bytes);
        LS_EQ_UINT(rec_lr_iq_convert(iq,n*4,iq,sizeof(iq),rate),bytes);
        LS_CHECK(!memcmp(iq,copy,bytes));
        double dot=0,cross=0;
        for(size_t j=0;j<bytes/2;j++) {
            double t=(double)j/rate;
            unsigned block=(unsigned)(t/0.01);
            double pos=t-block*0.01;
            if(pos<0.002 || pos>0.008)continue;
            int x=(int8_t)tx[j*2],y=(int8_t)tx[j*2+1];
            double mag=hypot(x,y);
            if(!(block%2)){LS_EQ_INT(x,0);LS_EQ_INT(y,0);}
            else {
                LS_CHECK(fabs(mag-48)<1.5);
                int px=(int8_t)tx[j*2-2],py=(int8_t)tx[j*2-1];
                dot+=px*x+py*y;cross+=px*y-py*x;
            }
        }
        LS_CHECK(fabs(atan2(cross,dot)*rate/(2*3.141592653589793))<2);
    }
    memset(iq,0,sizeof(iq));
    LS_EQ_UINT(rec_lr_iq_convert(iq,sizeof(iq),tx,sizeof(tx),9600),0);
    LS_EQ_UINT(rec_lr_iq_convert(iq,3,tx,sizeof(tx),9600),0);
    LS_EQ_UINT(rec_lr_iq_convert(iq,sizeof(iq),tx,sizeof(tx),125000),0);
    LS_EQ_UINT(rec_lr_iq_convert(iq,sizeof(iq),tx,1,9600),0);
}

static void trigger_reset(void)
{
    s_iq_used=s_iq_warm=s_iq_rise=s_iq_ring_at=s_iq_ring_count=0;
    s_iq_floor=s_iq_power=0;s_iq_triggered=false;atomic_store(&s_iq_full,false);
}
static void trigger_feed(unsigned count,int amplitude)
{
    pair(0,amplitude,0);
    for(unsigned j=0;j<count;j++)iq_consume(NULL,iq,4,false);
}
LS_CASE(lriq_trigger_noise_spikes_rise_and_pretrigger)
{
    trigger_reset();trigger_feed(2500,20);
    /* A full 30-second noise-only wait must never produce a saved buffer. */
    trigger_feed(3750000,20);
    LS_CHECK(!s_iq_triggered);LS_EQ_UINT(s_iq_used,0);
    trigger_feed(1,4000);trigger_feed(499,20);
    LS_CHECK(!s_iq_triggered);
    trigger_feed(250,100);LS_CHECK(!s_iq_triggered);
    trigger_feed(125,100);LS_CHECK(s_iq_triggered);
    LS_EQ_UINT(s_iq_used,5000);
    LS_EQ_UINT(s_iq_data[0],20);LS_EQ_UINT(s_iq_data[4996],100);
    trigger_feed(REC_LR_IQ_SAMPLES,100);
    LS_CHECK(atomic_load(&s_iq_full));LS_EQ_UINT(s_iq_used,REC_LR_IQ_BYTES);
    /* No rise when a steady signal is already present throughout calibration. */
    trigger_reset();trigger_feed(5000,4000);LS_CHECK(!s_iq_triggered);
    /* 400/1200 us OOK still triggers on sustained mean-power rise. */
    trigger_reset();trigger_feed(2500,20);
    for(unsigned j=0;j<8 && !s_iq_triggered;j++){trigger_feed(50,4000);trigger_feed(150,20);}
    LS_CHECK(s_iq_triggered);
}

LS_CASE(lriq_ook_packets_split_gaps_and_preserve_long_pulses)
{
    int32_t p[]={800,-4000,50000,-3980,800};uint8_t out[255];uint32_t gap;
    rec_ook_packet_t s={0};
    LS_EQ_UINT(rec_ook_packet(&s,p,5,out,sizeof(out),&gap),40);LS_EQ_UINT(gap,4000);
    LS_EQ_UINT(rec_ook_packet(&s,p,5,out,sizeof(out),&gap),2040);LS_EQ_UINT(gap,0);
    LS_EQ_UINT(s.remaining,460);
    LS_EQ_UINT(rec_ook_packet(&s,p,5,out,sizeof(out),&gap),460+199+40);LS_EQ_UINT(gap,0);
    LS_EQ_UINT(s.edge,5);LS_EQ_UINT(s.remaining,0);
}

LS_CASE(lriq_file_rejects_corrupt_headers_hex_and_extra_samples)
{
    const char *path="test-lriq-bad.sub";
    const char *counts[]={"0","32768","4294967297","-1","1junk","1","1"};
    const char *data[]={"00000000","00000000","00000000","00000000","00000000","0000000G","0000000000000000"};
    for(unsigned i=0;i<7;i++) {
        FILE *f=fopen(path,"wb");LS_CHECK(f!=NULL);
        fprintf(f,"Filetype: LakeShark SubGhz IQ File\nVersion: 1\nFrequency: 433920000\nProtocol: LR2021_IQ\nSampleRate: 125000\nSamples: %s\nIQ_Data: %s\n",counts[i],data[i]);fclose(f);
        uint32_t hz;size_t bytes=99;
        LS_CHECK(!rec_lr_iq_load(path,&hz,copy,sizeof(copy),&bytes));LS_EQ_UINT(bytes,0);
    }
    unlink(path);
    LS_CHECK(!rec_lr_iq_save(path,3000000000u,iq,4));
    LS_CHECK(!rec_lr_iq_save(path,433920000,iq,REC_LR_IQ_BYTES+4));
}
static const char *replay_path="test-lriq-runtime.sub";
static void runtime_fresh(void)
{
    antenna_allowed=true;stream_active=lease=stop_on_delay=false;
    mesh_listening=mesh_paused=background_owner=field_owner=false;
    hold_attempts=consumer_starts=antenna_checks=0;atomic_store(&s_iq_stop,false);
    s_iq_rate=REC_LR_IQ_TX_RATE;
    fk_install(FK_LR);fk_busy_step_us=1;LS_EQ_INT(ls_lora_start(),ESP_OK);
    ls_lora_cfg_t cfg;ls_lora_cfg_default(&cfg);LS_EQ_INT(ls_lora_configure(&cfg),ESP_OK);
    fk.tx_auto_done=true;
    memset(iq,0,4000);for(size_t i=500;i<1000;i++)pair(i,12000,0);
    LS_CHECK(rec_lr_iq_save(replay_path,433920000,iq,4000));
    rec_sidecar_t sc={.freq_hz=433920000,.sample_rate=REC_LR_IQ_RATE};
    strcpy(sc.radio,"LR2021");LS_EQ_INT(rec_sidecar_write(".","test-lriq-runtime",&sc),0);
}
static void runtime_cleanup(void)
{ unlink(replay_path);unlink("test-lriq-runtime.json"); }

LS_CASE(lriq_antenna_route_failure_precedes_radio_setup_and_rec_reports_refusal)
{
    runtime_fresh();antenna_allowed=false;
    const uint8_t wave[]={20,0};int frames=fk.nframes;
    LS_EQ_INT(ls_lora_iq_send(wave,sizeof(wave),433920000,0),ESP_ERR_INVALID_STATE);
    LS_EQ_UINT(antenna_checks,1);LS_EQ_INT(fk.nframes,frames);
    LS_CHECK(!ls_lora_hw_locked_by_me());
    char result[112];iq_replay(replay_path,0,result,sizeof(result));
    LS_CHECK(!strcmp(result,"Replay: antenna switch to internal failed"));
    LS_EQ_UINT(antenna_checks,2);LS_EQ_UINT(fk.tx_starts,0);LS_CHECK(!lease);
    antenna_allowed=true;iq_replay(replay_path,0,result,sizeof(result));
    LS_CHECK(strstr(result,"Sent LR2021 I/Q once"));LS_EQ_UINT(fk.tx_starts,1);
    LS_CHECK(!antenna_moved);
    LS_CHECK(!lease);LS_CHECK(!ls_lora_hw_locked_by_me());runtime_cleanup();
}

LS_CASE(lriq_active_stream_refused_before_record_or_replay_lease)
{
    runtime_fresh();stream_active=true;char result[112];
    iq_record(433920000,result,sizeof(result));LS_CHECK(!strcmp(result,"I/Q: radio busy"));
    iq_replay(replay_path,0,result,sizeof(result));LS_CHECK(!strcmp(result,"Replay: radio busy"));
    LS_EQ_UINT(hold_attempts,0);LS_EQ_UINT(consumer_starts,0);LS_EQ_UINT(fk.tx_starts,0);
    LS_CHECK(!lease);runtime_cleanup();
}

static atomic_bool waiter_done;
static bool waiter_replay,waiter_ready;
static char waiter_result[112];
static int64_t waiter_elapsed;
static void *lease_waiter(void *arg)
{
    (void)arg;waiter_ready=ls_lora_send_done();
    int64_t start=esp_timer_get_time();
    if(waiter_replay)iq_replay(replay_path,0,waiter_result,sizeof(waiter_result));
    else iq_record(433920000,waiter_result,sizeof(waiter_result));
    waiter_elapsed=esp_timer_get_time()-start;
    atomic_store(&waiter_done,true);return NULL;
}
LS_CASE(lriq_contended_spi_lease_times_out_or_stops_and_releases)
{
    for(int mode=0;mode<3;mode++) {
        runtime_fresh();waiter_replay=mode==1;stop_on_delay=mode==2;
        atomic_store(&waiter_done,false);ls_lora_hw_lock();int frames=fk.nframes;
        pthread_t thread;int created=pthread_create(&thread,NULL,lease_waiter,NULL);
        LS_EQ_INT(created,0);bool completed=false;
        if(!created) {
            struct timespec start,now;clock_gettime(CLOCK_MONOTONIC,&start);
            do {
                completed=atomic_load(&waiter_done);if(completed)break;
                const struct timespec pause={0,1000000};nanosleep(&pause,NULL);
                clock_gettime(CLOCK_MONOTONIC,&now);
            } while((now.tv_sec-start.tv_sec)*1000000000LL+now.tv_nsec-start.tv_nsec<1000000000LL);
        }
        /* Always release before joining, including with the old blocking code. */
        ls_lora_hw_unlock();if(!created)pthread_join(thread,NULL);
        LS_CHECK(completed);LS_CHECK(!waiter_ready);LS_CHECK(!lease);
        LS_CHECK(strstr(waiter_result,mode==2?"stopped":"radio busy"));
        if(mode==2)LS_CHECK(waiter_elapsed<1000000);
        else LS_EQ_UINT(waiter_elapsed,1000000);
        LS_CHECK(hold_attempts>0);LS_EQ_UINT(consumer_starts,0);LS_EQ_INT(fk.nframes,frames);
        runtime_cleanup();
    }
}

LS_CASE(lriq_mesh_auto_listen_rx_replay_restores_rx_and_keeps_gates)
{
    runtime_fresh();mesh_listening=true;
    LS_EQ_INT(ls_lora_receive(),ESP_OK);
    LS_CHECK(ls_lora_is_receiving());
    const ls_lora_cfg_t saved=*ls_lora_cfg();
    LS_EQ_UINT(saved.freq_hz,910525000);
    char result[256];
    /* An external route without acknowledgment is the hardware refusal,
       even with a valid lease and a running, TX-disarmed mesh receiver. */
    antenna_allowed=false;
    iq_replay(replay_path,-9,result,sizeof(result));
    LS_CHECK(strstr(result,"antenna switch to internal failed"));
    LS_EQ_UINT(fk.tx_starts,0);LS_CHECK(ls_lora_is_receiving());
    LS_CHECK(!lease && !mesh_paused);
    antenna_allowed=true;
    iq_replay(replay_path,-9,result,sizeof(result));
    LS_CHECK(strstr(result,"Sent LR2021 I/Q once"));
    LS_EQ_UINT(fk.tx_starts,1);LS_CHECK(ls_lora_is_receiving());
    LS_CHECK(!memcmp(&saved,ls_lora_cfg(),sizeof(saved)));
    LS_CHECK(!lease && !mesh_paused);
    background_owner=true;
    iq_replay(replay_path,-9,result,sizeof(result));
    LS_CHECK(strstr(result,"background listener holds the radio"));
    background_owner=false;field_owner=true;
    iq_replay(replay_path,-9,result,sizeof(result));
    LS_CHECK(strstr(result,"field session holds the radio"));
    field_owner=false;fk.proof_magic=0x600DB002u;
    iq_replay(replay_path,-9,result,sizeof(result));
    LS_CHECK(strstr(result,"ROM patch marker present"));
    LS_EQ_UINT(fk.tx_starts,1);LS_CHECK(ls_lora_is_receiving());
    LS_CHECK(!lease && !mesh_paused);runtime_cleanup();
}

LS_CASE(lriq_test_waveform_timing_and_stats)
{
    for(unsigned rate=9600;rate<=19200;rate*=2) {
        size_t bytes=rec_lr_iq_test(tx,sizeof(tx),rate);
        LS_EQ_UINT(bytes,2*((232000ull*rate+500000)/1000000));
        unsigned pulses=0;int prev=0;
        for(size_t j=0;j<bytes/2;j++) {
            int x=tx[2*j];LS_CHECK(x==0 || x==48);LS_EQ_UINT(tx[2*j+1],0);
            if(x && !prev)pulses++;
            prev=x;
        }
        LS_EQ_UINT(pulses,120);
        /* Every rounded edge differs by <= half an output sample. */
        for(unsigned r=0;r<5;r++)for(unsigned b=0;b<24;b++) {
            unsigned start=r*48400+b*1600,high=(0xA53C96u>>(23-b))&1?1200:400;
            size_t lo=((uint64_t)start*rate+500000)/1000000;
            size_t hi=((uint64_t)(start+high)*rate+500000)/1000000;
            LS_EQ_UINT(tx[2*lo],48);LS_EQ_UINT(tx[2*(hi-1)],48);LS_EQ_UINT(tx[2*hi],0);
        }
        LS_EQ_UINT(rec_lr_iq_test(tx,bytes-1,rate),0);
        LS_EQ_UINT(rec_lr_iq_test(NULL,sizeof(tx),rate),0);
    }
    /* Info slicer still uses unconverted 125 kS/s IQ16. */
    memset(iq,0,sizeof(iq));
    for(unsigned b=0;b<24;b++) {
        unsigned high=(0xA53C96u>>(23-b))&1?150:50;
        for(unsigned j=0;j<high;j++)pair(1250+b*200+j,12000,0);
    }
    rec_lr_iq_stats_t stats;LS_CHECK(rec_lr_iq_stats(iq,25000,&stats));
    LS_EQ_UINT(stats.pulses,24);LS_EQ_UINT(stats.short_us,400);LS_EQ_UINT(stats.long_us,1200);
    LS_CHECK(!strcmp(stats.bits,"101001010011110010010110"));
}

LS_CASE(lriq_stats_median_strict_threshold_and_extremes)
{
    rec_lr_iq_stats_t stats;
    pair(0,3,4);pair(1,-3,-4);pair(2,0,0);pair(3,0,10);
    LS_CHECK(rec_lr_iq_stats(iq,16,&stats));
    LS_CHECK(stats.peak==10 && stats.median==5);LS_EQ_UINT(stats.above,1);
    pair(0,0,0);pair(1,2,0);pair(2,3,0);pair(3,4,0);pair(4,5,0);
    LS_CHECK(rec_lr_iq_stats(iq,20,&stats));
    LS_CHECK(stats.median==3);LS_EQ_UINT(stats.above,1); /* strictly > 4 */
    pair(0,0,0);pair(1,0,10);
    LS_CHECK(rec_lr_iq_stats(iq,8,&stats));LS_CHECK(stats.median==5);
    pair(0,-32768,-32768);
    LS_CHECK(rec_lr_iq_stats(iq,4,&stats));LS_CHECK(fabs(stats.peak-sqrt(2147483648.0))<0.001);
    LS_EQ_UINT(stats.above,0);
    memset(iq,0,4000);LS_CHECK(rec_lr_iq_stats(iq,4000,&stats));
    LS_CHECK(stats.peak==0 && stats.median==0);LS_EQ_UINT(stats.above,0);LS_EQ_UINT(stats.pulses,0);
    LS_CHECK(!rec_lr_iq_stats(NULL,4,&stats));LS_CHECK(!rec_lr_iq_stats(iq,0,&stats));
    LS_CHECK(!rec_lr_iq_stats(iq,3,&stats));LS_CHECK(!rec_lr_iq_stats(iq,REC_LR_IQ_BYTES+4,&stats));
}

LS_CASE(lriq_diagnostics_info_and_shared_tx_gates)
{
    runtime_fresh();char result[256];
    s_iq_mode=1;snprintf(s_iq_path,sizeof(s_iq_path),"%s",replay_path);
    iq_diagnostic(result,sizeof(result));
    LS_CHECK(strstr(result,"samples=1000 rate=125000 Hz freq=433920000 Hz"));
    LS_EQ_UINT(fk.tx_starts,0);LS_EQ_UINT(hold_attempts,0);
    char short_result[112];iq_diagnostic(short_result,sizeof(short_result));
    LS_CHECK(strstr(short_result,"OOK pulses="));LS_CHECK(strstr(short_result,"widths="));LS_CHECK(strstr(short_result,"bits="));
    s_iq_mode=2;s_iq_hz=433920000;s_iq_dbm=-9;
    antenna_allowed=false;iq_diagnostic(result,sizeof(result));
    LS_CHECK(strstr(result,"antenna switch to internal failed"));LS_EQ_UINT(fk.tx_starts,0);
    antenna_allowed=true;stream_active=true;iq_diagnostic(result,sizeof(result));
    LS_CHECK(strstr(result,"radio busy"));LS_EQ_UINT(fk.tx_starts,0);
    stream_active=false;s_iq_hz=406050000;iq_diagnostic(result,sizeof(result));
    LS_CHECK(strstr(result,"Replay refused:"));LS_EQ_UINT(fk.tx_starts,0);
    s_iq_hz=433920000;iq_diagnostic(result,sizeof(result));
    LS_CHECK(strstr(result,"Sent LR2021 I/Q once: 231979 us, -9 dBm"));
    LS_EQ_UINT(fk.tx_starts,1);LS_CHECK(!lease);runtime_cleanup();
}

LS_CASE(lriq_record_timeout_reports_no_signal_without_saving)
{
    runtime_fresh();noise_capture=true;
    char result[112];int64_t start=esp_timer_get_time();
    iq_record(433920000,result,sizeof(result));
    LS_CHECK(strstr(result,"no signal"));LS_EQ_UINT(s_iq_used,0);
    LS_CHECK(esp_timer_get_time()-start>=30000000);
    LS_CHECK(!lease);noise_capture=false;runtime_cleanup();
}

LS_CASE(lriq_iq_bandwidth_preserves_am_sidebands)
{
    /* Non-OOK 20 kHz AM must raise the rate, retaining its sidebands. */
    for(size_t j=0;j<25000;j++) {
        double amp=j<6250?20:4000+1000*cos(2*3.141592653589793*20000*j/125000);
        pair(j,(int)lround(amp),0);
    }
    rec_lr_iq_classifier_t report;
    size_t bytes=rec_lr_iq_convert_report(iq,100000,tx,sizeof(tx),9600,&report);
    LS_CHECK(bytes>0 && !report.ook);LS_EQ_UINT(report.rate,125000);
    int low=127,high=0;
    for(size_t j=report.rate/16;j<bytes/2-100;j++) {
        int x=(int8_t)tx[2*j];if(x<low)low=x;if(x>high)high=x;
    }
    LS_CHECK(high-low>10);
}

LS_CASE(lriq_arm_during_ook_uses_quiet_window)
{
    /* ARM mid-frame: 19 loud windows and one OOK gap in calibration. */
    trigger_reset();trigger_feed(2375,4000);trigger_feed(125,20);
    LS_CHECK(s_iq_floor==400);LS_CHECK(!s_iq_triggered);
    for(unsigned j=0;j<8 && !s_iq_triggered;j++) {
        trigger_feed(50,4000);trigger_feed(150,20);
    }
    LS_CHECK(s_iq_triggered);LS_CHECK(s_iq_used>=5000);
}

static double wall_ms(void)
{
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return t.tv_sec*1000.0+t.tv_nsec/1e6;
}
LS_CASE(lriq_clean_five_frames_glitches_edges_and_latency)
{
    for(size_t i=0;i<REC_LR_IQ_SAMPLES;i++)pair(i,20,0);
    for(unsigned r=0;r<5;r++) {
        unsigned base=1250+r*6050;
        /* Weak 88/96 us junk, a full-level 56 us spike, then real data. */
        for(unsigned j=0;j<11;j++)pair(base-80+j,1176,0);
        for(unsigned j=0;j<12;j++)pair(base-55+j,1176,0);
        for(unsigned j=0;j<7;j++)pair(base-25+j,4000,0);
        for(unsigned b=0;b<24;b++) {
            unsigned high=(0xA53C96u>>(23-b))&1?150:50;
            for(unsigned j=0;j<high;j++)pair(base+b*200+j,4000,0);
            /* Hysteresis should bridge this brief dip below the rise level. */
            pair(base+b*200+20,1800,0);
        }
    }
    /* A carrier offset must still classify as OOK and regenerate at DC. */
    for(size_t i=0;i<REC_LR_IQ_SAMPLES;i++) {
        int amp=(int16_t)(iq[4*i]|(unsigned)iq[4*i+1]<<8);
        double phase=2*3.141592653589793*30000*i/125000;
        pair(i,(int)lround(amp*cos(phase)),(int)lround(amp*sin(phase)));
    }
    const char *path="test-lriq-perf.sub";unlink(path);
    LS_CHECK(rec_lr_iq_save(path,433920000,iq,sizeof(iq)));
    uint32_t hz;size_t bytes;double start=wall_ms();
    LS_CHECK(rec_lr_iq_load(path,&hz,copy,sizeof(copy),&bytes));
    double load=wall_ms()-start;unlink(path);
    for(unsigned rate=9600;rate<=19200;rate*=2) {
        bool ook=false;start=wall_ms();
        size_t used=rec_lr_iq_convert_path(copy,bytes,tx,sizeof(tx),rate,&ook);
        double convert=wall_ms()-start;
        printf("IQ host rate=%u load=%.3f ms convert=%.3f ms\n",rate,load,convert);
        LS_CHECK(load+convert<500);LS_CHECK(ook);LS_CHECK(used>0);
        unsigned pulses=0;
        for(size_t j=0;j<used/2;j++) {
            LS_EQ_UINT(tx[2*j+1],0);LS_CHECK(tx[2*j]==0 || tx[2*j]==48);
            if(tx[2*j] && (!j || !tx[2*j-2]))pulses++;
        }
        LS_EQ_UINT(pulses,120);
        for(unsigned r=0;r<5;r++)for(unsigned b=0;b<24;b++) {
            unsigned begin=1250+r*6050+b*200;
            unsigned end=begin+(((0xA53C96u>>(23-b))&1)?150:50);
            size_t lo=((uint64_t)begin*rate+62500)/125000;
            size_t hi=((uint64_t)end*rate+62500)/125000;
            LS_EQ_UINT(tx[2*(lo-1)],0);LS_EQ_UINT(tx[2*lo],48);
            LS_EQ_UINT(tx[2*(hi-1)],48);LS_EQ_UINT(tx[2*hi],0);
        }
        LS_EQ_UINT(rec_lr_iq_convert_path(iq,sizeof(iq),iq,sizeof(iq),rate,&ook),used);
        LS_CHECK(!memcmp(iq,tx,used));memcpy(iq,copy,sizeof(iq));
    }
}
LS_CASE(lriq_burst_fsk_keeps_iq_and_fast_filter)
{
    double phase=0;
    for(size_t i=0;i<REC_LR_IQ_SAMPLES;i++) {
        double amplitude=i<6250?20:4000;
        phase+=2*3.141592653589793*(i/100%2?2500:-2500)/125000;
        pair(i,(int)lround(amplitude*cos(phase)),(int)lround(amplitude*sin(phase)));
    }
    rec_lr_iq_classifier_t report;double start=wall_ms();
    size_t bytes=rec_lr_iq_convert_report(iq,sizeof(iq),tx,sizeof(tx),19200,&report);
    double elapsed=wall_ms()-start;
    printf("IQ host FSK convert=%.3f ms\n",elapsed);
    LS_CHECK(bytes>0);LS_CHECK(!report.ook);LS_EQ_UINT(report.rate,125000);LS_CHECK(elapsed<500);
    unsigned quadrature=0;
    for(size_t j=1200;j<bytes/2;j++)if((int8_t)tx[2*j+1]>10)quadrature++;
    LS_CHECK(quadrature>100);
}
LS_CASE(lriq_default_and_selectable_rates_and_status)
{
    LS_EQ_UINT(REC_LR_IQ_TX_RATE,19200);
    runtime_fresh();char result[256];
    for(unsigned rate=9600;rate<=19200;rate*=2) {
        s_iq_busy=false;
        LS_CHECK(rec_watch_iq_replay(replay_path,-9,rate));
        LS_EQ_UINT(s_iq_rate,rate);LS_EQ_INT(s_iq_mode,3);
        iq_diagnostic(result,sizeof(result));
        LS_CHECK(strstr(result,rate==9600?"7916 us":"7968 us"));
        LS_CHECK(strstr(result,"path=OOK-regen rate="));LS_CHECK(strstr(result,"pulses="));LS_CHECK(strstr(result,"ms convert="));
        LS_CHECK(strstr(result,"sep="));LS_CHECK(strstr(result,"shares="));LS_CHECK(strstr(result,"mid=0"));
        s_iq_busy=false;LS_CHECK(rec_watch_iq_test(433920000,-9,rate));
        LS_EQ_UINT(s_iq_rate,rate);
    }
    s_iq_busy=false;s_iq_pending=false;
    LS_CHECK(rec_watch_iq_replay(replay_path,-9,125000));runtime_cleanup();
}

static unsigned fsk_bit(unsigned b)
{ return b<32?!(b%2):b<56?(0xA53C96u>>(55-b))&1:!(b%2); }

/* Independent frequency histogram: locate modes, then average only their
   interiors so symbol transitions and off-sample noise cannot pull the tones. */
static void measured_tones(const uint8_t *data,size_t n,bool wide,unsigned rate,double tones[2])
{
    unsigned hist[512]={0};double sums[512]={0};
    double px=0,py=0;
    for(size_t i=0;i<n;i++) {
        double x=wide?(int16_t)(data[4*i]|data[4*i+1]<<8):(int8_t)data[2*i];
        double y=wide?(int16_t)(data[4*i+2]|data[4*i+3]<<8):(int8_t)data[2*i+1];
        double threshold=wide?30:2;
        if(hypot(x,y)>threshold && hypot(px,py)>threshold) {
            double hz=atan2(px*y-py*x,px*x+py*y)*rate/6.283185307179586;
            unsigned bin=(unsigned)((hz/rate+0.5)*512);
            if(bin<512){hist[bin]++;sums[bin]+=hz;}
        }
        px=x;py=y;
    }
    unsigned peak[2]={0};
    for(unsigned i=0;i<512;i++)if(hist[i]>hist[peak[0]])peak[0]=i;
    for(unsigned i=0;i<512;i++)
        if(abs((int)i-(int)peak[0])*rate/512>8000 && hist[i]>hist[peak[1]])peak[1]=i;
    for(unsigned k=0;k<2;k++) {
        double sum=0;unsigned count=0;
        for(unsigned i=0;i<512;i++)if(abs((int)i-(int)peak[k])*rate/512<2500) {
            sum+=sums[i];count+=hist[i];
        }
        LS_CHECK(count>100);tones[k]=count?sum/count:0;
    }
    if(tones[0]>tones[1]){double tmp=tones[0];tones[0]=tones[1];tones[1]=tmp;}
}

LS_CASE(lriq_real_imbalanced_fsk)
{
    char path[1024];snprintf(path,sizeof(path),"%s",__FILE__);
    char *slash=strrchr(path,'/');if(!slash)slash=strrchr(path,'\\');
    LS_CHECK(slash!=NULL);if(!slash)return;
    strcpy(slash+1,"data/fsk_rec.iq16le");
    FILE *f=fopen(path,"rb");LS_CHECK(f!=NULL);if(!f)return;
    size_t n=fread(iq,1,sizeof(iq),f);fclose(f);LS_EQ_UINT(n,sizeof(iq));
    double input[2],output[2];measured_tones(iq,n/4,true,125000,input);
    rec_lr_iq_classifier_t report;double start=wall_ms();
    size_t used=rec_lr_iq_convert_report(iq,n,tx,sizeof(tx),19200,&report);
    double elapsed=wall_ms()-start;
    LS_CHECK(used>0 && !report.ook);
    measured_tones(tx,used/2,false,report.rate,output);
    printf("IQ real FSK input=%.1f/%.1f deviation=%.1f output=%.1f/%.1f deviation=%.1f center=%.1f rate=%u convert=%.3f ms\n",
           input[0],input[1],(input[1]-input[0])/2,output[0],output[1],
           (output[1]-output[0])/2,(output[0]+output[1])/2,report.rate,elapsed);
    LS_CHECK(fabs((output[1]-output[0])/2-20000)<2000);
    LS_CHECK(fabs((output[0]+output[1])/2)<1000);
    LS_CHECK(elapsed<500);
    LS_EQ_UINT(rec_lr_iq_convert_report(iq,n,copy,used-1,19200,&report),0);
    LS_EQ_UINT(rec_lr_iq_convert_report(iq,n,iq,sizeof(iq),19200,&report),used);
    LS_CHECK(!memcmp(iq,tx,used));
}

LS_CASE(lriq_nine_db_tone_and_symbol_imbalance)
{
    for(unsigned variant=0;variant<12;variant++) {
        unsigned pattern=variant%4;
        double deviation=variant<4?20000:variant<8?12000:5000;
        double phase=0;
        for(size_t i=0;i<REC_LR_IQ_SAMPLES;i++) {
            /* 4800 baud, +/-5, 12 or 20 kHz with +6 kHz offset. Exercise either tone
               being 9 dB stronger, and 50/50 versus 75/25 symbol occupancy. */
            unsigned symbol=(unsigned)(i*4800ull/125000);
            bool bit=pattern<2?symbol%2:symbol%4!=0;
            bool on=i>=1250 && i<30000;
            double amp=on?(bit==(variant%2)?4000:4000/pow(10,9.0/20)):20;
            pair(i,(int)lround(amp*cos(phase)),(int)lround(amp*sin(phase)));
            phase+=6.283185307179586*(6000+(bit?deviation:-deviation))/125000;
        }
        rec_lr_iq_classifier_t report;
        size_t used=rec_lr_iq_convert_report(iq,sizeof(iq),tx,sizeof(tx),19200,&report);
        LS_CHECK(used>0 && !report.ook);
        LS_CHECK(fabs(report.tone_separation_hz-2*deviation)<500);
        LS_CHECK(report.tone_share[0]>=0.15 && report.tone_share[1]>=0.15);
        LS_CHECK(report.mid_burst_switches>=500);
        LS_CHECK(fabs(report.tone_share[0]+report.tone_share[1]-1)<1e-6);
        double tones[2];measured_tones(tx,used/2,false,report.rate,tones);
        LS_CHECK(fabs((tones[1]-tones[0])/2-deviation)<deviation*0.1);
        LS_CHECK(fabs((tones[1]+tones[0])/2)<500);
        /* Check both tone interiors survive, including the weak symbol.
           Gain is global; the amplitude ratio must remain about 9 dB. */
        double sum[2]={0};unsigned count[2]={0};
        for(size_t j=1;j<used/2;j++) {
            double pos=(double)j*125000/report.rate;
            if(pos<2000 || pos>29000)continue;
            double symbol=pos*4800/125000,within=symbol-floor(symbol);
            if(within<0.4 || within>0.6)continue;
            bool bit=pattern<2?(unsigned)symbol%2:(unsigned)symbol%4!=0;
            double amp=hypot((int8_t)tx[2*j],(int8_t)tx[2*j+1]);
            LS_CHECK(amp>8);sum[bit]+=amp;count[bit]++;
        }
        double ratio=20*log10((sum[1]/count[1])/(sum[0]/count[0]));
        LS_CHECK(fabs(ratio-(variant%2?9:-9))<1.5);
    }
}

static void fsk_capture(double deviation)
{
    double phase=0;
    for(size_t i=0;i<REC_LR_IQ_SAMPLES;i++) {
        double us=i*8.0-10000;
        double frame=us>=0?fmod(us,22500):22500;
        bool on=us>=0 && us<225000 && frame<12500;
        unsigned bit=on?fsk_bit((unsigned)(frame*4800/1000000)):0;
        double amplitude=on?3000+((unsigned)(us/22500)%3)*700:20;
        phase+=6.283185307179586*(6000+(bit?deviation:-deviation))/125000;
        pair(i,(int)lround(amplitude*cos(phase)),(int)lround(amplitude*sin(phase)));
    }
}

LS_CASE(lriq_repeated_wide_fsk_keeps_deviation_and_zero_gaps)
{
    /* Ten clustered ~13 ms bursts reproduce the erroneous OOK fallback. */
    const double deviations[]={12000,20000,20000};
    const unsigned requests[]={19200,19200,125000},rates[]={125000,125000,125000};
    for(unsigned k=0;k<3;k++) {
        fsk_capture(deviations[k]);
        rec_lr_iq_classifier_t report;
        size_t bytes=rec_lr_iq_convert_report(iq,sizeof(iq),tx,sizeof(tx),requests[k],&report);
        LS_CHECK(bytes>0 && !report.ook);LS_EQ_UINT(report.pulses,10);
        LS_CHECK(report.clustered>0.99 && report.phase<0.9);
        LS_EQ_UINT(report.rate,rates[k]);
        LS_CHECK(report.bandwidth_hz>2*deviations[k]);
        double sum[2]={0},magnitude=0;unsigned count[2]={0},gaps=0;
        for(size_t j=1;j<bytes/2;j++) {
            double us=(double)j*1000000/report.rate-10000;
            if(us<0 || us>=225000)continue;
            double frame=fmod(us,22500);
            int x=(int8_t)tx[2*j],y=(int8_t)tx[2*j+1];
            if(frame>12600 && frame<22400) {
                LS_EQ_INT(x,0);LS_EQ_INT(y,0);gaps++;continue;
            }
            double symbol=frame*4800/1000000,within=symbol-floor(symbol);
            if(frame<1000 || frame>11500 || within<0.35 || within>0.65)continue;
            unsigned bit=fsk_bit((unsigned)symbol);
            int px=(int8_t)tx[2*j-2],py=(int8_t)tx[2*j-1];
            sum[bit]+=atan2(px*y-py*x,px*x+py*y)*report.rate/6.283185307179586;
            count[bit]++;magnitude+=hypot(x,y);
        }
        LS_CHECK(gaps>1000 && count[0]>100 && count[1]>100);
        double low=sum[0]/count[0],high=sum[1]/count[1];
        printf("IQ FSK deviation=%.0f rate=%u bandwidth=%.0f tones=%.0f/%.0f phase=%.4f\n",
               deviations[k],report.rate,report.bandwidth_hz,low,high,report.phase);
        LS_CHECK(fabs(low+deviations[k])<1500 && fabs(high-deviations[k])<1500);
        LS_CHECK(fabs((low+high)/2)<500);
        LS_CHECK(magnitude/(count[0]+count[1])>35);
        LS_EQ_UINT(rec_lr_iq_convert_report(iq,sizeof(iq),copy,bytes-1,requests[k],&report),0);
        LS_EQ_UINT(rec_lr_iq_convert_report(iq,sizeof(iq),iq,sizeof(iq),requests[k],&report),bytes);
        LS_CHECK(!memcmp(iq,tx,bytes));
    }
}

LS_CASE(lriq_bandwidth_narrow_broad_and_uncertain_rates)
{
    rec_lr_iq_classifier_t report;
    fsk_capture(45000);
    LS_CHECK(rec_lr_iq_convert_report(iq,sizeof(iq),tx,sizeof(tx),19200,&report)>0);
    LS_CHECK(!report.ook);LS_EQ_UINT(report.rate,125000);
    /* Too short for two useful spectral windows: preserve native samples. */
    fsk_capture(20000);
    LS_EQ_UINT(rec_lr_iq_convert_report(iq+4*1250,128*4,tx,sizeof(tx),19200,&report),256);
    LS_CHECK(!report.ook);LS_EQ_UINT(report.rate,125000);
}

LS_CASE(lriq_test_all_rates_and_fsk_frame_content)
{
    const unsigned rates[]={4800,9600,19200,38400,76800,125000};
    for(unsigned k=0;k<6;k++) {
        unsigned rate=rates[k];
        LS_CHECK(rec_lr_iq_rate_supported(rate));
        LS_CHECK(rec_lr_iq_test(tx,sizeof(tx),rate)>0);
        size_t bytes=rec_lr_iq_test_fsk(tx,sizeof(tx),rate);
        LS_EQ_UINT(bytes,2*((102500ull*rate+500000)/1000000));
        LS_EQ_UINT(rec_lr_iq_test_fsk(copy,bytes-1,rate),0);
        unsigned frames=0;bool prev=false;
        for(size_t j=0;j<bytes/2;j++) {
            double mag=hypot((int8_t)tx[2*j],(int8_t)tx[2*j+1]);
            bool on=mag>0;
            if(on){LS_CHECK(fabs(mag-48)<0.75);if(!prev)frames++;}
            prev=on;
        }
        LS_EQ_UINT(frames,5);
        if(rate>=76800)for(unsigned r=0;r<5;r++)for(unsigned b=0;b<60;b++) {
            size_t start=((uint64_t)r*22500*rate+500000)/1000000;
            size_t j=start+(size_t)((b+0.5)*rate/4800);
            int x=(int8_t)tx[2*j],y=(int8_t)tx[2*j+1];
            int px=(int8_t)tx[2*j-2],py=(int8_t)tx[2*j-1];
            double hz=atan2(px*y-py*x,px*x+py*y)*rate/6.283185307179586;
            LS_CHECK(fabs(hz-(fsk_bit(b)?20000:-20000))<750);
        }
        s_iq_busy=false;LS_CHECK(rec_watch_iq_test(433920000,-9,rate));
        LS_EQ_UINT(s_iq_rate,rate);LS_EQ_INT(s_iq_mode,2);
        s_iq_busy=false;LS_CHECK(rec_watch_iq_test_fsk(433920000,-9,rate));
        LS_EQ_UINT(s_iq_rate,rate);LS_EQ_INT(s_iq_mode,5);
    }
    LS_CHECK(!rec_lr_iq_rate_supported(44100));
    LS_EQ_UINT(rec_lr_iq_test_fsk(tx,sizeof(tx),44100),0);
    LS_EQ_UINT(rec_lr_iq_test_fsk(NULL,sizeof(tx),125000),0);
    s_iq_busy=s_iq_pending=false;
}

LS_CASE(lriq_runtime_fsk_uses_reported_rate)
{
    runtime_fresh();runtime_cleanup();fsk_capture(20000);
    LS_CHECK(rec_lr_iq_save(replay_path,433920000,iq,sizeof(iq)));
    rec_sidecar_t sc={.freq_hz=433920000,.sample_rate=REC_LR_IQ_RATE};
    strcpy(sc.radio,"LR2021");LS_EQ_INT(rec_sidecar_write(".","test-lriq-runtime",&sc),0);
    char result[256];iq_replay(replay_path,-9,result,sizeof(result));
    LS_CHECK(strstr(result,"path=IQ rate=125000"));LS_EQ_UINT(fk.tx_starts,1);
    LS_CHECK(strstr(result,"262136 us"));
    runtime_cleanup();runtime_fresh();
    s_iq_mode=5;s_iq_hz=433920000;s_iq_dbm=-9;s_iq_rate=125000;
    iq_diagnostic(result,sizeof(result));LS_CHECK(strstr(result,"102504 us"));
    LS_EQ_UINT(fk.tx_starts,1);runtime_cleanup();
}

LS_CASE(lriq_noisy_ook_clusters_decode_every_full_frame)
{
    /* Amplitude spread and phase jitter deliberately defeat the old 98% /
       0.9995 classifier. Include high spikes and long, weak junk in gaps. */
    for(size_t i=0;i<REC_LR_IQ_SAMPLES;i++)pair(i,35+(int)(i%17),0);
    for(unsigned r=0;r<5;r++) {
        unsigned base=1250+r*6050;
        for(unsigned j=0;j<7;j++)pair(base-100+j,16000,0); /* 56 us */
        for(unsigned j=0;j<20;j++)pair(base-70+j,800,0); /* weak 160 us */
        for(unsigned b=0;b<24;b++) {
            unsigned high=(0xA53C96u>>(23-b))&1?150:50;
            for(unsigned j=0;j<high;j++) {
                unsigned i=base+b*200+j;
                double amp=3000+(i*73%2000);
                double phase=1.5*i+0.18*sin(i*0.71);
                pair(i,(int)lround(amp*cos(phase)),(int)lround(amp*sin(phase)));
            }
        }
    }
    for(unsigned rate=9600;rate<=19200;rate*=2) {
        rec_lr_iq_classifier_t report;
        size_t bytes=rec_lr_iq_convert_report(iq,sizeof(iq),tx,sizeof(tx),rate,&report);
        LS_CHECK(bytes>0 && report.ook);LS_EQ_UINT(report.pulses,120);
        LS_CHECK(report.clustered>0.99);LS_CHECK(report.bimodal<0.98);
        unsigned frames=0,bits=0,value=0;size_t previous_end=0;
        for(size_t j=0;j<bytes/2;) {
            LS_EQ_UINT(tx[2*j+1],0);LS_CHECK(tx[2*j]==0 || tx[2*j]==48);
            if(!tx[2*j]){j++;continue;}
            size_t begin=j;
            while(j<bytes/2 && tx[2*j]) {
                LS_EQ_UINT(tx[2*j],48);LS_EQ_UINT(tx[2*j+1],0);j++;
            }
            if(bits && (begin-previous_end)*1000000ull/rate>4000) {
                LS_EQ_UINT(bits,24);LS_EQ_UINT(value,0xA53C96);frames++;bits=value=0;
            }
            unsigned width=(unsigned)((j-begin)*1000000ull/rate);
            LS_CHECK((width>=290 && width<=510) || (width>=1090 && width<=1310));
            value=(value<<1)|(width>800);bits++;previous_end=j;
        }
        LS_EQ_UINT(bits,24);LS_EQ_UINT(value,0xA53C96);frames++;
        LS_EQ_UINT(frames,5);
    }
}

/* Princeton envelope with carrier offset, additive noise, and optional drift.
   Sparse phase impulses also exercise coherence below the former 0.9 gate. */
static void noisy_ook_capture(double drift)
{
    double phase=0;
    for(size_t i=0;i<REC_LR_IQ_SAMPLES;i++) {
        unsigned frame=i>=1250?(unsigned)(i-1250)/6050:5;
        unsigned within=i>=1250?(unsigned)(i-1250)%6050:6050;
        unsigned bit=within/200;
        unsigned high=bit<24 && ((0xA53C96u>>(23-bit))&1)?150:50;
        bool on=frame<5 && bit<24 && within%200<high;
        double amp=on?4000:0;
        double noisy_phase=phase+(i%20==0?2.0:0);
        int ni=(int)((i*73+19)%101)-50,nq=(int)((i*47+31)%103)-51;
        pair(i,(int)lround(amp*cos(noisy_phase))+ni,(int)lround(amp*sin(noisy_phase))+nq);
        phase+=6.283185307179586*(3500+drift*i/(REC_LR_IQ_SAMPLES-1))/125000;
    }
}

LS_CASE(lriq_offset_noisy_single_tone_ook_regenerates)
{
    noisy_ook_capture(0);
    rec_lr_iq_classifier_t report;
    size_t used=rec_lr_iq_convert_report(iq,sizeof(iq),tx,sizeof(tx),19200,&report);
    LS_CHECK(used>0 && report.ook);LS_EQ_UINT(report.rate,19200);
    LS_CHECK(report.phase<0.9);LS_EQ_UINT(report.pulses,120);
    LS_CHECK(report.tone_separation_hz<4000 ||
             report.tone_share[0]<0.15 || report.tone_share[1]<0.15);
    for(size_t j=0;j<used/2;j++) {
        LS_EQ_UINT(tx[2*j+1],0);LS_CHECK(tx[2*j]==0 || tx[2*j]==48);
    }
}

LS_CASE(lriq_drifting_single_tone_ook_regenerates)
{
    for(unsigned drift=1000;drift<=2000;drift+=1000) {
        noisy_ook_capture(drift);
        rec_lr_iq_classifier_t report;
        size_t used=rec_lr_iq_convert_report(iq,sizeof(iq),tx,sizeof(tx),19200,&report);
        LS_CHECK(used>0 && report.ook);LS_EQ_UINT(report.rate,19200);
        LS_EQ_UINT(report.pulses,120);
        for(size_t j=0;j<used/2;j++) {
            LS_EQ_UINT(tx[2*j+1],0);LS_CHECK(tx[2*j]==0 || tx[2*j]==48);
        }
    }
}

LS_CASE(lriq_ook_turn_on_chirp_regenerates)
{
    /* Every Princeton pulse starts 20 kHz above its carrier for 120 us.
       Test both a flat transient and a linear settling chirp. */
    for(unsigned variant=0;variant<2;variant++) {
        memset(iq,0,sizeof(iq));
        for(unsigned r=0;r<5;r++)for(unsigned b=0;b<24;b++) {
            unsigned begin=1250+r*6050+b*200;
            unsigned high=((0xA53C96u>>(23-b))&1)?150:50;
            double phase=0;
            for(unsigned j=0;j<high;j++) {
                pair(begin+j,(int)lround(4000*cos(phase)),(int)lround(4000*sin(phase)));
                double transient=j<15?20000*(variant?1.0-j/15.0:1.0):0;
                phase+=6.283185307179586*(3500+transient)/125000;
            }
        }
        for(unsigned rate=9600;rate<=19200;rate*=2) {
            rec_lr_iq_classifier_t report;
            size_t used=rec_lr_iq_convert_report(iq,sizeof(iq),tx,sizeof(tx),rate,&report);
            LS_CHECK(used>0 && report.ook);LS_EQ_UINT(report.rate,rate);
            LS_EQ_UINT(report.pulses,120);
            /* Quantization can split a single tone; it must not retain the chirp. */
            LS_CHECK(report.tone_separation_hz<100);
            LS_EQ_UINT(report.mid_burst_switches,0);
            for(size_t j=0;j<used/2;j++) {
                LS_EQ_UINT(tx[2*j+1],0);LS_CHECK(tx[2*j]==0 || tx[2*j]==48);
            }
            for(unsigned r=0;r<5;r++)for(unsigned b=0;b<24;b++) {
                unsigned begin=1250+r*6050+b*200;
                unsigned end=begin+(((0xA53C96u>>(23-b))&1)?150:50);
                size_t lo=((uint64_t)begin*rate+62500)/125000;
                size_t hi=((uint64_t)end*rate+62500)/125000;
                LS_EQ_UINT(tx[2*(lo-1)],0);LS_EQ_UINT(tx[2*lo],48);
                LS_EQ_UINT(tx[2*(hi-1)],48);LS_EQ_UINT(tx[2*hi],0);
            }
        }
    }
}

LS_CASE(lriq_two_tones_in_separate_ook_pulses_regenerate)
{
    /* Populated, separated tones alone cannot veto a valid OOK envelope. */
    memset(iq,0,sizeof(iq));
    for(unsigned b=0;b<100;b++)for(unsigned j=0;j<100;j++) {
        double phase=6.283185307179586*(b%2?20000:0)*j/125000;
        pair(1250+b*250+j,(int)lround(4000*cos(phase)),(int)lround(4000*sin(phase)));
    }
    rec_lr_iq_classifier_t report;
    LS_CHECK(rec_lr_iq_convert_report(iq,sizeof(iq),tx,sizeof(tx),19200,&report)>0);
    LS_CHECK(report.ook);LS_EQ_UINT(report.mid_burst_switches,0);
    LS_CHECK(report.tone_separation_hz>19000);
    LS_CHECK(report.tone_share[0]>0.45 && report.tone_share[1]>0.45);
}

LS_CASE(lriq_mid_burst_switch_thresholds)
{
    const unsigned on_samples[]={10000,28750},minimum[]={8,23};
    for(unsigned duration=0;duration<2;duration++)for(unsigned pass=0;pass<2;pass++) {
        unsigned switches=minimum[duration]-1+pass;
        memset(iq,0,sizeof(iq));double phase=0;
        for(unsigned j=0;j<on_samples[duration];j++) {
            unsigned tone=(j*(switches+1)/on_samples[duration])%2;
            pair(1250+j,(int)lround(4000*cos(phase)),(int)lround(4000*sin(phase)));
            phase+=6.283185307179586*(tone?20000:-20000)/125000;
        }
        rec_lr_iq_classifier_t report;
        LS_CHECK(rec_lr_iq_convert_report(iq,sizeof(iq),tx,sizeof(tx),19200,&report)>0);
        LS_EQ_UINT(report.mid_burst_switches,switches);
        LS_CHECK(report.ook==!pass);
        LS_EQ_UINT(report.rate,pass?125000:19200);
    }
}

static char dump_text[1024];
static unsigned dump_calls,dump_cancel;
static bool collect_dump(void *ctx,const char *line)
{
    (void)ctx;
    if(++dump_calls==dump_cancel)return false;
    if(strlen(dump_text)+strlen(line)>=sizeof(dump_text))return false;
    strcat(dump_text,line);return true;
}
LS_CASE(lriq_dump_base64_crc_and_cancellation)
{
    /* CRC-32/ISO-HDLC of twelve ASCII bytes, computed independently. */
    const uint8_t raw[]="123456789012";
    dump_text[0]=0;dump_calls=dump_cancel=0;
    LS_CHECK(rec_lr_iq_dump(raw,12,433920000,collect_dump,NULL));
    LS_CHECK(!strcmp(dump_text,
        "LRIQ-BEGIN bytes=12 rate=125000 freq=433920000 format=IQ16LE\n"
        "LRIQ-DATA MTIzNDU2Nzg5MDEy\nLRIQ-END crc32=5D34EB96\n"));
    dump_text[0]=0;dump_calls=0;dump_cancel=2;
    LS_CHECK(!rec_lr_iq_dump(raw,12,433920000,collect_dump,NULL));
    LS_CHECK(!strstr(dump_text,"LRIQ-END"));
    dump_cancel=0;
    LS_CHECK(!rec_lr_iq_dump(raw,11,433920000,collect_dump,NULL));
    s_iq_busy=false;LS_CHECK(rec_watch_iq_dump("capture.sub"));
    LS_EQ_INT(s_iq_mode,4);LS_CHECK(s_iq_pending);
    LS_CHECK(!rec_watch_iq_dump("capture.sub"));
    s_iq_busy=s_iq_pending=false;
}

LS_CASE(lriq_dump_host_decoder_roundtrip)
{
    uint8_t raw[52],decoded[53];
    for(unsigned i=0;i<sizeof(raw);i++)raw[i]=(uint8_t)(i*37);
    dump_text[0]=0;dump_calls=dump_cancel=0;
    LS_CHECK(rec_lr_iq_dump(raw,sizeof(raw),433920000,collect_dump,NULL));
    FILE *f=fopen("test-lriq-dump.log","wb");LS_CHECK(f!=NULL);
    if(!f)return;
    fputs("console noise\n",f);fputs(dump_text,f);fputs("console prompt\n",f);fclose(f);
    char helper[1024],command[1400];snprintf(helper,sizeof(helper),"%s",__FILE__);
    char *slash=strrchr(helper,'/');if(!slash)slash=strrchr(helper,'\\');
    LS_CHECK(slash!=NULL);if(!slash)return;
    strcpy(slash+1,"decode_lriq_dump.py");
    snprintf(command,sizeof(command),"python \"%s\" test-lriq-dump.log test-lriq-dump.iq16le",helper);
    LS_EQ_INT(system(command),0);
    f=fopen("test-lriq-dump.iq16le","rb");LS_CHECK(f!=NULL);
    if(f){LS_EQ_UINT(fread(decoded,1,sizeof(decoded),f),sizeof(raw));fclose(f);LS_CHECK(!memcmp(raw,decoded,sizeof(raw)));}
    unlink("test-lriq-dump.log");unlink("test-lriq-dump.iq16le");
}
