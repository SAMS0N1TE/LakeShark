/* LS_TEST_SOURCES: (none: includes lte_mib.c for its pilot sequences) */
/* LS_TEST_INCLUDE: ${APP}/cell */
#include "ls_test.h"
#include "../../components/lakeshark/apps/cell/lte_mib.c"
#include <stdlib.h>

/* Slot 1 symbols 0 and 4 of an FDD frame carrying only the port 0 cell
 * reference signals of the central six resource blocks, at an amplitude
 * (raw transform units) chosen so the expected level is known. */
#define SAMPLES 57600
static void build(uint8_t *iq,int pci,int frame_start,int cfo,float amp,float sigma,unsigned seed)
{
    workspace_t *w=calloc(1,sizeof(*w));
    LS_CHECK(w);
    initialize(w,pci);
    cx *acc=calloc(SAMPLES,sizeof(cx));LS_CHECK(acc);
    for(int frame=0;frame*19200+frame_start+1644<=SAMPLES;frame++) {
        int base=frame_start+19200*frame;
        for(int t=0;t<2;t++) {
            int l=t?4:0,shift=(pci+(t?3:0))%6;
            cx spec[128];memset(spec,0,sizeof(spec));
            for(int m=0;m<12;m++) {
                int sc=6*m+shift,bin=sc<36?92+sc:sc-35;
                spec[bin]=(cx){w->pilots[t?2:0][m].r*amp,w->pilots[t?2:0][m].i*amp};
            }
            /* inverse of the unnormalised forward transform the receiver applies */
            cx time[128];
            for(int n=0;n<128;n++) {
                cx sum={0,0};
                for(int k=0;k<128;k++) sum=add(sum,mul(spec[k],cis(TAU*(float)k*n/128.f)));
                time[n]=(cx){sum.r/128.f,sum.i/128.f};
            }
            int useful=base+970+137*l,cp=l?9:10;
            for(int n=-cp;n<128;n++) {
                int at=useful+n;if(at<0 || at>=SAMPLES) continue;
                cx v=time[(n+128)%128],rot=cis(TAU*cfo*(float)at/1920000.f);
                v=mul(v,rot);acc[at]=add(acc[at],v);
            }
        }
    }
    ls_rng_t r;ls_rng_seed(&r,seed);
    for(int n=0;n<SAMPLES;n++) {
        float i=acc[n].r+sigma*ls_rng_noise(&r)/.289f+127.5f,q=acc[n].i+sigma*ls_rng_noise(&r)/.289f+127.5f;
        iq[2*n]=(uint8_t)(i<0?0:i>255?255:i+.5f);iq[2*n+1]=(uint8_t)(q<0?0:q>255?255:q+.5f);
    }
    free(acc);free(w);
}
static float level_for(float amp){return 10.f*log10f(amp*amp/(128.f*127.5f*128.f*127.5f));}

LS_CASE(crs_power_reads_the_level_the_pilots_were_built_at)
{
    uint8_t *iq=malloc(SAMPLES*2);void *ws=malloc(lte_mib_workspace_size());LS_CHECK(iq && ws);
    const float amp=520.f;
    build(iq,301,7756,14000,amp,1.5f,1);
    float db;
    LS_CHECK(lte_mib_crs_power(iq,SAMPLES,301,7756,14000,ws,&db));
    LS_NEAR(db,level_for(amp),.8);
    /* Twice the amplitude is 6 dB more. */
    float db2;
    build(iq,301,7756,14000,2*amp,1.5f,1);
    LS_CHECK(lte_mib_crs_power(iq,SAMPLES,301,7756,14000,ws,&db2));
    LS_NEAR(db2-db,6.02,.4);
    /* Another PCI, another frame boundary and a small offset error still read it. */
    build(iq,17,1234,-9500,amp,1.5f,2);
    LS_CHECK(lte_mib_crs_power(iq,SAMPLES,17,1234,-9500,ws,&db));
    LS_NEAR(db,level_for(amp),.8);
    LS_CHECK(lte_mib_crs_power(iq,SAMPLES,17,1234,-9000,ws,&db));
    LS_NEAR(db,level_for(amp),1.5);
    free(ws);free(iq);
}
LS_CASE(the_wrong_pci_or_no_signal_reads_far_below_the_pilots)
{
    uint8_t *iq=malloc(SAMPLES*2);void *ws=malloc(lte_mib_workspace_size());LS_CHECK(iq && ws);
    build(iq,301,7756,0,520.f,1.5f,3);
    float right,wrong;
    LS_CHECK(lte_mib_crs_power(iq,SAMPLES,301,7756,0,ws,&right));
    LS_CHECK(lte_mib_crs_power(iq,SAMPLES,302,7756,0,ws,&wrong));
    LS_CHECK(right>level_for(520.f)-1.f);
    /* A pilot's own power is concentrated on its resource elements; the
     * wrong cell's pilot positions hold only the other pilots' neighbours. */
    LS_CHECK(wrong<right-6.f);
    build(iq,301,7756,0,0.f,1.5f,4);
    LS_CHECK(lte_mib_crs_power(iq,SAMPLES,301,7756,0,ws,&wrong));
    LS_CHECK(wrong<level_for(520.f)-15.f);
    free(ws);free(iq);
}
LS_CASE(crs_power_refuses_what_it_cannot_measure)
{
    uint8_t *iq=calloc(SAMPLES*2,1);void *ws=malloc(lte_mib_workspace_size());LS_CHECK(iq && ws);
    float db=0;
    LS_CHECK(!lte_mib_crs_power(NULL,SAMPLES,1,0,0,ws,&db));
    LS_CHECK(!lte_mib_crs_power(iq,SAMPLES,1,0,0,NULL,&db));
    LS_CHECK(!lte_mib_crs_power(iq,SAMPLES,1,0,0,ws,NULL));
    LS_CHECK(!lte_mib_crs_power(iq,100,1,0,0,ws,&db));
    LS_CHECK(!lte_mib_crs_power(iq,SAMPLES,504,0,0,ws,&db));
    LS_CHECK(!lte_mib_crs_power(iq,SAMPLES,1,19200,0,ws,&db));
    LS_CHECK(!lte_mib_crs_power(iq,SAMPLES,1,0,60000,ws,&db));
    /* All-zero bytes are a constant of -127.5: no pilot power at all. */
    LS_CHECK(!lte_mib_crs_power(iq,SAMPLES,1,0,0,ws,&db) || db<-60.f);
    free(ws);free(iq);
}
