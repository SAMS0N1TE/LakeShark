/* LS_TEST_SOURCES: ${FW}/components/lakeshark/radio/radio_endpoint.c ${FW}/components/lakeshark/radio/radio_health.c */
#include "ls_test.h"
#include "radio_endpoint.h"
#include "radio_health.h"
#include "esp_timer.h"

#include <string.h>

/* Tuner clock correction through the endpoint: only an idle session may set
 * it, only an endpoint that offers it takes it, and the value reaches the
 * driver untouched. */
typedef struct { int ppm, calls; ls_radio_err_t result; } fake_t;

static ls_radio_err_t f_configure(void *ctx, const ls_radio_iq_config_t *r, ls_radio_iq_config_t *a)
{(void)ctx;*a=*r;return LS_RADIO_OK;}
static ls_radio_err_t f_gain(void *ctx, ls_radio_gain_mode_t m, int g, int *a)
{(void)ctx;(void)m;*a=g;return LS_RADIO_OK;}
static ls_radio_err_t f_start(void *ctx){(void)ctx;return LS_RADIO_OK;}
static ls_radio_err_t f_stop(void *ctx){(void)ctx;return LS_RADIO_OK;}
static void f_cancel(void *ctx){(void)ctx;}
static ls_radio_err_t f_read(void *ctx, void *dst, size_t bytes, uint32_t t, size_t *got)
{(void)ctx;(void)t;memset(dst,0x80,bytes);*got=bytes;return LS_RADIO_OK;}
static ls_radio_err_t f_retune(void *ctx, uint64_t hz, bool fast, uint64_t *actual)
{(void)ctx;(void)fast;*actual=hz;return LS_RADIO_OK;}
static ls_radio_err_t f_ppm(void *ctx, int ppm)
{fake_t *f=ctx;f->ppm=ppm;f->calls++;return f->result;}

static const ls_radio_driver_ops_t WITH = {
    .iq_configure=f_configure,.iq_set_gain=f_gain,.iq_start=f_start,.iq_read=f_read,
    .iq_retune=f_retune,.iq_stop=f_stop,.cancel_read=f_cancel,.iq_set_ppm=f_ppm,
};
static const ls_radio_driver_ops_t WITHOUT = {
    .iq_configure=f_configure,.iq_set_gain=f_gain,.iq_start=f_start,.iq_read=f_read,
    .iq_retune=f_retune,.iq_stop=f_stop,.cancel_read=f_cancel,
};
static const ls_radio_range_t FREQ[] = {{24000000, 1766000000}};
static const ls_radio_range_t RATE[] = {{225001, 3200000}};

static ls_radio_session_t *open_session(const char *id, const ls_radio_driver_ops_t *ops, fake_t *fake)
{
    const ls_radio_endpoint_t endpoint = {
        .endpoint_id=id,.name="ppm test",.capabilities=LS_RADIO_RX_IQ_U8,
        .iq_formats=LS_RADIO_IQ_FORMAT_U8_INTERLEAVED,.frequency_ranges=FREQ,.frequency_range_count=1,
        .sample_rate_ranges=RATE,.sample_rate_range_count=1,.ops=ops,.driver_ctx=fake,
    };
    LS_EQ_INT(ls_radio_endpoint_register(&endpoint), LS_RADIO_OK);
    const ls_radio_requirements_t req = {
        .required_caps=LS_RADIO_RX_IQ_U8,.sample_rate_hz=1920000,
        .iq_format=LS_RADIO_IQ_FORMAT_U8_INTERLEAVED,.preferred_endpoint_id=id,
    };
    ls_radio_session_t *session=NULL;
    LS_EQ_INT(ls_radio_acquire("ppm", &req, &session), LS_RADIO_OK);
    return session;
}

LS_CASE(an_idle_session_sets_the_correction_and_the_driver_gets_the_value)
{
    fake_t fake={0,0,LS_RADIO_OK};
    ls_radio_session_t *s=open_session("ppm.with",&WITH,&fake);
    LS_EQ_INT(ls_radio_iq_set_ppm(s,12),LS_RADIO_OK);
    LS_EQ_INT(fake.ppm,12);LS_EQ_INT(fake.calls,1);
    LS_EQ_INT(ls_radio_iq_set_ppm(s,-7),LS_RADIO_OK);
    LS_EQ_INT(fake.ppm,-7);
    LS_EQ_INT(ls_radio_iq_set_ppm(s,0),LS_RADIO_OK);
    LS_EQ_INT(fake.ppm,0);
    /* A failing tuner is reported, not hidden. */
    fake.result=LS_RADIO_ERR_IO;
    LS_EQ_INT(ls_radio_iq_set_ppm(s,5),LS_RADIO_ERR_IO);
    ls_radio_release(s);
}
LS_CASE(refuses_a_streaming_session_an_absurd_value_and_an_endpoint_without_it)
{
    fake_t fake={0,0,LS_RADIO_OK};
    ls_radio_session_t *s=open_session("ppm.busy",&WITH,&fake);
    ls_radio_iq_config_t want={.center_hz=739000000,.sample_rate_hz=1920000},got;
    LS_EQ_INT(ls_radio_iq_configure(s,&want,&got),LS_RADIO_OK);
    LS_EQ_INT(ls_radio_iq_start(s),LS_RADIO_OK);
    LS_EQ_INT(ls_radio_iq_set_ppm(s,3),LS_RADIO_ERR_BUSY);
    LS_EQ_INT(fake.calls,0);
    LS_EQ_INT(ls_radio_iq_stop(s),LS_RADIO_OK);
    LS_EQ_INT(ls_radio_iq_set_ppm(s,201),LS_RADIO_ERR_INVALID);
    LS_EQ_INT(ls_radio_iq_set_ppm(s,-201),LS_RADIO_ERR_INVALID);
    LS_EQ_INT(ls_radio_iq_set_ppm(s,3),LS_RADIO_OK);
    ls_radio_release(s);
    fake_t none={0,0,LS_RADIO_OK};
    ls_radio_session_t *u=open_session("ppm.none",&WITHOUT,&none);
    LS_EQ_INT(ls_radio_iq_set_ppm(u,3),LS_RADIO_ERR_UNSUPPORTED);
    LS_EQ_INT(none.calls,0);
    LS_EQ_INT(ls_radio_iq_set_ppm(NULL,3),LS_RADIO_ERR_INVALID);
    ls_radio_release(u);
}
