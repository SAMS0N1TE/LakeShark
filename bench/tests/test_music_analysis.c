#include "ls_audio_analysis.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const double TAU=6.283185307179586;
static float reference_db(double p) { return p>1e-12?10*log10f((float)p):-120; }
static void exact_window_statistics(ls_audio_analysis *a) {
    int16_t pcm[4096*2];
    for(unsigned n=256;n<=4096;n*=2)for(unsigned channels=1;channels<=2;channels++)
    for(unsigned kind=0;kind<4;kind++) {
        unsigned random=42;double ll=0,rr=0,lr=0;float peak=0;
        for(unsigned i=0;i<n;i++) {
            int16_t left=0,right=0;
            if(kind==1)left=right=-32768;
            if(kind==2){left=i%2?32767:-32768;right=i%2?-32768:32767;}
            if(kind==3){random=random*1664525u+1013904223u;left=(int16_t)(random>>16);random=random*1664525u+1013904223u;right=(int16_t)(random>>16);}
            if(channels==1)right=left;
            pcm[i*channels]=left;if(channels==2)pcm[2*i+1]=right;
            /* The previous production normalized-double accumulation. */
            float l=left*(1.0f/32768),r=right*(1.0f/32768);
            ll+=(double)l*l;rr+=(double)r*r;lr+=(double)l*r;
            float sample=fmaxf(fabsf(l),fabsf(r));if(sample>peak)peak=sample;
        }
        ls_audio_analysis_reset(a);ls_aa_config c=ls_audio_analysis_defaults();c.fft_size=n;c.hop=n/2;
        assert(ls_audio_analysis_configure(a,&c));
        for(unsigned at=0;at<n;) {
            unsigned take=at%2?113:17;if(take>n-at)take=n-at;
            ls_audio_analysis_append(a,pcm+at*channels,take,channels,44100);
            ls_audio_analysis_process(a,1);at+=take;
        }
        const ls_aa_result *v=ls_audio_analysis_result(a);assert(v->frames==1);
        float rms=reference_db((ll+rr)/(2*n)),sample_peak=reference_db((double)peak*peak),
              correlation=ll*rr>1e-15?(float)(lr/sqrt(ll*rr)):0;
        if(correlation>1)correlation=1;
        if(correlation<-1)correlation=-1;
        assert(!memcmp(&v->rms_db,&rms,sizeof(float)));
        assert(!memcmp(&v->peak_db,&sample_peak,sizeof(float)));
        assert(!memcmp(&v->correlation,&correlation,sizeof(float)));
    }
    ls_aa_config defaults=ls_audio_analysis_defaults();assert(ls_audio_analysis_configure(a,&defaults));
}
/* Independent double-precision Catmull-Rom reference, evaluated per sample.
   This retains the old peak-hold behavior while the real metrics batch work. */
static float peak_reference(const int16_t *pcm,unsigned frames,unsigned channels,
                            float history[2][4],float held) {
    for(unsigned i=0;i<frames;i++)for(unsigned ch=0;ch<channels;ch++) {
        float *p=history[ch];p[0]=p[1];p[1]=p[2];p[2]=p[3];
        p[3]=pcm[i*channels+ch]*(1.0f/32768);
        double peak=fabs(p[3]);
        for(unsigned q=1;q<4;q++) {
            double t=q*.25,t2=t*t,t3=t2*t;
            double y=p[0]*(-.5*t+t2-.5*t3)+p[1]*(1-2.5*t2+1.5*t3)+
                     p[2]*(.5*t+2*t2-1.5*t3)+p[3]*(-.5*t2+.5*t3);
            if(fabs(y)>peak)peak=fabs(y);
        }
        float db=peak*peak>1e-12?10*log10f((float)(peak*peak)):-120;
        if(db>held)held=db;
    }
    return held;
}
static void peak_hold_equivalence(ls_audio_analysis *a) {
    for(unsigned channels=1;channels<=2;channels++) {
        ls_audio_analysis_reset(a);
        float history[2][4]={{0}},held=-120;
        const int16_t overshoot[]={0,32767,32767,0};
        /* Split the plateau across calls: interpolation must span batches. */
        for(unsigned i=0;i<4;i++) {
            int16_t pair[]={channels==1?overshoot[i]:0,overshoot[i]};
            const int16_t *pcm=channels==1?&pair[0]:pair;
            ls_audio_analysis_append(a,pcm,1,channels,8000);
            ls_audio_analysis_process(a,0);
            held=peak_reference(pcm,1,channels,history,held);
            assert(fabsf(ls_audio_analysis_result(a)->estimated_peak_db-held)<.0001f);
        }
        float expected=20*log10f(1.125f*(32767.f/32768));
        assert(fabsf(held-expected)<.0001f && held>1);
        int16_t quiet[128*2]={0};
        ls_audio_analysis_append(a,quiet,128,channels,8000);ls_audio_analysis_process(a,0);
        held=peak_reference(quiet,128,channels,history,held);
        assert(fabsf(ls_audio_analysis_result(a)->estimated_peak_db-held)<.0001f);
        float prior=ls_audio_analysis_result(a)->estimated_peak_db;
        ls_audio_analysis_process(a,0);assert(ls_audio_analysis_result(a)->estimated_peak_db==prior);
        ls_audio_analysis_reset(a);ls_audio_analysis_process(a,0);
        assert(ls_audio_analysis_result(a)->estimated_peak_db==-120);
        ls_audio_analysis_append(a,quiet,128,channels,8000);ls_audio_analysis_process(a,0);
        assert(ls_audio_analysis_result(a)->estimated_peak_db==-120);
        /* Multiple distinct peaks, signed extremes and asymmetric channels. */
        ls_audio_analysis_reset(a);held=-120;
        for(unsigned ch=0;ch<2;ch++)for(unsigned j=0;j<4;j++)history[ch][j]=0;
        unsigned random=42;
        for(unsigned block=0;block<7;block++) {
            int16_t pcm[91*2];
            for(unsigned i=0;i<91*channels;i++) {
                random=random*1664525u+1013904223u;pcm[i]=(int16_t)(random>>16);
            }
            if(block==2)pcm[17]=-32768;
            ls_audio_analysis_append(a,pcm,91,channels,8000);ls_audio_analysis_process(a,0);
            held=peak_reference(pcm,91,channels,history,held);
            assert(fabsf(ls_audio_analysis_result(a)->estimated_peak_db-held)<.0001f);
        }
    }
}
static void feed(ls_audio_analysis *a,float hz,unsigned rate,unsigned frames,int stereo,int kind) {
    int16_t *pcm=malloc(256*2*sizeof(*pcm));assert(pcm);
    for(unsigned at=0;at<frames;at+=256) {
        unsigned n=frames-at<256?frames-at:256;
        for(unsigned i=0;i<n;i++) {
            unsigned t=at+i;double v=.5*sin(TAU*hz*t/rate);
            if(kind==1)v=(t%(rate/2)<rate/40)?.75*sin(TAU*1000*t/rate):0;
            if(kind==2)v=.2*(sin(TAU*261.625565*t/rate)+sin(TAU*329.627557*t/rate)+sin(TAU*391.995436*t/rate));
            if(kind==3)v=0;
            if(kind==4){static unsigned random=42;random=random*1664525u+1013904223u;v=((int)(random>>16)-32768)*(.4/32768);}
            if(kind==5){double seconds=(double)t/rate,total=(double)frames/rate;v=.5*sin(TAU*(220*seconds+110*seconds*seconds/total));}
            if(kind==6)v=.2*sin(TAU*697*t/rate)+.2*sin(TAU*1209*t/rate);
            pcm[2*i]=(int16_t)lrint(v*32767);pcm[2*i+1]=stereo<0?-pcm[2*i]:pcm[2*i];
        }
        ls_audio_analysis_append(a,pcm,n,2,rate);ls_audio_analysis_process(a,2);
    }
    free(pcm);
}
int main(void) {
    ls_audio_analysis *a=ls_audio_analysis_new();assert(a);
    exact_window_statistics(a);
    peak_hold_equivalence(a);
    ls_audio_analysis_reset_at(a,30000);
    feed(a,440,44100,44100,1,0);
    assert(ls_audio_analysis_origin_ms(a)==30000);
    assert(ls_audio_analysis_result(a)->time_ms>30900);
    ls_audio_analysis_reset(a);
    const float tuner_hz[]={55,82.4069f,220,440,880,1000,1567.982f};
    const unsigned tuner_rate[]={16000,44100,48000};
    for(unsigned ri=0;ri<3;ri++)for(unsigned hi=0;hi<7;hi++){
        ls_audio_analysis_reset(a);feed(a,tuner_hz[hi],tuner_rate[ri],tuner_rate[ri],1,0);
        float measured=ls_audio_analysis_result(a)->pitch_hz;
        assert(measured>0 && fabsf(1200*log2f(measured/tuner_hz[hi]))<5);
    }
    ls_audio_analysis_reset(a);
    ls_aa_config c=ls_audio_analysis_defaults();c.fft_size=1024;c.hop=256;
    assert(ls_audio_analysis_configure(a,&c));
    feed(a,1000,8000,32000,1,0);const ls_aa_result *r=ls_audio_analysis_result(a);
    assert(fabsf(r->peak_hz-1000)<2 && fabsf(r->spectral_db+6.021f)<.15f);
    assert(fabsf(r->rms_db+9.031f)<.15f && fabsf(r->crest_db-3.01f)<.2f);
    assert(r->correlation>.999f && r->note==83 && fabsf(r->cents-21.31f)<2);
    ls_aa_cursor cursor;assert(ls_audio_analysis_cursor(a,1234,1000,&cursor));
    assert(cursor.time_ms==1234 && cursor.note==83 && fabsf(cursor.db+6.02f)<.2f);
    assert(isfinite(r->momentary_lufs) && isfinite(r->short_lufs));
    ls_audio_analysis_reset(a);feed(a,1000,48000,192000,1,0);r=ls_audio_analysis_result(a);
    /* BS.1770 stereo, half-scale 1 kHz sine: FFmpeg ebur128 reference -6 LUFS. */
    assert(fabsf(r->momentary_lufs+6)<.35f && fabsf(r->short_lufs+6)<.35f);
    ls_audio_analysis_reset(a);feed(a,440,16000,32000,-1,0);r=ls_audio_analysis_result(a);assert(r->correlation<-.999f);
    ls_audio_analysis_reset(a);c.fft_size=4096;c.hop=1024;c.window=LS_AA_BLACKMAN_HARRIS;
    assert(ls_audio_analysis_configure(a,&c));feed(a,440,16000,32000,1,0);r=ls_audio_analysis_result(a);
    assert(fabsf(r->pitch_hz-440)<2 && r->pitch_confidence>.85f && fabsf(r->peak_hz-440)<2);
    assert(ls_audio_analysis_axis(a,0,100,true)>=39.9f && ls_audio_analysis_axis(a,99,100,true)<=1600.1f);
    ls_aa_track track[8];assert(ls_audio_analysis_track(a,track,8)>0);
    ls_audio_analysis_reset(a);c.fft_size=1024;c.hop=256;c.window=LS_AA_HANN;
    assert(ls_audio_analysis_configure(a,&c));feed(a,0,8000,80000,1,1);r=ls_audio_analysis_result(a);
    assert(r->onsets>=15 && fabsf(r->bpm-120)<4);
    ls_audio_analysis_reset(a);c.fft_size=4096;c.hop=1024;assert(ls_audio_analysis_configure(a,&c));
    feed(a,0,16000,64000,1,2);r=ls_audio_analysis_result(a);assert(r->key_root==0 && !r->key_minor);
    ls_audio_analysis_reset(a);feed(a,0,16000,64000,1,3);r=ls_audio_analysis_result(a);
    assert(r->pitch_hz==0 && r->peak_hz==0 && r->key_root<0 && r->onsets==0 && r->rms_db<=-119);
    ls_audio_analysis_reset(a);feed(a,0,16000,64000,1,4);r=ls_audio_analysis_result(a);
    assert(r->pitch_hz==0 && r->key_root<0 && r->bpm==0);
    ls_audio_analysis_reset(a);feed(a,0,16000,32000,1,5);r=ls_audio_analysis_result(a);
    assert(r->peak_hz>410 && r->peak_hz<445);
    ls_audio_analysis_reset(a);feed(a,0,16000,32000,1,6);
    assert(ls_audio_analysis_bin_db(a,178,false)>-22 && ls_audio_analysis_bin_db(a,310,false)>-22);
    assert(ls_audio_analysis_bin_db(a,256,false)<-40);
    for(unsigned n=256;n<=4096;n*=2)for(unsigned window=0;window<2;window++)for(unsigned overlap=2;overlap<=8;overlap*=2) {
        ls_audio_analysis_reset(a);c.fft_size=n;c.hop=n/overlap;c.window=(ls_aa_window)window;
        assert(ls_audio_analysis_configure(a,&c));feed(a,1024,32768,8192,1,0);
        r=ls_audio_analysis_result(a);assert(fabsf(r->peak_hz-1024)<2 && fabsf(r->spectral_db+6.021f)<.15f);
    }
    c.fft_size=300;assert(!ls_audio_analysis_configure(a,&c));
    c.fft_size=256;c.hop=16;assert(!ls_audio_analysis_configure(a,&c));
    printf("Analysis working storage: %zu bytes\n",ls_audio_analysis_bytes());
    ls_audio_analysis_free(a);puts("PASS STFT/windows/overlap, calibrated spectrum, cursor, stereo, YIN, LUFS, BPM, key, chirp, DTMF, noise and silence");return 0;
}
