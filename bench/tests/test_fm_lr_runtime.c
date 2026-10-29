/* Production FM callback, with a host speaker sink and no hardware. */
#include "ls_test.h"
#include "../../components/lakeshark/apps/fm/app_fm.c"
static ls_radio_endpoint_info_t endpoint;
static bool endpoint_available,calls_busy,scanner_active;
static unsigned actual_retunes;
static bool inject_tune_start,inject_gain_start,injected_start_result;
const app_t *app_current(void){return NULL;}
void settings_set_freq_mode(const app_t *a,int mode,uint32_t hz){(void)a;(void)mode;(void)hz;}
void settings_set_gain(const app_t *a,int gain){(void)a;(void)gain;}
int scan_engine_get_zone(void)
{if(inject_tune_start){inject_tune_start=false;injected_start_result=fm_rx_plan_start();}return 0;}
int scan_channels_count(void){return 0;}
const scan_channel_t *scan_channel_get(int i){(void)i;return NULL;}
int ls_rsel_gain_max(ls_rsel_job_t job)
{(void)job;if(inject_gain_start){inject_gain_start=false;injected_start_result=fm_rx_plan_start();}return 500;}
static bool archive_ready,archive_init_failure;
static int archive_options[CALL_OPTIONS]={1,1,500,30,0,0};
void call_archive_init(void){if(!archive_init_failure)archive_ready=true;}
bool call_archive_ready(void){return archive_ready;}
int call_archive_option(call_option_t option){return archive_options[option];}
void call_archive_set_option(call_option_t option,int value){archive_options[option]=value;}
bool scan_engine_active(void){return scanner_active;}
bool call_archive_busy(void){return calls_busy;}
ls_radio_err_t ls_radio_endpoint_try_get(const char *id,ls_radio_endpoint_info_t *out)
{if(!endpoint_available||strcmp(id,LS_RADIO_ENDPOINT_RTL_USB))return LS_RADIO_ERR_UNAVAILABLE;*out=endpoint;return LS_RADIO_OK;}
__attribute__((externally_visible)) ls_radio_err_t ls_radio_iq_retune(ls_radio_session_t *session,uint64_t hz,bool fast,uint64_t *actual)
{
    (void)fast;if(session!=s_session)return LS_RADIO_ERR_BUSY;
    actual_retunes++;endpoint.actual_iq.center_hz=hz;endpoint.diagnostic_context_epoch++;
    *actual=hz;return LS_RADIO_OK;
}
__attribute__((externally_visible)) ls_radio_err_t ls_radio_iq_configure(ls_radio_session_t *s,const ls_radio_iq_config_t *in,ls_radio_iq_config_t *out)
{(void)s;(void)in;(void)out;return LS_RADIO_ERR_UNSUPPORTED;}
__attribute__((externally_visible)) ls_radio_err_t ls_radio_iq_set_gain(ls_radio_session_t *s,ls_radio_gain_mode_t mode,int gain,int *actual)
{if(s!=s_session)return LS_RADIO_ERR_BUSY;endpoint.actual_iq.gain_mode=mode;endpoint.actual_iq.gain_tenths_db=gain;
endpoint.diagnostic_context_epoch++;*actual=gain;return LS_RADIO_OK;}
static uint32_t drops;
static int writes, peak;
static bool drop_audio;
uint32_t audio_drops_get(void) { return drops; }
void audio_write_mono(const int16_t *p, int n)
{
    writes++;
    if (drop_audio) { drops++; return; }
    for (int i=0;i<n;i++) { int a=abs(p[i]); if(a>peak)peak=a; }
}
bool scan_engine_audio_open(void) { return true; }
LS_CASE(fm_observation_counts_only_supported_dsp_delivery_segments)
{
    s_observation=(ls_iq_observation_t){0};
    s_observation_rate=256000; s_observation_center=154785000;
    s_observation_config_valid=true; FM.mode=FM_MODE_LISTEN;
    fm_observation_delivered(100);
    ls_iq_observation_t snapshot;
    fm_get_iq_observation(&snapshot);
    LS_CHECK(snapshot.known && snapshot.delivered_bytes==FM_IQ_BLOCK_BYTES);
    LS_CHECK(snapshot.first_delivery_us==100 && snapshot.generation==1);
    fm_observation_boundary(LS_IQ_OBS_TUNE);
    s_observation_config_valid=false; /* pending/failed tune cannot earn credit */
    fm_observation_delivered(200);
    fm_get_iq_observation(&snapshot);
    LS_CHECK(!snapshot.accepting && snapshot.delivered_bytes==FM_IQ_BLOCK_BYTES);
    LS_CHECK(snapshot.state==LS_IQ_OBS_TUNE);
    s_observation_config_valid=true; FM.mode=FM_MODE_SCAN;
    fm_observation_delivered(300); /* sweep/settling path is excluded */
    fm_get_iq_observation(&snapshot);
    LS_CHECK(!snapshot.accepting && snapshot.generation==1);
    FM.mode=FM_MODE_LISTEN;
    fm_observation_delivered(400);
    fm_get_iq_observation(&snapshot);
    LS_CHECK(snapshot.accepting && snapshot.generation==2);
    LS_CHECK(snapshot.delivered_bytes==FM_IQ_BLOCK_BYTES && snapshot.first_delivery_us==400);
    fm_observation_boundary(LS_IQ_OBS_READ_ERROR);
    fm_get_iq_observation(&snapshot);
    LS_CHECK(!snapshot.accepting && snapshot.state==LS_IQ_OBS_READ_ERROR);
    fm_observation_boundary(LS_IQ_OBS_STOPPED);
    fm_get_iq_observation(&snapshot);
    LS_CHECK(!snapshot.accepting && snapshot.state==LS_IQ_OBS_STOPPED);
    s_observation_config_valid=false;
}
LS_CASE(fm_mission_allocates_only_psram_on_operator_start_and_records_owner_hooks)
{
    if (s_mission) { heap_caps_free(s_mission); s_mission = NULL; }
    ls_shim_heap_reset(); ls_shim_heap_fail_from(1);
    LS_CHECK(!fm_mission_start("allocation failure") && !s_mission);
    LS_CHECK(ls_shim_heap_call_caps(1) == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    LS_CHECK(ls_shim_heap_call_size(1) <= 4096);
    ls_shim_heap_reset();
    s_observation = (ls_iq_observation_t){0};
    s_observation_rate=256000; s_observation_center=154785000;
    s_observation_config_valid=true; FM.mode=FM_MODE_LISTEN;
    fm_observation_delivered(100); /* Before the operator session. */
    ls_shim_time_set(200); LS_CHECK(fm_mission_start("field"));
    LS_CHECK(ls_shim_heap_call_count() == 1);
    fm_observation_delivered(300);
    ls_shim_time_set(400); fm_observation_boundary(LS_IQ_OBS_TUNE);
    FM.mode=FM_MODE_SCAN; fm_observation_delivered(450); /* Excluded path. */
    FM.mode=FM_MODE_LISTEN; s_observation_center=162550000;
    fm_observation_delivered(500);
    ls_shim_time_set(600); LS_CHECK(fm_mission_stop());
    fm_observation_delivered(700); /* After the operator session. */
    ls_iq_mission_status_t status;
    ls_iq_mission_entry_t entries[5]; size_t n;
    LS_CHECK(fm_mission_snapshot(&status, entries, 5, &n));
    LS_CHECK(!status.active && n == 5 && status.delivered_blocks == 2);
    LS_CHECK(entries[1].at_us == 300 && entries[2].at_us == 400);
    LS_CHECK(entries[2].kind == LS_IQ_MISSION_BOUNDARY && entries[2].state == LS_IQ_OBS_TUNE);
    LS_CHECK(entries[3].configured_center_hz == 162550000);
    LS_CHECK(ls_shim_heap_call_count() == 1); /* No hot-path allocation. */
    ls_shim_heap_fail_from(2); LS_CHECK(fm_mission_start("reuse"));
    LS_CHECK(ls_shim_heap_call_count() == 1 && fm_mission_status(&status));
    LS_CHECK(status.mission_id == 2 && status.active);
    LS_CHECK(fm_mission_stop()); heap_caps_free(s_mission); s_mission = NULL;
    s_observation_config_valid=false; ls_shim_heap_reset();
}
static void scheduler_cleanup(void)
{
    if(s_rx_plan)heap_caps_free(s_rx_plan);
    s_rx_plan=NULL;
    if(s_coverage)heap_caps_free(s_coverage);
    s_coverage=NULL;
    s_plan_request_pending=s_plan_coverage_owned=s_plan_calls_suppressed=s_plan_control_inflight=false;
    s_plan_manual_epoch=s_plan_owner_epoch=0;s_plan_context_epoch=0;
    s_plan_manual_publishers=0;inject_tune_start=inject_gain_start=false;injected_start_result=false;
    s_session=s_plan_session=NULL;s_session_endpoint=NULL;s_lr_session=false;
    endpoint_available=false;calls_busy=scanner_active=false;s_frequency_locked=false;
    fm_lifecycle_test_reset();
}
static void scheduler_ready(void)
{
    scheduler_cleanup();fm_lifecycle_test_reset();LS_CHECK(fm_lifecycle_start());
    ls_shim_time_set(1000);s_session=(ls_radio_session_t *)(uintptr_t)0x1234;s_session_endpoint=LS_RADIO_ENDPOINT_RTL_USB;
    memset(&endpoint,0,sizeof(endpoint));strcpy(endpoint.endpoint_id,LS_RADIO_ENDPOINT_RTL_USB);strcpy(endpoint.owner,"fm");
    endpoint.present=endpoint.leased=endpoint.streaming=endpoint.configured=endpoint.diagnostic_context_known=true;
    endpoint.tuner_model_known=true;endpoint.actual_iq.center_hz=154785000;endpoint.actual_iq.sample_rate_hz=FM_RTL_RATE;
    endpoint.diagnostic_context_epoch=10;endpoint.frequency_range_count=1;
    endpoint.frequency_ranges[0]=(ls_radio_range_t){150000000,170000000};endpoint_available=true;
    FM.mode=FM_MODE_LISTEN;FM.freq_hz=154785000;s_mode_handoff.pending_mode=-1;
    ls_iq_control_reset(&s_radio_control);ls_iq_control_set_initial(&s_radio_control,FM.freq_hz,200,0);
    ls_iq_control_note_tune_result(&s_radio_control,FM.freq_hz,LS_RADIO_OK,FM.freq_hz);
    ls_iq_control_set_streaming(&s_radio_control,true,LS_RADIO_OK);actual_retunes=0;
}
static void scheduler_apply(void)
{
    ls_iq_control_request_t request;LS_CHECK(ls_iq_control_take(&s_radio_control,&request));
    LS_CHECK(fm_rx_plan_control_allowed(&request));fm_observation_boundary(LS_IQ_OBS_TUNE);
    ls_radio_err_t e=ls_iq_control_apply_tune(&s_radio_control,s_session,&request);
    fm_rx_plan_tune_result(&request,e);LS_CHECK(e==LS_RADIO_OK);
}
static uint64_t scheduler_settle(uint64_t base)
{
    ls_radio_iq_read_receipt_t r={.known=true,.context_epoch=endpoint.diagnostic_context_epoch,
        .configured_center_hz=endpoint.actual_iq.center_hz,.nominal_rate_hz=FM_RTL_RATE,
        .transport={.known=true,.fifo_reset_known=true,.epoch=endpoint.diagnostic_context_epoch}};
    r.read_started_us=base+1000;r.read_finished_us=base+2000;
    LS_CHECK(!ls_iq_coverage_accept(s_coverage,&r,FM_IQ_BLOCK_BYTES));
    r.read_started_us=base+50000;r.read_finished_us=base+51000;
    LS_CHECK(!ls_iq_coverage_accept(s_coverage,&r,FM_IQ_BLOCK_BYTES));
    LS_CHECK(s_rx_plan->status.state==LS_RX_PLAN_SETTLING);
    r.read_started_us=base+103000;r.read_finished_us=base+104000;
    LS_CHECK(ls_iq_coverage_accept(s_coverage,&r,FM_IQ_BLOCK_BYTES));
    fm_coverage_block_done(base+104100,true);
    LS_CHECK(s_rx_plan->status.state==LS_RX_PLAN_DWELLING);return base+104100;
}
LS_CASE(fm_scheduler_executes_real_control_queue_post_guard_dwell_and_restore)
{
    scheduler_ready();ls_rx_plan_step_t steps[]={{162550000,1000},{162400000,1000}};
    LS_CHECK(fm_rx_plan_load("actual wrapper",steps,2,1));calls_busy=true;
    LS_CHECK(!fm_rx_plan_start());calls_busy=false;LS_CHECK(fm_rx_plan_start());
    LS_CHECK(s_plan_calls_suppressed);fm_rx_plan_poll();scheduler_apply();
    LS_CHECK(actual_retunes==1 && endpoint.actual_iq.center_hz==162550000);
    uint64_t first=scheduler_settle(1000);
    for(unsigned i=1;i<=100;++i) {
        uint64_t at=first+i*1000;
        ls_radio_iq_read_receipt_t r={.known=true,.context_epoch=endpoint.diagnostic_context_epoch,
            .configured_center_hz=endpoint.actual_iq.center_hz,.nominal_rate_hz=FM_RTL_RATE,
            .read_started_us=at,.read_finished_us=at+100,
            .transport={.known=true,.fifo_reset_known=true,.epoch=endpoint.diagnostic_context_epoch}};
        LS_CHECK(ls_iq_coverage_accept(s_coverage,&r,FM_IQ_BLOCK_BYTES));fm_coverage_block_done(at+200,true);
    }
    LS_CHECK(s_coverage->status.overwritten_intervals>0 && s_rx_plan->status.detail_truncated);
    ls_shim_time_set((int64_t)(first+1000000));
    fm_rx_plan_poll();scheduler_apply();LS_CHECK(actual_retunes==2 && endpoint.actual_iq.center_hz==162400000);
    uint64_t second=scheduler_settle(first+1000000);ls_shim_time_set((int64_t)(second+1000000));
    fm_rx_plan_poll();scheduler_apply();
    LS_CHECK(actual_retunes==3 && endpoint.actual_iq.center_hz==154785000 && FM.freq_hz==154785000);
    LS_CHECK(s_rx_plan->status.state==LS_RX_PLAN_COMPLETED && s_rx_plan->status.restored);
    LS_CHECK(!s_plan_calls_suppressed && !s_coverage->status.active && s_coverage->status.delivered_blocks==102);
    LS_CHECK(s_rx_plan->status.steps[0].delivered_blocks==101 && s_rx_plan->status.steps[1].delivered_blocks==1);
    LS_CHECK(s_rx_plan->status.steps[0].delivered_bytes==101*FM_IQ_BLOCK_BYTES && s_rx_plan->status.steps[0].software_window_us==21100);
    uint64_t nominal=0;LS_CHECK(ls_rx_plan_nominal_us(&s_rx_plan->status.steps[0],&nominal)&&nominal==3232000);
    LS_CHECK(!s_coverage->status.calls_suppressed);
    scheduler_cleanup();
}
LS_CASE(fm_scheduler_manual_override_cancels_admitted_plan_control_without_restore)
{
    scheduler_ready();ls_rx_plan_step_t step={162550000,1000};
    LS_CHECK(fm_rx_plan_load("cancel",&step,1,1)&&fm_rx_plan_start());fm_rx_plan_poll();
    ls_iq_control_request_t request;LS_CHECK(ls_iq_control_take(&s_radio_control,&request));
    lakeshark_fm_frequency_lock(true);
    LS_CHECK(!fm_rx_plan_control_allowed(&request) && actual_retunes==0);
    LS_CHECK(FM.freq_hz==154785000 && s_radio_control.tune_state==LS_IQ_RESULT_EFFECTIVE);
    LS_CHECK(s_rx_plan->status.state==LS_RX_PLAN_CANCELLED && !s_rx_plan->status.restored && !s_plan_calls_suppressed);
    scheduler_cleanup();
}
LS_CASE(fm_scheduler_cancel_before_take_removes_only_its_pending_generation)
{
    scheduler_ready();ls_rx_plan_step_t step={162550000,1000};
    LS_CHECK(fm_rx_plan_load("queue cancel",&step,1,1)&&fm_rx_plan_start());fm_rx_plan_poll();
    fm_rx_plan_intervene();ls_iq_control_request_t request;
    LS_CHECK(!ls_iq_control_take(&s_radio_control,&request));LS_CHECK(actual_retunes==0 && FM.freq_hz==154785000);
    fm_rx_plan_poll();LS_CHECK(!s_plan_request_pending);
    ls_iq_control_request_tune(&s_radio_control,162400000,false);
    LS_CHECK(!ls_iq_control_cancel_tune(&s_radio_control,s_plan_request_generation,NULL));
    LS_CHECK(ls_iq_control_take(&s_radio_control,&request)&&request.center_hz==162400000);
    scheduler_cleanup();
}
LS_CASE(fm_scheduler_rejects_start_inside_real_manual_tune_and_gain_publication)
{
    scheduler_ready();ls_rx_plan_step_t step={162550000,1000};LS_CHECK(fm_rx_plan_load("publication",&step,1,1));
    inject_tune_start=true;injected_start_result=true;
    LS_CHECK(lakeshark_fm_set_freq(162400000));
    LS_CHECK(!injected_start_result && !s_plan_manual_publishers && !s_rx_plan->status.active);
    LS_CHECK(!fm_rx_plan_start()); /* Publication ended; manual tune still pending. */
    ls_iq_control_request_t request;LS_CHECK(ls_iq_control_take(&s_radio_control,&request));
    LS_CHECK(request.center_hz==162400000 && actual_retunes==0);
    LS_CHECK(ls_iq_control_apply_tune(&s_radio_control,s_session,&request)==LS_RADIO_OK);
    inject_gain_start=true;injected_start_result=true;lakeshark_fm_set_gain(300);
    LS_CHECK(!injected_start_result && !s_plan_manual_publishers && !fm_rx_plan_start());
    LS_CHECK(ls_iq_control_take(&s_radio_control,&request) && request.flags==LS_IQ_CONTROL_GAIN);
    LS_CHECK(ls_iq_control_apply_gain(&s_radio_control,s_session,&request)==LS_RADIO_OK);
    LS_CHECK(actual_retunes==1 && FM.freq_hz==162400000 && FM.gain_tenths==300);
    LS_CHECK(fm_rx_plan_start()); /* A later explicit start may use settled manual settings. */
    scheduler_cleanup();
}
LS_CASE(fm_scheduler_inflight_tune_or_restore_keeps_newer_manual_request_and_policy)
{
    for(int restoring=0;restoring<2;++restoring) {
        scheduler_ready();ls_rx_plan_step_t step={162550000,1000};LS_CHECK(fm_rx_plan_load("inflight",&step,1,1)&&fm_rx_plan_start());
        fm_rx_plan_poll();
        if(restoring) {
            scheduler_apply();ls_rx_plan_qualified(s_rx_plan,1001);
            LS_CHECK(fm_rx_plan_stop());ls_shim_time_set(1002);fm_rx_plan_poll();
            LS_CHECK(s_rx_plan->status.state==LS_RX_PLAN_RESTORING);
        }
        ls_iq_control_request_t old;LS_CHECK(ls_iq_control_take(&s_radio_control,&old)&&fm_rx_plan_control_allowed(&old));
        LS_CHECK(s_plan_control_inflight);LS_CHECK(lakeshark_fm_set_freq(162400000));
        LS_CHECK(!s_rx_plan->status.active && !s_plan_calls_suppressed);
        LS_CHECK(ls_iq_control_apply_tune(&s_radio_control,s_session,&old)==LS_RADIO_OK);
        fm_rx_plan_tune_result(&old,LS_RADIO_OK);
        LS_CHECK(s_radio_control.center_hz==162400000 && s_radio_control.tune_state==LS_IQ_RESULT_PENDING);
        ls_iq_control_request_t manual;LS_CHECK(ls_iq_control_take(&s_radio_control,&manual)&&manual.center_hz==162400000);
        LS_CHECK(fm_rx_plan_control_allowed(&manual)&&ls_iq_control_apply_tune(&s_radio_control,s_session,&manual)==LS_RADIO_OK);
        fm_rx_plan_poll();LS_CHECK(endpoint.actual_iq.center_hz==162400000 && FM.freq_hz==162400000);
        LS_CHECK(s_rx_plan->status.state==LS_RX_PLAN_CANCELLED && !s_rx_plan->status.restored);
        LS_CHECK(!s_plan_request_pending && !s_plan_control_inflight && archive_options[CALL_OPT_FM]==1);
        scheduler_cleanup();
    }
}
LS_CASE(fm_manual_publication_releases_on_refusal_and_nested_calls)
{
    scheduler_ready();s_frequency_locked=true;LS_CHECK(!lakeshark_fm_tune(1000)&&!s_plan_manual_publishers);
    LS_CHECK(!lakeshark_fm_set_freq(162400000)&&!s_plan_manual_publishers);s_frequency_locked=false;
    FM.mode=FM_MODE_AIS;LS_CHECK(!lakeshark_fm_tune(1000)&&!s_plan_manual_publishers);
    LS_CHECK(!lakeshark_fm_set_freq(162400000)&&!s_plan_manual_publishers);FM.mode=FM_MODE_LISTEN;
    LS_CHECK(!lakeshark_fm_set_freq(1)&&!s_plan_manual_publishers);
    lakeshark_fm_tune_transient(1);LS_CHECK(!s_plan_manual_publishers);
    fm_rx_plan_publish_begin();LS_CHECK(lakeshark_fm_set_freq(162400000)&&s_plan_manual_publishers==1);
    fm_rx_plan_publish_end();LS_CHECK(!s_plan_manual_publishers);scheduler_cleanup();
}
LS_CASE(fm_scheduler_refuses_existing_operator_sessions_and_preserves_record_options)
{
    scheduler_ready();ls_rx_plan_step_t step={162550000,1000};
    LS_CHECK(fm_rx_plan_load("busy mission",&step,1,1));LS_CHECK(fm_mission_start("operator"));
    LS_CHECK(!fm_rx_plan_start());LS_CHECK(fm_mission_stop());
    LS_CHECK(fm_coverage_start());LS_CHECK(!fm_rx_plan_start());LS_CHECK(fm_coverage_stop());
    LS_CHECK(!fm_rx_plan_record_fm(false)); /* FM active; no policy write. */
    scheduler_cleanup();archive_ready=false;archive_init_failure=false;calls_busy=true;
    LS_CHECK(!fm_rx_plan_record_fm(false)&&archive_options[CALL_OPT_FM]==1);calls_busy=false;
    LS_CHECK(fm_rx_plan_record_fm(false)&&archive_options[CALL_OPT_FM]==0);
    LS_CHECK(archive_options[CALL_OPT_P25]==1&&archive_options[CALL_OPT_MIN_MS]==500&&archive_options[CALL_OPT_DAYS]==30);
    LS_CHECK(fm_rx_plan_record_fm(true)&&archive_options[CALL_OPT_FM]==1);
    archive_ready=false;archive_init_failure=true;LS_CHECK(!fm_rx_plan_record_fm(false));
    archive_init_failure=false;scheduler_cleanup();
    if(s_mission){heap_caps_free(s_mission);s_mission=NULL;}
}
LS_CASE(fm_scheduler_rejects_actual_tuner_bounds_unknown_model_and_changed_lease)
{
    scheduler_ready();ls_rx_plan_step_t step={99000000,1000};LS_CHECK(fm_rx_plan_load("range",&step,1,1));
    LS_CHECK(!fm_rx_plan_start());step.center_hz=162550000;LS_CHECK(fm_rx_plan_load("model",&step,1,1));
    endpoint.tuner_model_known=false;LS_CHECK(!fm_rx_plan_start());endpoint.tuner_model_known=true;
    LS_CHECK(fm_rx_plan_start());endpoint.diagnostic_context_epoch++;
    fm_rx_plan_poll();LS_CHECK(!s_rx_plan->status.active && actual_retunes==0 && !s_rx_plan->status.restored);
    scheduler_cleanup();
}
/* fm_lr_consume also feeds the decoders on the LR path. This test is about
   the listening chain, so those answer and do nothing. */
void call_archive_audio(call_input_t k, uint32_t hz, uint32_t a, uint32_t b, bool c, const int16_t *p, unsigned n)
{ (void)k; (void)hz; (void)a; (void)b; (void)c; (void)p; (void)n; }
void call_archive_gate(call_input_t k, uint32_t hz, bool open) { (void)k; (void)hz; (void)open; }
bool fm_lifecycle_acars_enter(void) { return false; }
void fm_lifecycle_acars_leave(void) {}
int acars_rs_5to3(acars_rs_t *r, const float *in, int n_in, float *out, int cap)
{ (void)r; (void)in; (void)n_in; (void)out; (void)cap; return 0; }
void pocsag_process(pocsag_ctx_t *c, const float *d, int n) { (void)c; (void)d; (void)n; }
bool pocsag_synced(const pocsag_ctx_t *c) { (void)c; return false; }
uint32_t pocsag_n_frames(const pocsag_ctx_t *c) { (void)c; return 0; }
uint32_t pocsag_n_pages(const pocsag_ctx_t *c) { (void)c; return 0; }
uint32_t pocsag_n_cwerr(const pocsag_ctx_t *c) { (void)c; return 0; }
uint32_t pocsag_n_addr(const pocsag_ctx_t *c) { (void)c; return 0; }
uint32_t pocsag_n_msg(const pocsag_ctx_t *c) { (void)c; return 0; }
void flex_process(flex_ctx_t *c, const float *d, int n) { (void)c; (void)d; (void)n; }
bool flex_synced(const flex_ctx_t *c) { (void)c; return false; }
uint32_t flex_n_frames(const flex_ctx_t *c) { (void)c; return 0; }
uint32_t flex_n_pages(const flex_ctx_t *c) { (void)c; return 0; }
uint32_t flex_n_cwerr(const flex_ctx_t *c) { (void)c; return 0; }
int flex_near_min(const flex_ctx_t *c) { (void)c; return 0; }
LS_CASE(lr_callback_gap_backpressure_and_audio_cap)
{
    memset(&FM,0,sizeof(FM)); s_lr_channel=25000; s_lr_hz=162550000;
    fm_lr_init(&s_lr_dsp,s_lr_channel); fm_lr_reset_chain();
    uint8_t iq[1280]; double phase=0; writes=peak=0; drops=0;
    for (int block=0;block<100;block++) {
        for (int i=0;i<320;i++) {
            phase += 1.0; /* extreme deviation exercises peak limit */
            int16_t re=(int16_t)(30000*cos(phase)), im=(int16_t)(30000*sin(phase));
            iq[4*i]=re; iq[4*i+1]=(uint16_t)re>>8;
            iq[4*i+2]=im; iq[4*i+3]=(uint16_t)im>>8;
        }
        fm_lr_consume(NULL,iq,sizeof(iq),true);
    }
    LS_EQ_INT(100,writes); LS_CHECK(peak<=6000); LS_CHECK(peak>0);
    int before=writes;
    fm_lr_consume(NULL,iq,sizeof(iq),false);
    LS_EQ_INT(before,writes); LS_EQ_UINT(1,FM.lr_audio_drops);
    LS_CHECK(!s_lr_dsp.have); LS_CHECK(!FM.squelch_open);
    drop_audio=true; fm_lr_consume(NULL,iq,sizeof(iq),true); drop_audio=false;
    LS_EQ_UINT(2,FM.lr_audio_drops);
    fm_lr_consume(NULL,NULL,0,false);
    LS_CHECK(!s_lr_dsp.have); LS_EQ_INT(0,s_lr_dsp.phase);
    LS_CHECK(!s_dsp.f_deemph && !s_dsp.noise_hp);
    LS_CHECK(!FM.squelch_open);
}

LS_CASE(manual_marker_lock_survives_an_asynchronous_sweep_exit)
{
    FM.mode=FM_MODE_SCAN; FM.scan_start_hz=150000000; FM.scan_stop_hz=162000000;
    FM.freq_hz=154785000; s_scan_tune_hz=154785000;
    lakeshark_fm_frequency_lock(true);
    LS_EQ_UINT(lakeshark_fm_frequency_lock_hz(),154785000);
    s_scan_tune_hz=0; FM.freq_hz=150012500;
    lakeshark_fm_frequency_lock(true);
    LS_EQ_UINT(lakeshark_fm_frequency_lock_hz(),156000000);
    lakeshark_fm_frequency_lock(false); FM.mode=FM_MODE_LISTEN;
}
