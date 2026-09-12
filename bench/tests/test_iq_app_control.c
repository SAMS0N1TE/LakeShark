/* LS_TEST_SOURCES: ${FW}/components/lakeshark/radio/iq_app_control.c ${FW}/components/lakeshark/radio/radio_endpoint.c */
#include "ls_test.h"
#include "iq_app_control.h"
#include "ui/ls_receiver_status.h"

#include <string.h>

typedef struct {
    ls_radio_err_t configure_error;
    ls_radio_err_t tune_error;
    ls_radio_err_t gain_error;
    bool started;
} fake_iq_t;

static ls_radio_err_t fake_configure(void *ctx,
                                     const ls_radio_iq_config_t *requested,
                                     ls_radio_iq_config_t *actual)
{
    fake_iq_t *fake = ctx;
    if (fake->configure_error != LS_RADIO_OK)
        return fake->configure_error;
    *actual = *requested;
    actual->center_hz -= actual->center_hz % 1000u;
    actual->gain_tenths_db -= actual->gain_tenths_db % 10;
    return LS_RADIO_OK;
}

static ls_radio_err_t fake_gain(void *ctx, ls_radio_gain_mode_t mode,
                                int requested, int *actual)
{
    fake_iq_t *fake = ctx;
    if (fake->gain_error != LS_RADIO_OK) return fake->gain_error;
    *actual = mode == LS_RADIO_GAIN_AUTO ? 0 : requested - requested % 10;
    return LS_RADIO_OK;
}

static ls_radio_err_t fake_start(void *ctx)
{
    ((fake_iq_t *)ctx)->started = true;
    return LS_RADIO_OK;
}

static ls_radio_err_t fake_read(void *ctx, void *dst, size_t bytes,
                                uint32_t timeout_ms, size_t *read_bytes)
{
    (void)ctx;
    (void)timeout_ms;
    memset(dst, 0x80, bytes);
    *read_bytes = bytes;
    return LS_RADIO_OK;
}

static ls_radio_err_t fake_retune(void *ctx, uint64_t requested, bool fast,
                                  uint64_t *actual)
{
    fake_iq_t *fake = ctx;
    (void)fast;
    if (fake->tune_error != LS_RADIO_OK) return fake->tune_error;
    *actual = requested - requested % 1000u;
    return LS_RADIO_OK;
}

static ls_radio_err_t fake_stop(void *ctx)
{
    ((fake_iq_t *)ctx)->started = false;
    return LS_RADIO_OK;
}

static void fake_cancel(void *ctx) { (void)ctx; }

static const ls_radio_driver_ops_t s_fake_ops = {
    .iq_configure = fake_configure,
    .iq_set_gain = fake_gain,
    .iq_start = fake_start,
    .iq_read = fake_read,
    .iq_retune = fake_retune,
    .iq_stop = fake_stop,
    .cancel_read = fake_cancel,
};

static const ls_radio_range_t s_freqs[] = {{24000000, 1766000000}};
static const ls_radio_range_t s_rates[] = {{256000, 256000}};

static void register_fake(const char *id, fake_iq_t *fake)
{
    const ls_radio_endpoint_t endpoint = {
        .endpoint_id = id,
        .name = "IQ control fake",
        .capabilities = LS_RADIO_RX_IQ_U8,
        .duplex = LS_RADIO_DUPLEX_HALF,
        .iq_formats = LS_RADIO_IQ_FORMAT_U8_INTERLEAVED,
        .frequency_ranges = s_freqs,
        .frequency_range_count = 1,
        .sample_rate_ranges = s_rates,
        .sample_rate_range_count = 1,
        .ops = &s_fake_ops,
        .driver_ctx = fake,
    };
    LS_EQ_INT(ls_radio_endpoint_register(&endpoint), LS_RADIO_OK);
}

static ls_radio_session_t *acquire_fake(const char *id)
{
    ls_radio_requirements_t requirements = {
        .required_caps = LS_RADIO_RX_IQ_U8,
        .min_hz = 24000000,
        .max_hz = 1766000000,
        .sample_rate_hz = 256000,
        .iq_format = LS_RADIO_IQ_FORMAT_U8_INTERLEAVED,
        .preferred_endpoint_id = id,
    };
    ls_radio_session_t *session = NULL;
    LS_EQ_INT(ls_radio_acquire("control-test", &requirements, &session),
              LS_RADIO_OK);
    LS_CHECK(session != NULL);
    return session;
}

static void configure_start(ls_iq_control_t *control,
                            ls_radio_session_t *session, uint32_t center_hz,
                            int gain)
{
    const ls_radio_iq_config_t requested = {
        .center_hz = center_hz,
        .sample_rate_hz = 256000,
        .gain_mode = gain == 0 ? LS_RADIO_GAIN_AUTO : LS_RADIO_GAIN_MANUAL,
        .gain_tenths_db = gain,
    };
    ls_radio_iq_config_t actual;
    LS_EQ_INT(ls_iq_control_configure(control, session, &requested, &actual),
              LS_RADIO_OK);
    LS_EQ_INT(ls_radio_iq_start(session), LS_RADIO_OK);
    ls_iq_control_set_streaming(control, true, LS_RADIO_OK);
}

LS_CASE(routes_tune_gain_and_bandwidth_to_owner)
{
    ls_iq_control_t control = {0};
    ls_iq_control_request_t request;

    LS_CHECK(!ls_iq_control_take(&control, &request));
    ls_iq_control_request_gain(&control, 370);
    ls_iq_control_request_bandwidth(&control, 200000);
    LS_CHECK(ls_iq_control_take(&control, &request));
    LS_EQ_UINT(request.flags, LS_IQ_CONTROL_GAIN | LS_IQ_CONTROL_BANDWIDTH);
    LS_EQ_INT(request.gain_tenths_db, 370);
    LS_EQ_UINT(request.bandwidth_hz, 200000);
    LS_CHECK(!ls_iq_control_take(&control, &request));
}

LS_CASE(preserves_fast_and_ordinary_retune_flags)
{
    ls_iq_control_t control = {0};
    ls_iq_control_request_t request;

    ls_iq_control_request_tune(&control, 154785000, false);
    LS_CHECK(ls_iq_control_take(&control, &request));
    LS_EQ_UINT(request.flags, LS_IQ_CONTROL_TUNE);
    LS_EQ_UINT(request.center_hz, 154785000);
    LS_CHECK(!request.fast_retune);

    ls_iq_control_request_tune(&control, 155250000, true);
    LS_CHECK(ls_iq_control_take(&control, &request));
    LS_EQ_UINT(request.center_hz, 155250000);
    LS_CHECK(request.fast_retune);
}

LS_CASE(coalesces_each_control_to_the_latest_value)
{
    ls_iq_control_t control = {0};
    ls_iq_control_request_t request;

    ls_iq_control_request_tune(&control, 150000000, false);
    ls_iq_control_request_tune(&control, 151000000, true);
    ls_iq_control_request_gain(&control, 200);
    ls_iq_control_request_gain(&control, 280);
    LS_CHECK(ls_iq_control_take(&control, &request));
    LS_EQ_UINT(request.center_hz, 151000000);
    LS_CHECK(request.fast_retune);
    LS_EQ_INT(request.gain_tenths_db, 280);

    ls_iq_control_request_gain(&control, 0);
    ls_iq_control_reset(&control);
    LS_CHECK(!ls_iq_control_take(&control, &request));
}

LS_CASE(fake_endpoint_keeps_requested_and_rounded_effective_distinct)
{
    static const char *id = "test.iq.control.round";
    fake_iq_t fake = {0};
    ls_iq_control_t control = {0};
    ls_iq_control_request_t request;
    ls_iq_control_status_t status;

    register_fake(id, &fake);
    ls_radio_session_t *session = acquire_fake(id);
    ls_iq_control_reset(&control);
    ls_iq_control_set_initial(&control, 433920123, 407, 0);
    configure_start(&control, session, 433920123, 407);

    ls_iq_control_status(&control, &status);
    LS_EQ_UINT(status.requested_center_hz, 433920123);
    LS_EQ_UINT(status.effective_center_hz, 433920000);
    LS_EQ_INT(status.requested_gain_tenths_db, 407);
    LS_EQ_INT(status.effective_gain_tenths_db, 400);
    LS_EQ_INT(status.tune_state, LS_IQ_RESULT_EFFECTIVE);
    LS_EQ_INT(status.gain_state, LS_IQ_RESULT_EFFECTIVE);
    LS_CHECK(status.receiver_streaming);

    ls_iq_control_request_tune(&control, 434070987, true);
    ls_iq_control_request_gain(&control, 459);
    ls_iq_control_status(&control, &status);
    LS_EQ_INT(status.tune_state, LS_IQ_RESULT_PENDING);
    LS_EQ_UINT(status.effective_center_hz, 433920000);
    LS_CHECK(ls_iq_control_take(&control, &request));
    LS_EQ_INT(ls_iq_control_apply_tune(&control, session, &request),
              LS_RADIO_OK);
    LS_EQ_INT(ls_iq_control_apply_gain(&control, session, &request),
              LS_RADIO_OK);
    ls_iq_control_status(&control, &status);
    LS_EQ_UINT(status.requested_center_hz, 434070987);
    LS_EQ_UINT(status.effective_center_hz, 434070000);
    LS_EQ_INT(status.requested_gain_tenths_db, 459);
    LS_EQ_INT(status.effective_gain_tenths_db, 450);

    ls_receiver_presentation_t presentation;
    ls_receiver_present(&status, &presentation);
    LS_CHECK(strstr(presentation.frequency, "REQ 434.0709") != NULL);
    LS_CHECK(strstr(presentation.frequency, "ACT 434.0700") != NULL);
    LS_CHECK(strstr(presentation.gain, "REQ 45.9 dB") != NULL);
    LS_CHECK(strstr(presentation.gain, "ACT 45.0 dB") != NULL);

    ls_radio_release(session);
    LS_EQ_INT(ls_radio_endpoint_unregister(id), LS_RADIO_OK);
}

LS_CASE(fail_detach_and_reacquire_preserve_last_success_until_new_success)
{
    static const char *id = "test.iq.control.reconnect";
    fake_iq_t fake = {0};
    ls_iq_control_t control = {0};
    ls_iq_control_request_t request;
    ls_iq_control_status_t status;

    register_fake(id, &fake);
    ls_radio_session_t *session = acquire_fake(id);
    ls_iq_control_reset(&control);
    ls_iq_control_set_initial(&control, 433920000, 400, 0);
    configure_start(&control, session, 433920000, 400);

    fake.tune_error = LS_RADIO_ERR_IO;
    fake.gain_error = LS_RADIO_ERR_UNSUPPORTED;
    ls_iq_control_request_tune(&control, 434070123, false);
    ls_iq_control_request_gain(&control, 411);
    LS_CHECK(ls_iq_control_take(&control, &request));
    LS_EQ_INT(ls_iq_control_apply_tune(&control, session, &request),
              LS_RADIO_ERR_IO);
    LS_EQ_INT(ls_iq_control_apply_gain(&control, session, &request),
              LS_RADIO_ERR_UNSUPPORTED);
    ls_iq_control_status(&control, &status);
    LS_EQ_INT(status.tune_state, LS_IQ_RESULT_FAILED);
    LS_EQ_INT(status.tune_error, LS_RADIO_ERR_IO);
    LS_EQ_UINT(status.effective_center_hz, 433920000);
    LS_EQ_INT(status.gain_state, LS_IQ_RESULT_FAILED);
    LS_EQ_INT(status.gain_error, LS_RADIO_ERR_UNSUPPORTED);
    LS_EQ_INT(status.effective_gain_tenths_db, 400);

    ls_receiver_presentation_t presentation;
    ls_receiver_present(&status, &presentation);
    LS_CHECK(strstr(presentation.frequency, "io") != NULL);
    LS_CHECK(strstr(presentation.gain, "unsupported") != NULL);

    LS_EQ_INT(ls_radio_endpoint_unregister(id), LS_RADIO_OK);
    ls_iq_control_receiver_lost(&control, LS_RADIO_ERR_DISCONNECTED);
    ls_iq_control_status(&control, &status);
    LS_CHECK(!status.receiver_streaming);
    LS_EQ_INT(status.receiver_error, LS_RADIO_ERR_DISCONNECTED);
    LS_EQ_UINT(status.requested_center_hz, 434070123);
    LS_EQ_UINT(status.effective_center_hz, 433920000);
    ls_radio_release(session);

    memset(&fake, 0, sizeof(fake));
    fake.configure_error = LS_RADIO_ERR_IO;
    register_fake(id, &fake);
    session = acquire_fake(id);
    const ls_radio_iq_config_t retry = {
        .center_hz = 434070123,
        .sample_rate_hz = 256000,
        .gain_mode = LS_RADIO_GAIN_MANUAL,
        .gain_tenths_db = 411,
    };
    ls_radio_iq_config_t retry_actual;
    LS_EQ_INT(ls_iq_control_configure(&control, session, &retry,
                                      &retry_actual), LS_RADIO_ERR_IO);
    ls_iq_control_receiver_lost(&control, LS_RADIO_ERR_IO);
    ls_iq_control_status(&control, &status);
    LS_CHECK(!status.receiver_streaming);
    LS_EQ_UINT(status.effective_center_hz, 433920000);
    LS_EQ_INT(status.effective_gain_tenths_db, 400);
    ls_radio_release(session);
    LS_EQ_INT(ls_radio_endpoint_unregister(id), LS_RADIO_OK);

    memset(&fake, 0, sizeof(fake));
    register_fake(id, &fake);
    session = acquire_fake(id);
    configure_start(&control, session, 434070123, 411);
    ls_iq_control_status(&control, &status);
    LS_CHECK(status.receiver_streaming);
    LS_EQ_UINT(status.effective_center_hz, 434070000);
    LS_EQ_INT(status.effective_gain_tenths_db, 410);
    LS_EQ_INT(status.tune_state, LS_IQ_RESULT_EFFECTIVE);
    LS_EQ_INT(status.gain_state, LS_IQ_RESULT_EFFECTIVE);

    ls_radio_release(session);
    LS_EQ_INT(ls_radio_endpoint_unregister(id), LS_RADIO_OK);
}
