#include "ls_test.h"
#include "p25_lr_frontend.h"
#include <math.h>
#include <string.h>

static void put(uint8_t *p, int v) { p[0]=(uint8_t)v; p[1]=(uint8_t)((unsigned)v>>8); }
LS_CASE(frontend_chunk_boundaries_are_bit_exact_and_gaps_reset_history)
{
    uint8_t in[4096]; float whole[4000], split[4000], reset[4000];
    for (unsigned i=0;i<2048;i++) put(in+2*i,(int)((i*373)%65536)-32768);
    p25_lr_frontend_t a,b;
    LS_CHECK(p25_lr_frontend_init(&a,1300,true)); b=a;
    size_t n=p25_lr_frontend_process(&a,in,sizeof(in),whole,2000), used=0;
    for (size_t pos=0;pos<sizeof(in);) {
        size_t bytes=4*(1+(pos*17)%61);
        if(bytes>sizeof(in)-pos) bytes=sizeof(in)-pos;
        size_t expected=p25_lr_frontend_needed(&b,bytes);
        size_t got=p25_lr_frontend_process(&b,in+pos,bytes,split+2*used,2000-used);
        LS_EQ_UINT(got,expected); used+=got; pos+=bytes;
    }
    LS_EQ_UINT(n,used); LS_CHECK(memcmp(whole,split,n*2*sizeof(float))==0);
    p25_lr_frontend_reset(&b);
    LS_EQ_UINT(p25_lr_frontend_process(&b,in,sizeof(in),reset,2000),n);
    LS_CHECK(memcmp(whole,reset,n*2*sizeof(float))==0);
}
LS_CASE(frontend_failure_is_atomic_and_single_pairs_keep_exact_rate)
{
    p25_lr_frontend_t s, saved;
    uint8_t in[4]={1,0,255,255}; float out[4]={123,123,123,123};
    LS_CHECK(p25_lr_frontend_init(&s,0,false)); saved=s;
    LS_EQ_UINT(p25_lr_frontend_process(&s,in,4,out,0),SIZE_MAX);
    LS_EQ_UINT(p25_lr_frontend_process(&s,in,3,out,2),SIZE_MAX);
    LS_CHECK(memcmp(&s,&saved,sizeof(s))==0); LS_CHECK(out[0]==123);
    size_t total=0;
    for(unsigned i=0;i<125001;i++) total+=p25_lr_frontend_process(&s,in,4,out,2);
    LS_EQ_UINT(total,240001);
    LS_CHECK(out[0]==1/32768.0f); LS_CHECK(out[1]==-1/32768.0f);
    LS_CHECK(!p25_lr_frontend_init(&s,NAN,true));
}
static double tone_rms(double hz, double center)
{
    uint8_t in[16000]; float out[16000]; p25_lr_frontend_t s;
    LS_CHECK(p25_lr_frontend_init(&s,(float)center,true));
    for(unsigned k=0;k<4000;k++) {
        double angle=6.283185307179586*hz*k/125000;
        put(in+4*k,(int)lround(16000*cos(angle)));
        put(in+4*k+2,(int)lround(16000*sin(angle)));
    }
    size_t n=p25_lr_frontend_process(&s,in,sizeof(in),out,8000);
    double power=0;
    for(size_t k=256;k<n;k++) power+=out[2*k]*out[2*k]+out[2*k+1]*out[2*k+1];
    return sqrt(power/(n-256));
}
LS_CASE(frontend_rejects_adjacent_tones_and_translates_both_frequency_signs)
{
    double pass=tone_rms(1800,0), stop=tone_rms(12500,0);
    LS_CHECK(pass>0.45); LS_CHECK(stop/pass<0.01);
    LS_CHECK(tone_rms(21800,20000)>0.45);
    LS_CHECK(tone_rms(-21800,-20000)>0.45);
    ls_note("channel filter 12.5 kHz rejection %.1f dB",20*log10(stop/pass));
}
