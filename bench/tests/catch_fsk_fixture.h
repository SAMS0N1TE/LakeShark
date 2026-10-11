/* Original synthetic RTL fixture: continuous-phase 2-FSK, no decoder code. */
#ifndef CATCH_FSK_FIXTURE_H
#define CATCH_FSK_FIXTURE_H
#include <math.h>
#include <stdint.h>
typedef struct {
    uint64_t sample, delay_samples;
    uint32_t random;
    double re, im, amplitude;
    bool comb;
} catch_fixture_t;
static int fixture_noise(catch_fixture_t *f)
{
    int sum=0;
    for(int j=0;j<6;j++) {f->random=f->random*1664525u+1013904223u;sum+=(int)(f->random>>28);}
    return (sum-45)/3;
}
static uint8_t fixture_u8(double v)
{if(v<0)return 0;if(v>255)return 255;return (uint8_t)(v+0.5);}
static void fixture_block(catch_fixture_t *f,uint8_t *iq,size_t n)
{
    const double c=0.0130895955713444,s=0.999914327574007;
    for(size_t i=0;i<n/2;i++,f->sample++) {
        /* Eight 47 ms packets, three seconds apart, deliberately unaligned. */
        uint64_t pos;
        uint64_t period=(f->sample>=f->delay_samples?f->sample-f->delay_samples:0)/(3ull*CATCH_RATE);
        pos=(f->sample>=f->delay_samples?f->sample-f->delay_samples:0)%(3ull*CATCH_RATE);
        uint64_t onset=CATCH_RATE+1237;
        bool on=f->sample>=f->delay_samples && f->amplitude>0 && period<8 && pos>=onset && pos<onset+CATCH_RATE*47/1000;
        int bit=0;
        if(on) {
            unsigned symbol=(unsigned)((pos-onset)*4800/CATCH_RATE);
            if(symbol<64)bit=(int)(symbol&1);
            else if(symbol<96)bit=(0xD391DA26u>>(95-symbol))&1;
            else bit=(0xA6593C87u>>((symbol-96)%32))&1;
        }
        /* exp(j*(600k +/-5k)/2.4M): rotation remains phase continuous. */
        double cr=bit?-c:c;
        double r=f->re*cr-f->im*s,q=f->re*s+f->im*cr;f->re=r;f->im=q;
        int ni=fixture_noise(f),nq=fixture_noise(f);
        int image=(f->sample&1)?2:-2;
        /* Symmetric +/-400 and +/-800 kHz teeth, mixed before ADC clipping. */
        static const double tooth[]={1,0.5,-0.5,-1,-0.5,0.5};
        double comb=f->comb?10*(tooth[f->sample%6]+tooth[(2*f->sample)%6]):0;
        iq[2*i]=fixture_u8(128+20+image+ni+comb+(on?f->amplitude*r:0));
        iq[2*i+1]=fixture_u8(128+13+nq+(on?f->amplitude*q:0));
    }
}
/* Reported board spurs: one wandering tooth, two fixed teeth and a
   close pair; mix before ADC clipping at the full transport cadence. */
static inline void fixture_wandering_spurs(uint8_t *iq,size_t n,unsigned block,
                                           double *re,double *im)
{
    const int wander[]={-835000,-827000,-804000,-802000,-789000};
    int offsets[]={wander[(block/3)%5],275000,669000,block%7<3?807000:815000};
    double cr[4],ci[4];
    for(unsigned j=0;j<4;j++) {
        double a=6.283185307179586*offsets[j]/CATCH_RATE;
        cr[j]=cos(a);ci[j]=sin(a);
    }
    for(size_t i=0;i<n/2;i++) {
        double r=iq[2*i],q=iq[2*i+1];
        for(unsigned j=0;j<4;j++) {
            double next=re[j]*cr[j]-im[j]*ci[j];
            im[j]=re[j]*ci[j]+im[j]*cr[j];re[j]=next;
            double amp=j==0?22:6;
            /* Level modulation and brief dropouts defeat edge-only
               and fixed +/-5 dB reference matching. */
            if((block+j)%19==0)amp=0;
            else if((block+j)%11<4)amp*=0.5;
            r+=amp*re[j];q+=amp*im[j];
        }
        iq[2*i]=fixture_u8(r);iq[2*i+1]=fixture_u8(q);
    }
}
#endif
