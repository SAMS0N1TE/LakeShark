/* LS_TEST_SOURCES: ${FW}/components/lakeshark/dsp/ls_sweep.c ${FW}/components/lakeshark/dsp/ls_sweep_core.c */
/* LS_TEST_INCLUDE: ${FW}/components/lakeshark/dsp ${FW}/components/lakeshark/radio */
#include "ls_test.h"
#include "ls_sweep.h"
#include "spectrum.h"
#include <string.h>
static char requested_endpoint[48];
/* live: a receiver that works. Every read is mid-scale I/Q, except that each
 * 1024-byte transform of the tune at rail_hz has rail_n bytes at 255, and the
 * first tune's have one byte at 0. reads counts every read, settling ones too. */
static bool live;
static uint64_t at_hz,rail_hz,first_hz;
static unsigned rail_n,reads;
ls_radio_err_t ls_radio_acquire(const char *owner,const ls_radio_requirements_t *r,ls_radio_session_t **out)
{
    (void)owner;*out=NULL;
    snprintf(requested_endpoint,sizeof(requested_endpoint),"%s",r->preferred_endpoint_id?r->preferred_endpoint_id:"");
    if(live){*out=(ls_radio_session_t *)1;return LS_RADIO_OK;}
    return LS_RADIO_ERR_BUSY;
}
ls_radio_err_t ls_radio_iq_configure(ls_radio_session_t *s,const ls_radio_iq_config_t *c,ls_radio_iq_config_t *a)
{(void)s;if(!live)return LS_RADIO_ERR_INVALID;*a=*c;at_hz=first_hz=c->center_hz;return LS_RADIO_OK;}
ls_radio_err_t ls_radio_iq_start(ls_radio_session_t *s){(void)s;return live?LS_RADIO_OK:LS_RADIO_ERR_INVALID;}
ls_radio_err_t ls_radio_iq_stop(ls_radio_session_t *s){(void)s;return live?LS_RADIO_OK:LS_RADIO_ERR_INVALID;}
ls_radio_err_t ls_radio_iq_retune(ls_radio_session_t *s,uint64_t hz,bool fast,uint64_t *a)
{(void)s;(void)fast;if(!live)return LS_RADIO_ERR_INVALID;at_hz=hz;*a=hz;return LS_RADIO_OK;}
ls_radio_err_t ls_radio_iq_read(ls_radio_session_t *s,void *b,size_t n,uint32_t timeout,size_t *got)
{
    (void)s;(void)timeout;
    if(!live)return LS_RADIO_ERR_INVALID;
    uint8_t *iq=b;memset(iq,128,n);reads++;
    for(size_t c=0;c+1024<=n;c+=1024) {
        if(at_hz==rail_hz)memset(iq+c,255,rail_n);
        if(at_hz==first_hz)iq[c+5]=0;
    }
    *got=n;return LS_RADIO_OK;
}
void ls_radio_release(ls_radio_session_t *s){(void)s;}
const char *ls_radio_err_name(ls_radio_err_t e){(void)e;return "fake";}
void spectrum_init(void){}
void spectrum_reset(void){}
void spectrum_accum(const uint8_t *iq,int n){(void)iq;(void)n;}
bool spectrum_read_db(float *out,int n){(void)out;(void)n;return false;}
LS_CASE(explicit_sweep_receiver_reaches_broker_without_fallback)
{
    ls_sweep_plan_t plan;int8_t bins[2];
    LS_CHECK(ls_sweep_plan(869000000,869050000,25000,256000,&plan));
    LS_EQ_INT(ls_sweep_run_endpoint(&plan,297,8,false,bins,NULL,NULL,NULL,LS_RADIO_ENDPOINT_RTL_USB),LS_RADIO_ERR_BUSY);
    LS_CHECK(!strcmp(requested_endpoint,LS_RADIO_ENDPOINT_RTL_USB));
    LS_EQ_INT(ls_sweep_run_cancelable(&plan,297,8,false,bins,NULL,NULL,NULL),LS_RADIO_ERR_BUSY);
    LS_CHECK(!requested_endpoint[0]);
    /* A receiver that cannot be had leaves no stale rail count behind. */
    ls_sweep_rails_t r;memset(&r,0x5a,sizeof(r));
    LS_EQ_INT(ls_sweep_run_rails(&plan,297,8,false,bins,NULL,NULL,NULL,LS_RADIO_ENDPOINT_RTL_USB,&r),LS_RADIO_ERR_BUSY);
    LS_EQ_UINT(r.samples,0);LS_EQ_UINT(r.worst_rails,0);LS_EQ_UINT(r.worst_hz,0);
}
LS_CASE(the_sweep_counts_rail_bytes_per_tune_and_names_the_worst)
{
    /* 869-871 MHz at 256 kS/s: a handful of tunes, 8 transforms each. */
    ls_sweep_plan_t plan;static int8_t bins[128];
    LS_CHECK(ls_sweep_plan(869000000,871000000,25000,256000,&plan));
    LS_CHECK(plan.n_tunes>=4 && plan.n_bins<=128);
    live=true;rail_n=3;rail_hz=ls_sweep_tune_center(&plan,2);reads=0;
    ls_sweep_rails_t r;
    LS_EQ_INT(ls_sweep_run_rails(&plan,297,8,false,bins,NULL,NULL,NULL,LS_RADIO_ENDPOINT_RTL_USB,&r),LS_RADIO_OK);
    /* Only the 8 x 1024 bytes each tune folded in: the two settling reads
     * of every tune carried rails as well, and are not counted. */
    LS_EQ_UINT(reads,plan.n_tunes*3);
    LS_EQ_UINT(r.samples,plan.n_tunes*8192u);
    LS_EQ_UINT(r.rails,8u*3u+8u*1u);
    LS_EQ_UINT(r.worst_rails,24);LS_EQ_UINT(r.worst_samples,8192);
    LS_EQ_UINT(r.worst_hz,rail_hz);
    /* Without the clipped tune, the first tune's byte a transform is the worst. */
    rail_n=0;
    LS_EQ_INT(ls_sweep_run_rails(&plan,297,8,false,bins,NULL,NULL,NULL,LS_RADIO_ENDPOINT_RTL_USB,&r),LS_RADIO_OK);
    LS_EQ_UINT(r.rails,8u);LS_EQ_UINT(r.worst_rails,8u);LS_EQ_UINT(r.worst_hz,ls_sweep_tune_center(&plan,0));
    /* The plain entry point runs the same sweep and asks for none of it. */
    LS_EQ_INT(ls_sweep_run_endpoint(&plan,297,8,false,bins,NULL,NULL,NULL,LS_RADIO_ENDPOINT_RTL_USB),LS_RADIO_OK);
    live=false;
}
LS_CASE(a_rail_is_either_end_of_the_converter_in_i_or_q)
{
    static const uint8_t iq[]={0,128,255,1,254,127,0,0,255,128};
    LS_EQ_UINT(ls_sweep_rail_count(iq,sizeof(iq)),5);
    LS_EQ_UINT(ls_sweep_rail_count(iq,1),1);
    LS_EQ_UINT(ls_sweep_rail_count(iq,0),0);
    LS_EQ_UINT(ls_sweep_rail_count(NULL,10),0);
}
