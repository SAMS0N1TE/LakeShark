/* Compile the actual firmware adapter; inspect sessions using transport fakes. */
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#ifdef _WIN32
#define mkdir(path,mode) mkdir(path)
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static void fake_delay(TickType_t);
static int fake_printf(const char *,...);
#define printf fake_printf
#define vTaskDelay fake_delay
#ifndef CATCH_ADAPTER_SOURCE
#define CATCH_CURRENT_ADAPTER 1
#define CATCH_ADAPTER_SOURCE "../../components/lakeshark/apps/rec/rec_catch.c"
#endif
#include CATCH_ADAPTER_SOURCE
#undef vTaskDelay
#undef printf
#include <stdarg.h>
#include <assert.h>
#include "ls_test.h"
#include "exp_lr433_vectors.h"
#include "catch_fsk_fixture.h"
void ls_shim_time_set(int64_t);
void ls_shim_time_advance(int64_t);
static bool inject_row;
static unsigned printed_rows;
static bool check_diagnostics;
static char console_text[8192];
static size_t console_used;
static bool synth_iq;
static catch_fixture_t fixture;
static uint32_t read_cost_us=700;
static bool board_clock, fft_clock_started;
static bool finite_stream;
static uint64_t consumer_block, producer_blocks, model_max_gap, model_last_read;
static uint8_t producer_iq[16][32768];
static uint64_t model_overwrites;
static uint32_t clock_checks, clock_frames;
/* Charge measured board stages, not host execution time. The actual adapter
   still chooses allowance and measures stages; only the old adapter sleeps. */
static uint64_t board_detector_clock(void *ctx)
{
    (void)ctx;
    if(s_detector.coarse_frames!=clock_checks) {
        ls_shim_time_advance(21);clock_checks=s_detector.coarse_frames;
    }
    if(s_detector.frames!=clock_frames) {
        ls_shim_time_advance(815);clock_frames=s_detector.frames;fft_clock_started=false;
    } else if(s_detector.fill==CATCH_BINS) {
        if(fft_clock_started)ls_shim_time_advance(919);
        fft_clock_started=true;
    }
    return esp_timer_get_time();
}
static uint64_t printed_ms[4];
static int fake_printf(const char *fmt,...)
{
    va_list ap;va_start(ap,fmt);
    if(!strncmp(fmt,"%llu ms",7) && inject_row) {
        unsigned long long stamp=va_arg(ap,unsigned long long);
        assert(printed_rows<4);printed_ms[printed_rows++]=stamp;
        if(printed_rows==1) {
            catch_burst_t b={.id=3,.frequency=868300000,.start_us=300000,.stop_us=304000};
            detection(&b,true,NULL);
        }
        va_end(ap);return 0;
    }
    if(check_diagnostics) {
        int n=vsnprintf(console_text+console_used,sizeof(console_text)-console_used,fmt,ap);
        assert(n>=0 && (size_t)n<sizeof(console_text)-console_used);
        console_used+=(size_t)n;va_end(ap);return n;
    }
    int n=vprintf(fmt,ap);va_end(ap);return n;
}
static unsigned yields;
static bool stop_on_delay;
static void fake_delay(TickType_t ticks)
{
    assert(ticks>0);yields++;
    ls_shim_time_advance((int64_t)ticks*1000);
    if(stop_on_delay)rec_catch_stop();
}
static uint32_t fake_backlog;
static uint64_t fake_drops;
static ls_radio_err_t read_error=LS_RADIO_ERR_DISCONNECTED;
static size_t read_size;
uint32_t rtlsdr_stream_avail(void){return fake_backlog;}
uint64_t rtlsdr_stream_dropped(void){return fake_drops;}
static bool lease_ok=true, wanted;
static int lease, lease_attempts, ook_ends, fsk_ends, submitted;
static ls_lora_chip_t chip=LS_LORA_CHIP_LR20XX;
static esp_err_t session_error;
static ls_ook_cfg_t ook;
static ls_fsk_cfg_t fsk;
bool ls_mesh_radio_hold_bg(bool on){lease_attempts++;lease=on&&lease_ok;return !on||lease_ok;}
bool ls_mesh_radio_wanted(void){return wanted;}
ls_lora_chip_t ls_lora_chip(void){return chip;}
uint32_t ls_lora_fsk_bw_snap(uint32_t hz){return hz;}
esp_err_t ls_lora_fsk_begin(const ls_fsk_cfg_t *cfg){fsk=*cfg;ls_shim_time_advance(3200);return session_error;}
esp_err_t ls_lora_ook_begin(const ls_ook_cfg_t *cfg){ook=*cfg;ls_shim_time_advance(3200);return session_error;}
esp_err_t ls_lora_fsk_end(void){fsk_ends++;return ESP_OK;}
esp_err_t ls_lora_ook_end(void){ook_ends++;return ESP_OK;}
void rec_watch_submit_mod(rec_source_t source,uint32_t hz,const int32_t *p,int n,int peak,int reason,const rec_fsk_mod_t *m)
{(void)source;(void)p;(void)peak;(void)reason;assert(hz>400000000 && n>=6 && m->bitrate>0);submitted++;}
void rec_watch_submit_from(rec_source_t source,uint32_t hz,const int32_t *p,int n,int peak,int reason)
{(void)source;(void)hz;(void)p;(void)n;(void)peak;(void)reason;submitted++;}
ls_radio_err_t ls_radio_acquire(const char *owner,const ls_radio_requirements_t *req,ls_radio_session_t **out)
{(void)owner;(void)req;(void)out;return LS_RADIO_ERR_UNAVAILABLE;}
void ls_radio_release(ls_radio_session_t *session){(void)session;}
ls_radio_err_t ls_radio_iq_configure(ls_radio_session_t *s,const ls_radio_iq_config_t *cfg,ls_radio_iq_config_t *actual)
{(void)s;*actual=*cfg;return LS_RADIO_OK;}
ls_radio_err_t ls_radio_iq_start(ls_radio_session_t *s){(void)s;return LS_RADIO_OK;}
ls_radio_err_t ls_radio_iq_stop(ls_radio_session_t *s){(void)s;return LS_RADIO_OK;}
ls_radio_err_t ls_radio_iq_read(ls_radio_session_t *s,void *dst,size_t n,uint32_t wait,size_t *got)
{(void)s;assert(wait==0);
 if(finite_stream) {
     uint64_t now=esp_timer_get_time();
     uint64_t next=1000000+(producer_blocks+1)*read_size*500000/CATCH_RATE;

     uint64_t available=(now-1000000)*CATCH_RATE/(read_size*500000ull);
     /* Integer arrival timestamps round down by less than one microsecond. */
     if(now>=next && available==producer_blocks)available++;
     unsigned slots=256*1024/(unsigned)read_size;
     while(producer_blocks<available) {
         fixture_block(&fixture,producer_iq[producer_blocks%slots],read_size);
         producer_blocks++;
         if(producer_blocks-consumer_block>slots) {
             consumer_block++;fake_drops+=read_size;model_overwrites++;
         }
     }
     assert(producer_blocks>consumer_block);
     *got=read_size;memcpy(dst,producer_iq[consumer_block++%slots],*got);
     fake_backlog=(uint32_t)((producer_blocks-consumer_block)*read_size);
     if(model_last_read && now-model_last_read>model_max_gap)model_max_gap=now-model_last_read;
     model_last_read=now;ls_shim_time_advance(read_cost_us);return LS_RADIO_OK;
 }
 *got=read_size<n?read_size:n;if(synth_iq)fixture_block(&fixture,dst,*got);else memset(dst,128,*got);
 ls_shim_time_advance(read_cost_us);
 if(board_clock) {
     uint64_t sample_end=1000000+fixture.sample*1000000/CATCH_RATE;
     if((uint64_t)esp_timer_get_time()<sample_end)ls_shim_time_set(sample_end);
     fake_backlog=(uint32_t)(((uint64_t)esp_timer_get_time()-sample_end)*CATCH_RATE/500000);
 }
 return read_error;}
const char *ls_radio_err_name(ls_radio_err_t err){(void)err;return "fake";}
int ls_lora_fsk_poll(uint8_t *b,size_t size,float *rssi){(void)b;(void)size;(void)rssi;return 0;}
int ls_lora_ook_poll(uint8_t *b,size_t size,float *rssi){(void)b;(void)size;(void)rssi;return 0;}
LS_CASE(firmware_sessions_and_raw_capture)
{
    s_lock=xSemaphoreCreateMutex();assert(s_lock);
    memset(&s_status,0,sizeof(s_status));memset(s_result,0,sizeof(s_result));
    s_head=0;s_has_pending=false;
    catch_burst_t b={.id=1,.frequency=433926000,.modulation=CATCH_OOK};
    chip=LS_LORA_CHIP_SX126X;assert(!handoff_begin(NULL,&b) && lease_attempts==0);
    chip=LS_LORA_CHIP_LR20XX;b.frequency=315000000;assert(!handoff_begin(NULL,&b) && lease_attempts==0);
    b.frequency=433926000;lease_ok=false;assert(!handoff_begin(NULL,&b) && !lease);
    lease_ok=true;session_error=ESP_ERR_INVALID_STATE;assert(!handoff_begin(NULL,&b) && !lease);
    session_error=ESP_OK;assert(handoff_begin(NULL,&b) && lease);
    assert(ook.freq_hz==433926000 && ook.bitrate==16000 && ook.pattern==14 && ook.frame_bytes==252);
    handoff_end(NULL);assert(!lease && ook_ends==1);
    b.modulation=CATCH_FSK;b.deviation=26000;
    assert(handoff_begin(NULL,&b) && lease && fsk.freq_hz==433926000 && fsk.deviation_hz==26000);
    assert(fsk.sync_word==0x0F0F0000 && fsk.sync_bits==16 && fsk.payload_bytes==128);
    detection(&b,true,NULL);s_slot=0;
    detection(&b,true,NULL);assert(s_status.count==1 && s_status.detections==1);
    catch_burst_t duplicate=b;duplicate.id=2;duplicate.frequency+=1000;
    detection(&duplicate,true,NULL);assert(s_status.count==1 && s_status.detections==1);
    memset(s_bits,0xAA,32);capture(1,b.frequency,-58,32);
    assert(s_result[0].captures==1 && s_result[0].edges>250 && !s_result[0].decoded && submitted==1);
    assert(s_result[0].lr_rssi==-58);
    catch_result_t row;assert(rec_catch_result(0,&row) && row.burst.frequency==b.frequency);
    /* An evicted slot must not receive a frame from the old session. */
    s_result[0].burst.id=42;capture(1,b.frequency,-58,32);assert(s_result[0].captures==1);
    handoff_end(NULL);assert(!lease && fsk_ends==1);
    b.modulation=CATCH_OOK;b.frequency=912600000;
    assert(handoff_begin(NULL,&b) && ook.bitrate==32768 && ook.pattern==0x2999 && s_kind==LR433_IN_ERT);
    handoff_end(NULL);assert(!lease);
    b.frequency=433926000;b.detected_us=10000;
    ls_shim_time_set(10500);catch_hold_t hold={0};uint32_t latency;
    assert(catch_handoff(&hold,&b,&HANDOFF,&latency) && latency==3700);
    catch_handoff_stop(&hold,&HANDOFF);
    printf("actual adapter with fake SPI session: %u us handoff\n",latency);
    /* Existing recording fixtures pass through the adapter's capture path,
       covering TPMS, weather and ERT without importing decoder code. */
    const int cases[]={4,6,10,13,14,17,18,19,21,22};
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        const lr433_vec_t *v=&VECS[cases[i]];
        s_kind=v->kind;s_mod=v->kind==LR433_IN_FSK?CATCH_FSK:CATCH_OOK;
        s_rate=v->rate;s_lead=v->lead;s_lead_bits=v->lead_bits;
        memset(s_bits,0,sizeof(s_bits));memcpy(s_bits,v->data,(size_t)v->len);
        s_slot=0;s_result[0].burst.id=1;
        capture(1,433926000,-61,v->bits/8);
        assert(s_result[0].decoded && !strcmp(s_result[0].message.id,v->id));
        assert(!strncmp(s_result[0].message.model,v->model,strlen(v->model)));
        if(isfinite(v->temp_c))assert(fabs(s_result[0].message.temp_c-v->temp_c)<0.05);
        if(v->consumption>=0)assert(s_result[0].message.consumption==v->consumption);
    }
    lr433_capture_t cap={.rate=16000,.lead=7,.lead_bits=4,.data=(const uint8_t *)"\x80",.bits=8};
    int n=edges_from_bits(&cap);assert(n==3 && s_pulses[0]==-62 && s_pulses[1]==250 && s_pulses[2]==-438);
    rec_catch_stop();catch_status_t status;rec_catch_status(&status);assert(!status.running);
    printf("CATCH firmware adapter OK; detector %zu bytes, decoder %zu bytes, raw ring %zu bytes\n",
        sizeof(s_detector),sizeof(s_decode),sizeof(s_edges));
    assert(rec_catch_start(433920000)==LS_RADIO_OK);
    assert(rec_catch_start(433920000)==LS_RADIO_ERR_BUSY);
    rec_catch_stop();assert(rec_catch_start(433920000)==LS_RADIO_ERR_BUSY);
    s_rtl_done=s_lr_done=true;
    assert(rec_catch_start(868300000)==LS_RADIO_OK);rec_catch_stop();
}

#ifdef CATCH_CONTINUOUS_IQ
LS_CASE(rtl_continuous_drain_loss_and_stop)
{
    if(!s_lock)s_lock=xSemaphoreCreateMutex();
    memset(&s_status,0,sizeof(s_status));catch_detector_init(&s_detector,433920000);
    ls_shim_time_set(1000000);s_rtl_epoch=1000000;s_usb_drops=0;s_status_due=0;
    s_detector.clock_us=detector_clock;s_detector.expensive_ready_us=10000000;
    s_run=true;read_error=LS_RADIO_OK;read_size=sizeof(s_iq);fake_drops=fake_backlog=0;
    read_cost_us=700;
    unsigned before=yields;
    for(int i=0;i<6;i++)assert(rtl_block(NULL)==LS_RADIO_OK);
    assert(s_status.blocks==6 && yields==before);
    assert(s_detector.coarse_frames==24 && !s_detector.frames);
    fake_backlog=sizeof(s_iq)*3;fake_drops=8192;
    assert(rtl_block(NULL)==LS_RADIO_OK && yields==before);
    assert(s_status.lost_samples==4096 && s_status.usb_drop_bytes==8192);
    assert(!s_status.full_allowance);
    assert(rtl_block(NULL)==LS_RADIO_OK && s_status.lost_samples==4096);
    ls_shim_time_advance(1000000); /* Unreported pre-USB starvation is visible. */
    assert(rtl_block(NULL)==LS_RADIO_OK && s_status.deficit_samples>2000000);
    assert(s_status.lost_samples==4096 && s_status.max_block_gap_us>=1000000);
    read_error=LS_RADIO_ERR_TIMEOUT;read_size=0;fake_backlog=0;
    stop_on_delay=true;assert(rtl_block(NULL)==LS_RADIO_OK);
    assert(!running() && yields==before+1);stop_on_delay=false;
    read_error=LS_RADIO_ERR_DISCONNECTED;
    assert(rtl_block(NULL)==read_error && yields==before+1);
    puts("CATCH drains without cooldown, counts loss once, stops on empty read");
}
#endif

LS_CASE(console_list_snapshot_during_insert)
{
    if(!s_lock)s_lock=xSemaphoreCreateMutex();
    memset(&s_status,0,sizeof(s_status));memset(s_result,0,sizeof(s_result));
    s_head=0;s_has_pending=false;
    catch_burst_t b={.id=1,.frequency=433197500,.start_us=100000,.stop_us=104000};
    detection(&b,true,NULL);
    b.id=2;b.frequency=434500000;b.start_us=200000;b.stop_us=204000;detection(&b,true,NULL);
    char *argv[]={"catch","list"};printed_rows=0;inject_row=true;
    assert(rec_catch_command(2,argv)==0);inject_row=false;
    assert(printed_rows==2 && printed_ms[0]==200 && printed_ms[1]==100);
    puts("CATCH list stays consistent during concurrent ring insertion");
}

LS_CASE(fsk_at_usb_cadence_through_actual_worker)
{
    if(!s_lock)s_lock=xSemaphoreCreateMutex();
    const float snrs[]={-6,0,6,20,32};
    synth_iq=true;read_size=sizeof(s_iq);read_error=LS_RADIO_OK;
    fake_drops=0;read_cost_us=1800; /* Explicit cost model, not P4 measurement. */
    for(unsigned run=0;run<6;run++) {
        memset(&s_status,0,sizeof(s_status));memset(s_result,0,sizeof(s_result));
        s_head=0;s_has_pending=false;s_run=true;s_usb_drops=0;
        s_full_allowance=1;
#ifdef CATCH_CONTINUOUS_IQ
        s_status_due=0;
#else
        s_frame_cost=1800;s_check_cost=200;
#endif
        catch_detector_init(&s_detector,915000000);
        fixture=(catch_fixture_t){.random=0x12345678+run,.re=1,
            .amplitude=run<5?4.964819187*pow(10,snrs[run]/20):0};
        ls_shim_time_set(1000000);s_rtl_epoch=1000000;
        unsigned before=yields;
        for(unsigned block=0;block<3516;block++) {
            uint64_t start=1000000+fixture.sample*1000000/CATCH_RATE;
            ls_shim_time_set((int64_t)start);
            /* Read a completed block, then spend modeled compute/cooldown time.
               A large pre-existing backlog must never cause intentional drops. */
            fake_backlog=run==4 && block==0?3*sizeof(s_iq):0;
            assert(rtl_block(NULL)==LS_RADIO_OK);
            assert((uint64_t)esp_timer_get_time()<=start+sizeof(s_iq)*500000ull/CATCH_RATE);
        }
        catch_status_t st;rec_catch_status(&st);
        printf("worker SNR %.0f %s: %lu detections, blocks %lu dropped %lu USB %llu, modeled duty %.2f%% allowance %u\n",
            run<5?(double)snrs[run]:0.0,run<5?"burst":"noise",(unsigned long)st.detections,
            (unsigned long)st.blocks,(unsigned long)st.block_drops,(unsigned long long)st.usb_drop_bytes,
            100.0*(double)st.active_us/st.elapsed_us,s_full_allowance);
        fflush(stdout);assert(st.detections==(run<5?8u:0u));
        assert(st.blocks==3516 && !st.block_drops && !st.usb_drop_bytes);
        (void)before;assert(st.active_us*100<=st.elapsed_us*35);
        assert(st.coarse_frames==3516*4);
        if(run==4)assert(st.clips>0);
    }
    synth_iq=false;read_cost_us=700;fake_backlog=0;rec_catch_stop();
}
LS_CASE(rtl_only_clipped_packets_at_sparse_cadence)
{
    if(!s_lock)s_lock=xSemaphoreCreateMutex();
    memset(&s_status,0,sizeof(s_status));memset(s_result,0,sizeof(s_result));
    memset(&s_sources,0,sizeof(s_sources));
    s_status.center=915000000;s_head=0;s_has_pending=false;s_rtl_epoch=0;
    chip=LS_LORA_CHIP_SX126X;int leases=lease_attempts;
    catch_detector_init(&s_detector,915000000);
    catch_fixture_t f={.random=0x76123456,.re=1,.amplitude=450,
        .delay_samples=3ull*CATCH_RATE,.comb=true};
    unsigned seen[8]={0};bool warmed=false;
    for(unsigned block=0;block<4102;block++) {
        fixture_block(&f,s_iq,sizeof(s_iq));
        uint64_t now=f.sample*1000000/CATCH_RATE;
        ls_shim_time_set(now);
        /* Full FFT only every other USB block: 13.65 ms observations,
           including budget skips, with every byte still consumed. */
        catch_detector_allow(&s_detector,block%2);
        catch_detector_iq(&s_detector,s_iq,sizeof(s_iq),now,detection,NULL);
        if(!warmed && now>=3000000) {
            memset(&s_status,0,sizeof(s_status));memset(s_result,0,sizeof(s_result));
            s_status.center=915000000;s_head=0;warmed=true;
        }
    }
    printf("RTL-only clipped comb: %lu rows, accepted %lu short %lu wide %lu clips %llu FFT %lu\n",
        (unsigned long)s_status.detections,(unsigned long)s_detector.reasons[CATCH_ACCEPTED],
        (unsigned long)s_detector.reasons[CATCH_TOO_SHORT],(unsigned long)s_detector.reasons[CATCH_TOO_WIDE],
        (unsigned long long)s_detector.clips,(unsigned long)s_detector.frames);
    fflush(stdout);
    assert(s_status.detections==8 && s_status.count==8);
    assert(s_detector.clips>60000 && !s_detector.reasons[CATCH_TOO_WIDE]);
    assert(!s_has_pending && !s_status.handoffs && !s_status.captures && lease_attempts==leases);
    for(int i=0;i<8;i++) {
        catch_result_t row;assert(rec_catch_result(i,&row));
        printf("  packet %llu us: %lu Hz %.1f dBFS %s\n",(unsigned long long)row.burst.start_us,
            (unsigned long)row.burst.frequency,(double)row.burst.rtl_dbfs,row.decode_status);
        fflush(stdout);
        assert(abs((int)row.burst.frequency-915600000)<3000);
        unsigned packet=(unsigned)((row.burst.start_us-4000000)/3000000);
        assert(packet<8);seen[packet]++;
        assert(!strcmp(row.decode_status,"LR2021 absent (SX1262 has no stream)"));
        detection(&row.burst,true,NULL); /* Callback retry cannot duplicate. */
    }
    for(int i=0;i<8;i++)assert(seen[i]==1);
    assert(s_status.count==8 && s_status.detections==8);
    char *list[]={"catch","list"};console_used=0;check_diagnostics=true;
    assert(rec_catch_command(2,list)==0);check_diagnostics=false;
    unsigned reasons=0;
    for(char *p=console_text;(p=strstr(p,"LR2021 absent (SX1262 has no stream)"));p++)reasons++;
    assert(reasons==8);
    chip=LS_LORA_CHIP_LR20XX;
}

/* Older adapter snapshots do not expose the learned-row filter. */
#ifdef CATCH_CURRENT_ADAPTER
LS_CASE(rtl_only_full_rate_wandering_spurs)
{
    if(!s_lock)s_lock=xSemaphoreCreateMutex();
    memset(&s_status,0,sizeof(s_status));memset(s_result,0,sizeof(s_result));
    memset(&s_sources,0,sizeof(s_sources));s_status.center=915000000;
    s_head=0;s_has_pending=false;s_rtl_epoch=0;chip=LS_LORA_CHIP_SX126X;
    int leases=lease_attempts;
    catch_detector_init(&s_detector,915000000);
    catch_fixture_t f={.random=0x615193,.re=1,.amplitude=12,.delay_samples=11ull*CATCH_RATE};
    double re[4]={1,1,1,1},im[4]={0};unsigned seen[8]={0};
    for(unsigned block=0;f.sample<45ull*CATCH_RATE;block++) {
        size_t n=17242;fixture_block(&f,s_iq,n);
        fixture_wandering_spurs(s_iq,n,block,re,im);
        uint64_t now=f.sample*1000000/CATCH_RATE;ls_shim_time_set(now);
        catch_detector_allow(&s_detector,1);
        catch_detector_iq(&s_detector,s_iq,n,now,detection,NULL);
        lock();hide_learned_spurs();unlock();
    }
    assert(s_status.count==8 && !s_has_pending && !s_status.handoffs && lease_attempts==leases);
    for(int j=0;j<8;j++) {
        catch_result_t row;assert(rec_catch_result(j,&row));
        assert(abs((int)row.burst.frequency-915600000)<3000 && !row.burst.spur);
        unsigned packet=(unsigned)((row.burst.start_us-12000000)/3000000);
        assert(packet<8);seen[packet]++;
        assert(!strcmp(row.decode_status,"LR2021 absent (SX1262 has no stream)"));
    }
    for(unsigned j=0;j<8;j++)assert(seen[j]==1);
    puts("full-rate wandering spurs: exactly eight RTL-only list rows, no socket lease");
    chip=LS_LORA_CHIP_LR20XX;
}

LS_CASE(learned_spurs_leave_no_list_rows)
{
    if(!s_lock)s_lock=xSemaphoreCreateMutex();
    memset(&s_status,0,sizeof(s_status));memset(s_result,0,sizeof(s_result));
    s_head=0;s_has_pending=false;chip=LS_LORA_CHIP_SX126X;
    catch_detector_init(&s_detector,915000000);
    catch_burst_t b={.id=1,.frequency=914200000,.rtl_dbfs=-35,.start_us=100000,.stop_us=147000};
    detection(&b,true,NULL);
    b.id=2;b.frequency=915600000;b.start_us=4000000;b.stop_us=4047000;
    detection(&b,true,NULL);
    assert(s_status.count==2);
    static float spectrum[CATCH_BINS];
    for(int i=0;i<CATCH_BINS;i++)spectrum[i]=-70;
    spectrum[85]=-35;
    for(unsigned t=0;t<2000000;t+=3592)catch_detector_spectrum(&s_detector,spectrum,t,NULL,NULL);
    lock();hide_learned_spurs();unlock();
    assert(s_status.count==1 && s_status.detections==2);
    catch_result_t row;assert(rec_catch_result(0,&row) && row.burst.id==2);
    assert(!rec_catch_result(1,&row));
    char *list[]={"catch","list"};console_used=0;console_text[0]=0;check_diagnostics=true;
    assert(rec_catch_command(2,list)==0);check_diagnostics=false;
    assert(strstr(console_text,"915.600000 MHz") && !strstr(console_text,"914.200000 MHz"));
    b.id=3;b.spur=true;detection(&b,true,NULL);assert(s_status.count==1);
    /* Reuse hidden slots on wrap without shifting a socket/save slot. */
    for(unsigned j=0;j<CATCH_RESULTS;j++) {
        b.id=4+j;b.spur=false;b.start_us+=3000000;b.stop_us=b.start_us+47000;
        detection(&b,true,NULL);
    }
    assert(s_status.count==CATCH_RESULTS);
    for(int j=0;j<CATCH_RESULTS;j++)assert(rec_catch_result(j,&row) && !row.burst.spur);
    chip=LS_LORA_CHIP_LR20XX;
}

#endif

LS_CASE(console_diagnostics_snapshot)
{
    if(!s_lock)s_lock=xSemaphoreCreateMutex();
    memset(&s_status,0,sizeof(s_status));
    s_status.actual_center=915000000;s_status.sample_rate=CATCH_RATE;
    s_status.gain_mode=LS_RADIO_GAIN_MANUAL;s_status.gain_tenths_db=402;
    s_status.floor_dbfs=-57;s_status.threshold_dbfs=-45;s_status.clips=123;
    s_status.iq_rms=0.123f;s_status.peak_count=1;
    s_status.peaks[0]=(catch_peak_t){.first_us=10000,.last_us=15000,.peak_us=12000,
        .frequency=915600000,.snr_db=24,.dbfs=-33,.reason=CATCH_TOO_SHORT};
    s_status.recent[0]=s_status.peaks[0];
    s_status.reasons[CATCH_TOO_SHORT]=1;
    s_status.spur_count=1;s_status.spurs[0]=(catch_spur_info_t){914200000,-38,true};
    ls_shim_time_set(20000);
    char *stats[]={"catch","stats"},*peaks[]={"catch","peaks"},*spurs[]={"catch","spurs"};
    console_used=0;console_text[0]=0;check_diagnostics=true;
    assert(rec_catch_command(2,stats)==0 && rec_catch_command(2,peaks)==0);
    assert(rec_catch_command(2,spurs)==0);
    check_diagnostics=false;
    assert(strstr(console_text,"floor median -57.0 dBFS threshold median -45.0"));
    assert(strstr(console_text,"centre 915000000 Hz rate 2400000 sps gain manual 40.2 dB"));
    assert(strstr(console_text,"IQ RMS 0.12300 clips 123"));
    assert(strstr(console_text,"rejected short 1 long 0 wide 0 margin 0 mask 0 merged 0"));
    assert(strstr(console_text,"peak last 10 s SNR 24.0 dB 915600000 Hz at 12 ms age 8 ms"));
    assert(strstr(console_text,"peak 10..15 ms 915600000 Hz SNR 24.0 dB -33.0 dBFS too short"));
    assert(strstr(console_text,"learned spurs 1 suppressed 0; top 914200000 Hz comb"));
    assert(strstr(console_text,"spur 914200000 Hz -38.0 dBFS comb family"));
    puts("CATCH stats and peaks diagnostics OK");
}

LS_CASE(board_cost_cadence_and_fft_blind_tail)
{
    if(!s_lock)s_lock=xSemaphoreCreateMutex();
    bool packets_ok=true;
    for(unsigned blind=0;blind<2;blind++) {
        memset(&s_status,0,sizeof(s_status));memset(s_result,0,sizeof(s_result));
        memset(&s_sources,0,sizeof(s_sources));
        s_head=0;s_has_pending=false;s_run=true;s_usb_drops=0;s_status.center=915000000;
        chip=LS_LORA_CHIP_SX126X;
        synth_iq=board_clock=true;read_error=LS_RADIO_OK;read_size=sizeof(s_iq);
        /* 353 us read + 640 us scan/status, plus 4*21 checks and 919+815 FFT.
           The timer charges scan here so cooldown sees the complete cost. */
        read_cost_us=993;fake_drops=0;fake_backlog=0;
        fixture=(catch_fixture_t){.random=0x76123456,.re=1,.amplitude=450,
            .delay_samples=3ull*CATCH_RATE,.comb=true};
        catch_detector_init(&s_detector,915000000);s_detector.clock_us=board_detector_clock;
        clock_checks=clock_frames=0;fft_clock_started=false;
        s_full_allowance=1;
#ifdef CATCH_CONTINUOUS_IQ
        s_status_due=0;
#else
        s_frame_cost=1734;s_check_cost=1077;
#endif
        ls_shim_time_set(1000000);s_rtl_epoch=1000000;
        unsigned skipped=0,cap_skipped=0,last_frames=0;
        uint64_t blind_until=0;
        for(unsigned block=0;block<4102;block++) {
            uint64_t available=1000000+(fixture.sample+sizeof(s_iq)/2)*1000000/CATCH_RATE;
            if((uint64_t)esp_timer_get_time()+read_cost_us<available)
                ls_shim_time_set(available-read_cost_us);
            uint64_t sample_time=1000000+fixture.sample*1000000/CATCH_RATE;
            /* Worst cap phase: after the first refined packet observation,
               exhaust this wall-clock bucket through the tail and silence.
               Every IQ block is still consumed by the actual worker. */
            if(blind && sample_time<blind_until) {
                s_detector.budget_second=(uint64_t)esp_timer_get_time()/1000000;
                s_detector.second_frames=CATCH_FULL_PER_SECOND;
            }
            unsigned before=s_detector.frames;
            assert(rtl_block(NULL)==LS_RADIO_OK);
            if(s_detector.frames==before) {skipped++;if(sample_time<blind_until)cap_skipped++;}
            if(blind && sample_time>=blind_until) {
                for(int j=0;j<CATCH_TRACKS;j++)if(s_detector.track[j].used &&
                    abs((int)s_detector.track[j].burst.frequency-915600000)<15000) {
                    blind_until=sample_time+120000;break;
                }
            }
            assert(s_detector.second_frames<=CATCH_FULL_PER_SECOND);
            last_frames=s_detector.frames;
        }
        printf("board cost%s: rows %lu FFT %u skipped %u cap-tail %u over-budget %lu duty %.2f%% wall/block %.1f us\n",
            blind?" + blind tail":"",(unsigned long)s_status.detections,last_frames,skipped,cap_skipped,
            (unsigned long)s_status.over_budget,100.0*s_status.active_us/s_status.elapsed_us,
            (double)s_status.elapsed_us/s_status.blocks);fflush(stdout);
        packets_ok=packets_ok && s_status.detections==8 && s_status.count==8;
        assert(skipped>0 && (!blind || cap_skipped>80));
        assert(s_status.active_us*100<=s_status.elapsed_us*35 && !s_status.block_drops);
        unsigned seen[8]={0};
        for(int i=0;i<s_status.count;i++) {
            catch_result_t row;assert(rec_catch_result(i,&row));
            uint64_t duration=row.burst.stop_us-row.burst.start_us;
            printf("  board packet %llu duration %llu Hz %lu\n",(unsigned long long)row.burst.start_us,
                (unsigned long long)duration,(unsigned long)row.burst.frequency);
            packets_ok=packets_ok && duration>=35000 && duration<=55000;
            unsigned packet=(unsigned)((row.burst.start_us-5000000)/3000000);
            assert(packet<8);seen[packet]++;
            assert(abs((int)row.burst.frequency-915600000)<15000);
            assert(row.burst.detected_us>row.burst.stop_us);
            assert(!strcmp(row.decode_status,"LR2021 absent (SX1262 has no stream)"));
        }
        for(int i=0;i<8;i++)packets_ok=packets_ok && seen[i]==1;
        fflush(stdout);
        assert(!s_has_pending && !s_status.handoffs && !s_status.captures);
    }
    synth_iq=board_clock=false;read_cost_us=700;fake_backlog=0;chip=LS_LORA_CHIP_LR20XX;
    rec_catch_stop();
    assert(packets_ok); /* Both the cadence and blind-tail regressions run. */
}

#ifdef CATCH_QUIET_BLOCKS
LS_CASE(console_trace_bounded_and_expires)
{
    if(!s_lock)s_lock=xSemaphoreCreateMutex();
    ls_shim_time_set(1000000);s_trace_lines=s_trace_suppressed=0;s_trace_second=0;
    char *trace[]={"catch","trace","2"},*off[]={"catch","trace","0"};
    char *bad[]={"catch","trace","-1"},*junk[]={"catch","trace","1x"};
    assert(rec_catch_command(3,bad)==1 && rec_catch_command(3,junk)==1);
    console_used=0;check_diagnostics=true;
    assert(rec_catch_command(3,trace)==0);
    catch_trace_t t={.block_us=1234567,.id=7,.frequency=915600000,.coarse_db=31,
        .bin=24,.fft="skipped cap",.state="end",.reason="quiet blocks"};
    for(int i=0;i<45;i++) {
        trace_candidate(&t,NULL);
#ifdef CATCH_CONTINUOUS_IQ
        if(i<40)assert(trace_drain());
#endif
    }
    assert(s_trace_suppressed==5);
#ifdef CATCH_CONTINUOUS_IQ
    assert(strstr(console_text,"period=") && strstr(console_text,"gap="));
#endif
    assert(strstr(console_text,"block=1234567 us id=7 coarse=31.0 dB/floor bin=24 freq=915600000 FFT=skipped cap state=end reason=quiet blocks"));
    ls_shim_time_set(2000000);trace_candidate(&t,NULL);
#ifdef CATCH_CONTINUOUS_IQ
    assert(trace_drain());
#endif
    assert(strstr(console_text,"suppressed 5 lines"));
    size_t used=console_used;ls_shim_time_set(3000000);trace_candidate(&t,NULL);
    assert(console_used==used);
    assert(rec_catch_command(3,off)==0 && !s_trace_until);
#ifdef CATCH_CONTINUOUS_IQ
    assert(rec_catch_command(3,trace)==0);
    used=console_used;
    for(unsigned i=0;i<TRACE_QUEUE+2;i++)trace_candidate(&t,NULL);
    assert(console_used==used && s_trace_count==TRACE_QUEUE && s_trace_suppressed==2);
    /* Stalled console never printed or blocked in the producer. Stop clears it. */
    rec_catch_stop();assert(!trace_drain());
#endif
    check_diagnostics=false;
    puts("CATCH trace format, 40 lines/s, suppression count, expiry and disable OK");
}
#endif

LS_CASE(finite_producer_board_cost)
{
    if(!s_lock)s_lock=xSemaphoreCreateMutex();
    for(unsigned size=16384;size<=32768;size*=2) {
    read_size=size;
    memset(&s_status,0,sizeof(s_status));memset(s_result,0,sizeof(s_result));
    memset(&s_sources,0,sizeof(s_sources));
    s_head=0;s_has_pending=false;s_run=true;s_usb_drops=0;s_status.center=915000000;
    chip=LS_LORA_CHIP_SX126X;finite_stream=board_clock=true;
    consumer_block=producer_blocks=model_max_gap=model_last_read=model_overwrites=0;
    fake_drops=fake_backlog=0;read_cost_us=993;
    fixture=(catch_fixture_t){.random=0x76123456,.re=1,.amplitude=450,
        .delay_samples=3ull*CATCH_RATE,.comb=true};
    catch_detector_init(&s_detector,915000000);s_detector.clock_us=board_detector_clock;
    clock_checks=clock_frames=0;fft_clock_started=false;s_full_allowance=1;
#ifdef CATCH_CONTINUOUS_IQ
    s_status_due=0;
#else
    s_frame_cost=1734;s_check_cost=1077;
#endif
    ls_shim_time_set(1000000);s_rtl_epoch=1000000;
    while((uint64_t)esp_timer_get_time()<29000000) {
        uint64_t next=1000000+(producer_blocks+1)*read_size*500000/CATCH_RATE;
        /* Empty-read wait is idle time, outside the adapter's active timer. */
        if(consumer_block==producer_blocks && (uint64_t)esp_timer_get_time()<next)
            ls_shim_time_set(next);
        assert(rtl_block(NULL)==LS_RADIO_OK);
    }
    printf("finite producer %u bytes: %lu/8 packets, %lu blocks, %llu overwrites, max gap %llu us, %.3f MB/s\n",
        size,(unsigned long)s_status.detections,(unsigned long)s_status.blocks,
        (unsigned long long)model_overwrites,(unsigned long long)model_max_gap,
        s_status.blocks*(double)read_size/s_status.elapsed_us);fflush(stdout);
    uint64_t period=(read_size*500000ull+CATCH_RATE-1)/CATCH_RATE;
#ifdef CATCH_CONTINUOUS_IQ
    assert(s_status.detections==8 && !model_overwrites && model_max_gap<=period);
    assert(s_status.max_block_gap_us<=period && !s_status.lost_samples);
    assert(s_status.processed_bytes>=(uint64_t)(s_status.elapsed_us*4.79));
    assert(s_status.coarse_frames==s_status.blocks*(size/8192));
    char *stats[]={"catch","stats"};rec_catch_command(2,stats);
#else
    assert(s_status.detections==0 && model_overwrites>0 && model_max_gap>period);
#endif
    }
    finite_stream=board_clock=false;chip=LS_LORA_CHIP_LR20XX;fake_backlog=0;
    read_cost_us=700;rec_catch_stop();
}
