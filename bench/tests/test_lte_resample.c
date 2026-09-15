/* LS_TEST_SOURCES: ${APP}/cell/lte_resample.c */
/* LS_TEST_INCLUDE: ${APP}/cell */
#include "ls_test.h"
#include "lte_resample.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
static double tone(uint32_t rate,double hz)
{
    const size_t n=20000;size_t count=lte_resample_count(n,rate);
    uint8_t *in=malloc(2*n),*out=malloc(2*count);void *ws=malloc(lte_resample_workspace_size());
    for(size_t i=0;i<n;i++) {
        double angle=6.283185307179586*hz*i/rate;
        in[2*i]=(uint8_t)lround(127.5+75*cos(angle));
        in[2*i+1]=(uint8_t)lround(127.5+75*sin(angle));
    }
    LS_EQ_INT(lte_resample_u8(in,n,rate,out,count,ws,NULL,NULL),count);
    double energy=0;
    for(size_t i=0;i<2*count;i++){double x=out[i]-127.5;energy+=x*x;}
    free(in);free(out);free(ws);return sqrt(energy/count);
}
LS_CASE(center_lte_band_survives_and_aliasing_is_rejected)
{
    const uint32_t rates[]={2000000,8000000,8000123,10000000,19200000,20000000};
    for(unsigned i=0;i<sizeof(rates)/sizeof(rates[0]);i++) {
        double pass=tone(rates[i],450000);
        LS_CHECK(pass>55 && pass<85);
        if(rates[i]>=8000000)LS_CHECK(tone(rates[i],1450000)<2);
    }
}
static bool cancelled(void *arg){(*(int *)arg)++;return true;}
LS_CASE(dc_bad_bounds_and_cancel_do_not_produce_results)
{
    uint8_t in[2000],out[400];memset(in,180,sizeof(in));
    void *ws=malloc(lte_resample_workspace_size());int calls=0;
    size_t count=lte_resample_count(1000,10000000);
    LS_CHECK(count>0 && count<=sizeof(out)/2);
    LS_EQ_INT(lte_resample_u8(in,1000,10000000,out,sizeof(out)/2,ws,NULL,NULL),count);
    for(size_t i=0;i<2*count;i++)LS_CHECK(out[i]==127 || out[i]==128);
    LS_EQ_INT(lte_resample_u8(in,1000,10000000,out,count-1,ws,NULL,NULL),0);
    LS_EQ_INT(lte_resample_u8(in,1000,10000000,out,count,ws,cancelled,&calls),0);
    LS_EQ_INT(calls,1);
    LS_EQ_INT(lte_resample_count(1000,0),0);
    LS_EQ_INT(lte_resample_count(128,10000000),0);
    LS_EQ_INT(lte_resample_count(2000001,10000000),0);
    free(ws);
}
