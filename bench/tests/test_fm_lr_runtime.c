/* Production FM callback, with a host speaker sink and no hardware. */
#include "ls_test.h"
#include "../../components/lakeshark/apps/fm/app_fm.c"
void ls_iq_control_set_streaming(ls_iq_control_t *c, bool on, ls_radio_err_t e)
{ c->receiver_streaming=on; c->receiver_error=e; }
void ls_iq_control_note_tune_result(ls_iq_control_t *c, uint64_t hz, ls_radio_err_t e, uint64_t actual)
{ (void)hz; c->effective_center_hz=actual; c->tune_error=e; }
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
