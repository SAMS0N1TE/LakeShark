/* SPDX-License-Identifier: GPL-3.0-or-later
 * Original windowed-sinc rational resampler. The staged narrowband search
 * follows the acquisition architecture documented by LTE-Cell-Scanner and
 * srsRAN; no upstream implementation is copied into this firmware.
 */
#include "lte_resample.h"
#include <math.h>
#define OUTPUT_RATE 1920000u
#define HALF 64
#define TAPS (2*HALF+1)
#define PHASES 96
#define TAU 6.2831853071795864769f
typedef struct {float h[PHASES][TAPS];} filter_t;
size_t lte_resample_workspace_size(void){return sizeof(filter_t);}
size_t lte_resample_count(size_t n,uint32_t rate)
{
    if(rate<2000000 || rate>20000000 || n<=2*HALF || n>2000000)return 0;
    return (size_t)((uint64_t)(n-2*HALF)*OUTPUT_RATE/rate);
}
size_t lte_resample_u8(const uint8_t *input,size_t n,uint32_t rate,
                       uint8_t *output,size_t capacity,void *memory,
                       lte_sync_yield_fn yield,void *arg)
{
    size_t count=lte_resample_count(n,rate);
    if(!input || !output || !memory || !count || capacity<count)return 0;
    filter_t *f=memory;
    const float cutoff=650000.f/rate;
    /* Generate each needed fractional phase once. At 8 MS/s only six of
     * the 96 phases are visited; arbitrary accepted rates remain supported. */
    bool prepared[PHASES]={false};
    size_t period=OUTPUT_RATE,a=rate,b=OUTPUT_RATE;
    while(b){size_t remainder=a%b;a=b;b=remainder;}
    period/=a;if(period>count)period=count;
    for(size_t i=0;i<period;i++) {
        uint64_t position=(uint64_t)i*rate;
        unsigned p=(unsigned)(((position%OUTPUT_RATE)*PHASES+OUTPUT_RATE/2)/OUTPUT_RATE);
        if(p==PHASES)p=0;
        if(prepared[p])continue;
        float sum=0;
        for(int k=0;k<TAPS;k++) {
            float x=(float)(k-HALF)-(float)p/PHASES;
            float sinc=fabsf(x)<1e-6f?2*cutoff:sinf(TAU*cutoff*x)/(TAU*.5f*x);
            float window=.42f-.5f*cosf(TAU*k/(TAPS-1))+.08f*cosf(2*TAU*k/(TAPS-1));
            sum+=(f->h[p][k]=sinc*window);
        }
        for(int k=0;k<TAPS;k++)f->h[p][k]/=sum;
        prepared[p]=true;
        if(yield && yield(arg))return 0;
    }
    /* Remove capture DC and use a conservative common I/Q gain. Never
     * normalize each symbol independently or change the saved raw evidence. */
    uint64_t si=0,sq=0;int min_i=255,min_q=255,max_i=0,max_q=0;
    for(size_t i=0;i<n;i++) {
        int a=input[2*i],b=input[2*i+1];si+=a;sq+=b;
        if(a<min_i)min_i=a;
        if(a>max_i)max_i=a;
        if(b<min_q)min_q=b;
        if(b>max_q)max_q=b;
        if((i&4095)==0 && yield && yield(arg))return 0;
    }
    float mi=(float)((double)si/n),mq=(float)((double)sq/n);
    float peak=fmaxf(fmaxf(max_i-mi,mi-min_i),fmaxf(max_q-mq,mq-min_q));
    float gain=80.f/fmaxf(peak,1.f);
    for(size_t i=0;i<count;i++) {
        uint64_t position=(uint64_t)i*rate;
        size_t start=(size_t)(position/OUTPUT_RATE);
        unsigned p=(unsigned)(((position%OUTPUT_RATE)*PHASES+OUTPUT_RATE/2)/OUTPUT_RATE);
        if(p==PHASES){p=0;start++;}
        if(start+TAPS>n)return 0;
        float a=0,b=0;
        for(unsigned k=0;k<TAPS;k++) {
            float h=f->h[p][k];
            a+=((float)input[2*(start+k)]-mi)*h;
            b+=((float)input[2*(start+k)+1]-mq)*h;
        }
        int ai=(int)lroundf(a*gain+127.5f),bi=(int)lroundf(b*gain+127.5f);
        output[2*i]=(uint8_t)(ai<0?0:ai>255?255:ai);
        output[2*i+1]=(uint8_t)(bi<0?0:bi>255?255:bi);
        if((i&255)==0 && yield && yield(arg))return 0;
    }
    return count;
}
