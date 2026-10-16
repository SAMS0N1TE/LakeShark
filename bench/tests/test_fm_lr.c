#include "ls_test.h"
#include "fm_lr_dsp.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#define PI 3.14159265358979323846
static fm_lr_dsp_t lr;
static fm_dsp_t fm;
static uint32_t rng = 123;
static int16_t noise(void) { rng = rng*1664525u+1013904223u; return (int16_t)(rng>>16); }
static void pack(uint8_t *p, int16_t i, int16_t q) {
    p[0]=(uint16_t)i; p[1]=(uint16_t)i>>8; p[2]=(uint16_t)q; p[3]=(uint16_t)q>>8;
}
static double phase;
static int samples;
static int run_block(int kind, int16_t *pcm, int *open)
{
    uint8_t iq[1280]; float demod[84];
    for(int n=0;n<320;n++,samples++) {
        phase += 2*PI*3000/125000*sin(2*PI*1000*samples/125000.0);
        pack(iq+4*n,kind ? noise() : (int16_t)(18000*cos(phase)),
                       kind ? noise() : (int16_t)(18000*sin(phase)));
    }
    int nd=fm_lr_demod(&lr,&fm,iq,sizeof(iq),demod,84);
    *open=fm_nfm_squelch(&fm,30,nd);
    return fm_demod_to_audio(&fm,demod,nd,pcm,44);
}
LS_CASE(lr_nfm_tone_125k_to_16k_both_channels)
{
    for(unsigned channel=12500;channel<=25000;channel*=2) {
        fm_lr_init(&lr,channel); fm_dsp_init(&fm); phase=0; samples=0;
        double ss=0,cc=0,power=0; int used=0,total=0,open=0;
        for(int b=0;b<800;b++) {
            int16_t pcm[44]; int n=run_block(0,pcm,&open);
            for(int k=0;k<n;k++,total++) if(total>=8000 && used<16000) {
                double a=2*PI*1000*used/16000;
                ss+=pcm[k]*sin(a); cc+=pcm[k]*cos(a); power+=(double)pcm[k]*pcm[k]; used++;
            }
        }
        double signal=2*(ss*ss+cc*cc)/used;
        double snr=10*log10(signal/(power-signal));
        printf("channel %u: %d samples, tone SNR %.2f dB\n",channel,total,snr);
        LS_EQ_INT(16000,used); LS_CHECK(abs(total-32768)<=1);
        LS_CHECK(snr>22); LS_CHECK(open);
    }
}
LS_CASE(lr_noise_squelch_closes_and_carrier_reopens)
{
    for(unsigned channel=12500;channel<=25000;channel*=2) {
        fm_lr_init(&lr,channel); fm_dsp_init(&fm); int open=0; int16_t pcm[44];
        for(int b=0;b<400;b++) run_block(0,pcm,&open);
        LS_CHECK(open);
        for(int b=0;b<500;b++) run_block(1,pcm,&open);
        printf("channel %u noise %.3f open %d\n",channel,fm.demod_noise,open);
        LS_CHECK(!open);
        for(int b=0;b<400;b++) run_block(0,pcm,&open);
        LS_CHECK(open);
    }
}
LS_CASE(lr_gap_reset_matches_fresh_receiver)
{
    fm_lr_init(&lr,25000); fm_dsp_init(&fm); int open=0; int16_t pcm[44];
    for(int b=0;b<100;b++) run_block(0,pcm,&open);
    fm_lr_reset(&lr); fm_dsp_init(&fm);
    fm_lr_dsp_t clean; fm_dsp_t clean_fm;
    fm_lr_init(&clean,25000); fm_dsp_init(&clean_fm);
    LS_CHECK(!memcmp(&lr,&clean,sizeof(lr)));
    LS_CHECK(!memcmp(&fm,&clean_fm,sizeof(fm)));
    LS_CHECK(!fm.squelch_is_open); LS_CHECK(fm.squelch_settle_samples>0);
}
LS_CASE(lr_range_and_modes)
{
    /* The LR2021's measured receive range, 104-2740 MHz. */
    LS_CHECK(!fm_lr_supported(103999999,FM_MODE_LISTEN));
    LS_CHECK(!fm_lr_supported(91000000,FM_MODE_LISTEN));
    LS_CHECK(!fm_lr_supported(91000000,FM_MODE_WFM));
    LS_CHECK(fm_lr_supported(152600000,FM_MODE_POCSAG));   /* pagers ride the I/Q too */
    LS_CHECK(fm_lr_supported(131550000,FM_MODE_ACARS));
    LS_CHECK(fm_lr_supported(127500000,FM_MODE_AM));
    LS_CHECK(!fm_lr_supported(162550000,FM_MODE_AIS));      /* AIS needs SDR-rate I/Q */
    LS_CHECK(!fm_lr_supported(2740000001u,FM_MODE_LISTEN));
    LS_CHECK(fm_lr_supported(104000000,FM_MODE_LISTEN));
    LS_CHECK(fm_lr_supported(144000000,FM_MODE_LISTEN));
    LS_CHECK(fm_lr_supported(1200000000,FM_MODE_LISTEN));
    LS_CHECK(fm_lr_supported(2740000000u,FM_MODE_LISTEN));
}
