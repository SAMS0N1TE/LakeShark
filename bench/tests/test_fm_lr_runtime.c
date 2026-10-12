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
