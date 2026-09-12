/* LS_TEST_SOURCES: ${FW}/components/lakeshark/radio/radio_endpoint.c ${FW}/components/lakeshark/radio/radio_health.c */
#include "ls_test.h"
#include "radio_endpoint.h"
#include "radio_health.h"
#include "esp_timer.h"
#include "freertos/task.h"

#include <string.h>

typedef struct {
    bool started;
} health_fake_t;

static ls_radio_err_t iq_configure(void *ctx,
                                    const ls_radio_iq_config_t *requested,
                                    ls_radio_iq_config_t *actual)
{
    (void)ctx;
    *actual = *requested;
    return LS_RADIO_OK;
}

static ls_radio_err_t iq_gain(void *ctx, ls_radio_gain_mode_t mode,
                              int gain, int *actual)
{
    (void)ctx;
    (void)mode;
    *actual = gain;
    return LS_RADIO_OK;
}

static ls_radio_err_t stream_start(void *ctx)
{
    ((health_fake_t *)ctx)->started = true;
    return LS_RADIO_OK;
}

static ls_radio_err_t iq_read(void *ctx, void *dst, size_t bytes,
                              uint32_t timeout_ms, size_t *read_bytes)
{
    (void)ctx;
    (void)timeout_ms;
    size_t n = bytes < 4 ? bytes : 4;
    memset(dst, 0x80, n);
    *read_bytes = n;
    return LS_RADIO_OK;
}

static ls_radio_err_t iq_retune(void *ctx, uint64_t requested, bool fast,
                                uint64_t *actual)
{
    (void)ctx;
    (void)fast;
    *actual = requested;
    return LS_RADIO_OK;
}

static ls_radio_err_t stream_stop(void *ctx)
{
    ((health_fake_t *)ctx)->started = false;
    return LS_RADIO_OK;
}

static void cancel_read(void *ctx) { (void)ctx; }

static ls_radio_err_t packet_configure(
    void *ctx, const ls_radio_packet_config_t *requested,
    ls_radio_packet_config_t *actual)
{
    (void)ctx;
    *actual = *requested;
    return LS_RADIO_OK;
}

static ls_radio_err_t packet_read(void *ctx, ls_radio_packet_t *packet,
                                  uint32_t timeout_ms)
{
    (void)ctx;
    (void)timeout_ms;
    packet->bytes = 1;
    packet->data[0] = 0x42;
    return LS_RADIO_OK;
}

static const ls_radio_driver_ops_t s_iq_ops = {
    .iq_configure = iq_configure,
    .iq_set_gain = iq_gain,
    .iq_start = stream_start,
    .iq_read = iq_read,
    .iq_retune = iq_retune,
    .iq_stop = stream_stop,
    .cancel_read = cancel_read,
};

static const ls_radio_driver_ops_t s_packet_ops = {
    .packet_rx_configure = packet_configure,
    .packet_rx_start = stream_start,
    .packet_rx_read = packet_read,
    .packet_rx_stop = stream_stop,
    .cancel_read = cancel_read,
};

static const ls_radio_range_t s_iq_freq[] = {{24000000, 1766000000}};
static const ls_radio_range_t s_iq_rate[] = {{225001, 3200000}};
static const ls_radio_range_t s_packet_freq[] = {{300000000, 928000000}};

static unsigned s_recovery_requests;
static char s_recovery_id[LS_RADIO_ENDPOINT_ID_MAX];

static void request_recovery(const char *endpoint_id)
{
    ++s_recovery_requests;
    strncpy(s_recovery_id, endpoint_id, sizeof(s_recovery_id) - 1);
}

LS_CASE(health_is_per_endpoint_across_attach_stall_recovery_and_detach)
{
    ls_shim_time_set(1000);
    ls_shim_task_reset();
    radio_health_hooks_t hooks = {.request_recovery = request_recovery};
    radio_health_init(&hooks);

    /* health runs on the product's existing 200/250 ms service
       loops.  A dedicated stack here starved the later 4096-byte USB pump. */
    LS_CHECK(ls_shim_task_last_name()[0] == '\0');

    health_fake_t iq = {0}, packet = {0};
    const ls_radio_endpoint_t iq_endpoint = {
        .endpoint_id = "health.iq",
        .name = "Health IQ",
        .capabilities = LS_RADIO_RX_IQ_U8,
        .iq_formats = LS_RADIO_IQ_FORMAT_U8_INTERLEAVED,
        .frequency_ranges = s_iq_freq,
        .frequency_range_count = 1,
        .sample_rate_ranges = s_iq_rate,
        .sample_rate_range_count = 1,
        .ops = &s_iq_ops,
        .driver_ctx = &iq,
    };
    const ls_radio_endpoint_t packet_endpoint = {
        .endpoint_id = "health.packet",
        .name = "Health packet",
        .capabilities = LS_RADIO_RX_PACKET,
        .packet_formats = LS_RADIO_PACKET_FORMAT_FSK,
        .frequency_ranges = s_packet_freq,
        .frequency_range_count = 1,
        .ops = &s_packet_ops,
        .driver_ctx = &packet,
    };
    LS_EQ_INT(ls_radio_endpoint_register(&iq_endpoint), LS_RADIO_OK);
    LS_EQ_INT(ls_radio_endpoint_register(&packet_endpoint), LS_RADIO_OK);

    radio_health_snapshot_t iq_health, packet_health;
    LS_CHECK(radio_health_get("health.iq", &iq_health));
    LS_CHECK(radio_health_get("health.packet", &packet_health));
    ls_radio_endpoint_info_t iq_info;
    LS_EQ_INT(ls_radio_endpoint_get("health.iq", &iq_info), LS_RADIO_OK);
    LS_CHECK(radio_health_get_for_endpoint(&iq_info, &iq_health));
    LS_EQ_STR(iq_health.endpoint_id, "health.iq");
    LS_CHECK(!radio_health_get_for_endpoint(NULL, &iq_health));
    LS_EQ_INT(iq_health.state, RH_SETTLING);
    LS_EQ_INT(packet_health.state, RH_SETTLING);
    LS_EQ_UINT(iq_health.attaches, 1);
    LS_EQ_UINT(packet_health.attaches, 1);

    ls_shim_time_advance(3000000);
    radio_health_tick();
    LS_CHECK(radio_health_get("health.packet", &packet_health));
    LS_EQ_INT(packet_health.state, RH_OK);

    const ls_radio_requirements_t requirements = {
        .required_caps = LS_RADIO_RX_IQ_U8,
        .sample_rate_hz = 240000,
        .iq_format = LS_RADIO_IQ_FORMAT_U8_INTERLEAVED,
        .preferred_endpoint_id = "health.iq",
    };
    ls_radio_session_t *session = NULL;
    LS_EQ_INT(ls_radio_acquire("park-test", &requirements, &session),
              LS_RADIO_OK);
    const ls_radio_iq_config_t requested = {
        .center_hz = 100000000,
        .sample_rate_hz = 240000,
        .gain_mode = LS_RADIO_GAIN_AUTO,
    };
    ls_radio_iq_config_t actual;
    LS_EQ_INT(ls_radio_iq_configure(session, &requested, &actual),
              LS_RADIO_OK);
    LS_EQ_INT(ls_radio_iq_start(session), LS_RADIO_OK);

    ls_shim_time_advance(9000000);
    radio_health_tick();
    radio_health_tick();
    LS_EQ_UINT(s_recovery_requests, 1);
    LS_EQ_STR(s_recovery_id, "health.iq");
    LS_CHECK(radio_health_get("health.iq", &iq_health));
    LS_EQ_INT(iq_health.state, RH_RECOVERING);
    LS_EQ_UINT(iq_health.recoveries, 1);
    LS_CHECK(radio_health_get("health.packet", &packet_health));
    LS_EQ_INT(packet_health.state, RH_OK);
    LS_EQ_UINT(packet_health.recoveries, 0);

    ls_radio_release(session);
    LS_EQ_INT(ls_radio_endpoint_unregister("health.iq"), LS_RADIO_OK);
    LS_CHECK(radio_health_get("health.iq", &iq_health));
    LS_EQ_INT(iq_health.state, RH_ABSENT);
    LS_EQ_UINT(iq_health.detaches, 1);
    ls_radio_endpoint_info_t info;
    LS_EQ_INT(ls_radio_endpoint_get("health.packet", &info), LS_RADIO_OK);
    LS_CHECK(info.present);
    LS_EQ_INT(ls_radio_endpoint_unregister("health.packet"), LS_RADIO_OK);
}
