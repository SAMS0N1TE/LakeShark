/* LS_TEST_SOURCES: ${FW}/components/lakeshark/radio/radio_endpoint.c ${FW}/components/lakeshark/radio/radio_health.c */
#include "ls_test.h"
#include "radio_endpoint.h"
#include "radio_health.h"

#include <string.h>

static ls_radio_err_t configure(void *ctx,
                                const ls_radio_iq_config_t *requested,
                                ls_radio_iq_config_t *actual)
{
    (void)ctx;
    *actual = *requested;
    return LS_RADIO_OK;
}

static ls_radio_err_t gain(void *ctx, ls_radio_gain_mode_t mode, int requested,
                           int *actual)
{
    (void)ctx; (void)mode;
    *actual = requested;
    return LS_RADIO_OK;
}

static ls_radio_err_t start(void *ctx) { (void)ctx; return LS_RADIO_OK; }
static ls_radio_err_t stop(void *ctx) { (void)ctx; return LS_RADIO_OK; }
static void cancel(void *ctx) { (void)ctx; }

static ls_radio_err_t read_iq(void *ctx, void *dst, size_t bytes,
                              uint32_t timeout_ms, size_t *read_bytes)
{
    (void)ctx; (void)dst; (void)timeout_ms;
    *read_bytes = bytes;
    return LS_RADIO_OK;
}

static ls_radio_err_t retune(void *ctx, uint64_t requested, bool fast,
                             uint64_t *actual)
{
    (void)ctx; (void)fast;
    *actual = requested;
    return LS_RADIO_OK;
}

LS_CASE(uninitialised_health_is_unknown_not_absent_while_streaming)
{
    static const ls_radio_driver_ops_t ops = {
        .iq_configure = configure,
        .iq_set_gain = gain,
        .iq_start = start,
        .iq_read = read_iq,
        .iq_retune = retune,
        .iq_stop = stop,
        .cancel_read = cancel,
    };
    static const ls_radio_range_t frequencies[] = {{24000000, 1766000000}};
    static const ls_radio_range_t rates[] = {{225001, 300000}};
    const ls_radio_endpoint_t endpoint = {
        .endpoint_id = LS_RADIO_ENDPOINT_RTL_USB,
        .name = "RTL test",
        .capabilities = LS_RADIO_RX_IQ_U8,
        .iq_formats = LS_RADIO_IQ_FORMAT_U8_INTERLEAVED,
        .frequency_ranges = frequencies,
        .frequency_range_count = 1,
        .sample_rate_ranges = rates,
        .sample_rate_range_count = 1,
        .ops = &ops,
    };
    LS_EQ_INT(ls_radio_endpoint_register(&endpoint), LS_RADIO_OK);
    ls_radio_session_t *session = NULL;
    const ls_radio_requirements_t requirements = {
        .required_caps = LS_RADIO_RX_IQ_U8,
        .sample_rate_hz = 240000,
        .iq_format = LS_RADIO_IQ_FORMAT_U8_INTERLEAVED,
    };
    LS_EQ_INT(ls_radio_acquire("p25", &requirements, &session), LS_RADIO_OK);
    const ls_radio_iq_config_t requested = {
        .center_hz = 851012500,
        .sample_rate_hz = 240000,
        .gain_mode = LS_RADIO_GAIN_MANUAL,
        .gain_tenths_db = 230,
    };
    ls_radio_iq_config_t actual;
    LS_EQ_INT(ls_radio_iq_configure(session, &requested, &actual), LS_RADIO_OK);
    LS_EQ_INT(ls_radio_iq_start(session), LS_RADIO_OK);

    ls_radio_endpoint_info_t info;
    radio_health_snapshot_t snapshot;
    LS_EQ_INT(ls_radio_endpoint_get(LS_RADIO_ENDPOINT_RTL_USB, &info),
              LS_RADIO_OK);
    LS_CHECK(!radio_health_get_for_endpoint(&info, &snapshot));
    LS_CHECK(!radio_health_get_for_endpoint(NULL, &snapshot));

    char report[128];
    radio_health_report(LS_RADIO_ENDPOINT_RTL_USB, report, sizeof(report));
    LS_CHECK(strstr(report, "health=unknown") != NULL);
    LS_CHECK(strstr(report, "present=1 stream=1") != NULL);
    LS_CHECK(strstr(report, "absent") == NULL);

    ls_radio_release(session);
    LS_EQ_INT(ls_radio_endpoint_unregister(LS_RADIO_ENDPOINT_RTL_USB),
              LS_RADIO_OK);
}
