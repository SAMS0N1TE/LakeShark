/* LS_TEST_SOURCES: ${FW}/components/lakeshark/dsp/ls_sweep.c ${FW}/components/lakeshark/dsp/ls_sweep_core.c */
/* LS_TEST_INCLUDE: ${FW}/components/lakeshark/dsp ${FW}/components/lakeshark/radio */
#include "ls_test.h"
#include "ls_sweep.h"
#include "spectrum.h"
#include <string.h>
static char requested_endpoint[48];
ls_radio_err_t ls_radio_acquire(const char *owner,const ls_radio_requirements_t *r,ls_radio_session_t **out)
{
    (void)owner;*out=NULL;
    snprintf(requested_endpoint,sizeof(requested_endpoint),"%s",r->preferred_endpoint_id?r->preferred_endpoint_id:"");
    return LS_RADIO_ERR_BUSY;
}
ls_radio_err_t ls_radio_iq_configure(ls_radio_session_t *s,const ls_radio_iq_config_t *c,ls_radio_iq_config_t *a)
{(void)s;(void)c;(void)a;return LS_RADIO_ERR_INVALID;}
ls_radio_err_t ls_radio_iq_start(ls_radio_session_t *s){(void)s;return LS_RADIO_ERR_INVALID;}
ls_radio_err_t ls_radio_iq_stop(ls_radio_session_t *s){(void)s;return LS_RADIO_ERR_INVALID;}
ls_radio_err_t ls_radio_iq_retune(ls_radio_session_t *s,uint64_t hz,bool fast,uint64_t *a)
{(void)s;(void)hz;(void)fast;(void)a;return LS_RADIO_ERR_INVALID;}
ls_radio_err_t ls_radio_iq_read(ls_radio_session_t *s,void *b,size_t n,uint32_t timeout,size_t *got)
{(void)s;(void)b;(void)n;(void)timeout;(void)got;return LS_RADIO_ERR_INVALID;}
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
}
