/* LS_TEST_SOURCES: ${FW}/components/lakeshark/radio/radio_endpoint.c */
#include "ls_test.h"
#include "radio_endpoint.h"

#include <pthread.h>
#include <string.h>

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    bool blocking;
    bool read_entered;
    bool cancelled;
    bool started;
    ls_radio_err_t configure_error;
    ls_radio_err_t tune_error;
    ls_radio_err_t gain_error;
    ls_radio_err_t start_error;
    ls_radio_err_t read_error;
    ls_radio_err_t stop_error;
    bool last_retune_fast;
    unsigned retune_count;
    unsigned recover_count;
} fake_radio_t;

static const ls_radio_range_t s_iq_freq[] = {{24000000, 1766000000}};
static const ls_radio_range_t s_iq_rates[] = {
    {225001, 300000},
    {900001, 3200000},
};
static const ls_radio_range_t s_packet_freq[] = {{300000000, 928000000}};

static void fake_init(fake_radio_t *fake)
{
    memset(fake, 0, sizeof(*fake));
    pthread_mutex_init(&fake->lock, NULL);
    pthread_cond_init(&fake->changed, NULL);
}

static void fake_destroy(fake_radio_t *fake)
{
    pthread_cond_destroy(&fake->changed);
    pthread_mutex_destroy(&fake->lock);
}

static ls_radio_err_t fake_configure(void *ctx,
                                     const ls_radio_iq_config_t *requested,
                                     ls_radio_iq_config_t *actual)
{
    fake_radio_t *fake = ctx;
    if (fake->configure_error != LS_RADIO_OK)
        return fake->configure_error;
    *actual = *requested;
    actual->bandwidth_hz = requested->bandwidth_hz != 0
                               ? requested->bandwidth_hz
                               : requested->sample_rate_hz;
    return LS_RADIO_OK;
}

static ls_radio_err_t fake_gain(void *ctx, ls_radio_gain_mode_t mode,
                                int requested, int *actual)
{
    fake_radio_t *fake = ctx;
    if (fake->gain_error != LS_RADIO_OK) return fake->gain_error;
    *actual = mode == LS_RADIO_GAIN_AUTO ? 0 : requested - (requested % 10);
    return LS_RADIO_OK;
}

static ls_radio_err_t fake_start(void *ctx)
{
    fake_radio_t *fake = ctx;
    if (fake->start_error != LS_RADIO_OK) return fake->start_error;
    pthread_mutex_lock(&fake->lock);
    fake->started = true;
    fake->cancelled = false;
    pthread_mutex_unlock(&fake->lock);
    return LS_RADIO_OK;
}

static ls_radio_err_t fake_read(void *ctx, void *dst, size_t bytes,
                                uint32_t timeout_ms, size_t *read_bytes)
{
    fake_radio_t *fake = ctx;
    (void)timeout_ms;
    if (fake->read_error != LS_RADIO_OK) return fake->read_error;
    pthread_mutex_lock(&fake->lock);
    fake->read_entered = true;
    pthread_cond_broadcast(&fake->changed);
    while (fake->blocking && !fake->cancelled)
        pthread_cond_wait(&fake->changed, &fake->lock);
    bool cancelled = fake->cancelled;
    pthread_mutex_unlock(&fake->lock);
    if (cancelled) return LS_RADIO_ERR_STOPPED;
    size_t amount = bytes < 4 ? bytes : 4;
    memset(dst, 0x80, amount);
    *read_bytes = amount;
    return LS_RADIO_OK;
}

static ls_radio_err_t fake_retune(void *ctx, uint64_t requested, bool fast,
                                  uint64_t *actual)
{
    fake_radio_t *fake = ctx;
    if (fake->tune_error != LS_RADIO_OK) return fake->tune_error;
    fake->last_retune_fast = fast;
    ++fake->retune_count;
    *actual = requested - (requested % 1000);
    return LS_RADIO_OK;
}

static ls_radio_err_t fake_stop(void *ctx)
{
    fake_radio_t *fake = ctx;
    if (fake->stop_error != LS_RADIO_OK) return fake->stop_error;
    pthread_mutex_lock(&fake->lock);
    fake->started = false;
    pthread_mutex_unlock(&fake->lock);
    return LS_RADIO_OK;
}

static ls_radio_err_t fake_packet_configure(
    void *ctx, const ls_radio_packet_config_t *requested,
    ls_radio_packet_config_t *actual)
{
    fake_radio_t *fake = ctx;
    if (fake->configure_error != LS_RADIO_OK)
        return fake->configure_error;
    *actual = *requested;
    return LS_RADIO_OK;
}

static ls_radio_err_t fake_packet_read(void *ctx, ls_radio_packet_t *packet,
                                       uint32_t timeout_ms)
{
    fake_radio_t *fake = ctx;
    (void)timeout_ms;
    if (fake->read_error != LS_RADIO_OK) return fake->read_error;
    packet->bytes = packet->capacity < 3 ? packet->capacity : 3;
    memset(packet->data, 0x5a, packet->bytes);
    packet->rssi_tenths_dbm = -650;
    packet->crc_ok = true;
    return LS_RADIO_OK;
}

static void fake_cancel(void *ctx)
{
    fake_radio_t *fake = ctx;
    pthread_mutex_lock(&fake->lock);
    fake->cancelled = true;
    pthread_cond_broadcast(&fake->changed);
    pthread_mutex_unlock(&fake->lock);
}

static ls_radio_err_t fake_recover(void *ctx)
{
    fake_radio_t *fake = ctx;
    ++fake->recover_count;
    return LS_RADIO_OK;
}

static const ls_radio_driver_ops_t s_fake_ops = {
    .iq_configure = fake_configure,
    .iq_set_gain = fake_gain,
    .iq_start = fake_start,
    .iq_read = fake_read,
    .iq_retune = fake_retune,
    .iq_stop = fake_stop,
    .packet_rx_configure = fake_packet_configure,
    .packet_rx_start = fake_start,
    .packet_rx_read = fake_packet_read,
    .packet_rx_stop = fake_stop,
    .cancel_read = fake_cancel,
    .recover = fake_recover,
};

static ls_radio_err_t register_iq(const char *id, fake_radio_t *fake)
{
    ls_radio_endpoint_t endpoint = {
        .endpoint_id = id,
        .name = "Fake IQ radio",
        .capabilities = LS_RADIO_RX_IQ_U8,
        .duplex = LS_RADIO_DUPLEX_HALF,
        .iq_formats = LS_RADIO_IQ_FORMAT_U8_INTERLEAVED,
        .frequency_ranges = s_iq_freq,
        .frequency_range_count = 1,
        .sample_rate_ranges = s_iq_rates,
        .sample_rate_range_count = 2,
        .ops = &s_fake_ops,
        .driver_ctx = fake,
    };
    return ls_radio_endpoint_register(&endpoint);
}

static ls_radio_err_t register_packet(const char *id, fake_radio_t *fake)
{
    ls_radio_endpoint_t endpoint = {
        .endpoint_id = id,
        .name = "Fake packet radio",
        .capabilities = LS_RADIO_RX_PACKET | LS_RADIO_TX_PACKET,
        .packet_formats = LS_RADIO_PACKET_FORMAT_FSK,
        .frequency_ranges = s_packet_freq,
        .frequency_range_count = 1,
        .ops = &s_fake_ops,
        .driver_ctx = fake,
    };
    return ls_radio_endpoint_register(&endpoint);
}

static ls_radio_requirements_t iq_requirements(uint32_t rate)
{
    ls_radio_requirements_t requirements = {
        .required_caps = LS_RADIO_RX_IQ_U8,
        .min_hz = 100000000,
        .max_hz = 1100000000,
        .sample_rate_hz = rate,
        .iq_format = LS_RADIO_IQ_FORMAT_U8_INTERLEAVED,
    };
    return requirements;
}

static void configure_and_start(ls_radio_session_t *session)
{
    ls_radio_iq_config_t requested = {
        .center_hz = 1090000000,
        .sample_rate_hz = 2000000,
        .gain_mode = LS_RADIO_GAIN_MANUAL,
        .gain_tenths_db = 496,
    };
    ls_radio_iq_config_t actual;
    LS_EQ_INT(ls_radio_iq_configure(session, &requested, &actual), LS_RADIO_OK);
    LS_EQ_UINT(actual.bandwidth_hz, 2000000);
    LS_EQ_INT(ls_radio_iq_start(session), LS_RADIO_OK);
    LS_EQ_INT(ls_radio_iq_start(session), LS_RADIO_ERR_BUSY);
}

LS_CASE(capability_and_constraint_selection)
{
    fake_radio_t packet, iq;
    fake_init(&packet);
    fake_init(&iq);
    LS_EQ_INT(register_packet("test.packet", &packet), LS_RADIO_OK);
    LS_EQ_INT(register_iq("test.iq.select", &iq), LS_RADIO_OK);
    LS_EQ_INT(register_iq("test.iq.select", &iq), LS_RADIO_ERR_EXISTS);
    LS_CHECK(ls_radio_endpoint_count() >= 2);

    ls_radio_requirements_t requirements = iq_requirements(2000000);
    ls_radio_session_t *session = NULL;
    LS_EQ_INT(ls_radio_acquire("adsb", &requirements, &session), LS_RADIO_OK);
    LS_CHECK(session != NULL);
    ls_radio_endpoint_info_t info;
    LS_EQ_INT(ls_radio_endpoint_get("test.iq.select", &info), LS_RADIO_OK);
    LS_CHECK(info.present);
    LS_CHECK(info.leased);
    LS_EQ_STR(info.owner, "adsb");
    LS_EQ_UINT(info.capabilities, LS_RADIO_RX_IQ_U8);
    LS_EQ_INT(info.duplex, LS_RADIO_DUPLEX_HALF);
    LS_EQ_UINT(info.iq_formats, LS_RADIO_IQ_FORMAT_U8_INTERLEAVED);
    ls_radio_release(session);

    ls_radio_requirements_t packet_requirements = {
        .required_caps = LS_RADIO_RX_PACKET,
        .min_hz = 433000000,
        .max_hz = 433920000,
        .packet_format = LS_RADIO_PACKET_FORMAT_FSK,
        .preferred_endpoint_id = "test.packet",
    };
    LS_EQ_INT(ls_radio_acquire("packet-test", &packet_requirements, &session),
              LS_RADIO_OK);
    ls_radio_packet_config_t packet_requested = {
        .center_hz = 433920000,
        .bandwidth_hz = 100000,
        .bitrate = 38400,
        .deviation_hz = 20000,
        .format = LS_RADIO_PACKET_FORMAT_FSK,
        .max_payload_bytes = 64,
    };
    ls_radio_packet_config_t packet_actual;
    LS_EQ_INT(ls_radio_packet_rx_configure(session, &packet_requested,
                                            &packet_actual), LS_RADIO_OK);
    LS_EQ_UINT(packet_actual.center_hz, 433920000);
    LS_EQ_INT(ls_radio_packet_rx_start(session), LS_RADIO_OK);
    uint8_t payload[8];
    ls_radio_packet_t received = {.data = payload, .capacity = sizeof(payload)};
    LS_EQ_INT(ls_radio_packet_rx_read(session, &received, 10), LS_RADIO_OK);
    LS_EQ_UINT(received.bytes, 3);
    LS_EQ_INT(received.rssi_tenths_dbm, -650);
    LS_CHECK(received.crc_ok);
    LS_EQ_INT(ls_radio_packet_rx_stop(session), LS_RADIO_OK);
    LS_EQ_INT(ls_radio_endpoint_get("test.packet", &info), LS_RADIO_OK);
    LS_EQ_UINT(info.packets_read, 1);
    LS_EQ_UINT(info.bytes_read, 3);
    ls_radio_release(session);

    requirements = iq_requirements(500000);
    LS_EQ_INT(ls_radio_acquire("fm", &requirements, &session),
              LS_RADIO_ERR_UNAVAILABLE);
    LS_EQ_INT(ls_radio_endpoint_unregister("test.iq.select"), LS_RADIO_OK);
    LS_EQ_INT(ls_radio_endpoint_unregister("test.packet"), LS_RADIO_OK);
    fake_destroy(&iq);
    fake_destroy(&packet);
}

LS_CASE(exclusive_lease_and_preferred_endpoint)
{
    fake_radio_t first, second;
    fake_init(&first);
    fake_init(&second);
    LS_EQ_INT(register_iq("test.iq.first", &first), LS_RADIO_OK);
    LS_EQ_INT(register_iq("test.iq.second", &second), LS_RADIO_OK);
    ls_radio_requirements_t requirements = iq_requirements(240000);
    requirements.preferred_endpoint_id = "test.iq.first";
    ls_radio_session_t *a = NULL;
    ls_radio_session_t *b = NULL;
    LS_EQ_INT(ls_radio_acquire("p25", &requirements, &a), LS_RADIO_OK);
    LS_EQ_INT(ls_radio_acquire("rec", &requirements, &b), LS_RADIO_ERR_BUSY);
    requirements.preferred_endpoint_id = NULL;
    LS_EQ_INT(ls_radio_acquire("rec", &requirements, &b), LS_RADIO_OK);
    LS_CHECK(a != b);
    ls_radio_release(b);
    ls_radio_release(a);
    LS_EQ_INT(ls_radio_endpoint_unregister("test.iq.first"), LS_RADIO_OK);
    LS_EQ_INT(ls_radio_endpoint_unregister("test.iq.second"), LS_RADIO_OK);
    fake_destroy(&second);
    fake_destroy(&first);
}

typedef struct {
    unsigned attached;
    unsigned detached;
    char last_id[LS_RADIO_ENDPOINT_ID_MAX];
} endpoint_events_t;

static endpoint_events_t s_endpoint_events;

static void note_endpoint_event(const ls_radio_endpoint_event_t *event,
                                void *user)
{
    endpoint_events_t *events = user;
    if (event->kind == LS_RADIO_ENDPOINT_ATTACHED) ++events->attached;
    else ++events->detached;
    strncpy(events->last_id, event->endpoint_id,
            sizeof(events->last_id) - 1);
}

LS_CASE(service_publishes_attach_detach_and_park_recovery_is_endpoint_scoped)
{
    fake_radio_t iq, packet;
    fake_init(&iq);
    fake_init(&packet);
    memset(&s_endpoint_events, 0, sizeof(s_endpoint_events));
    LS_CHECK(ls_radio_endpoint_subscribe(note_endpoint_event,
                                         &s_endpoint_events) >= 0);

    LS_EQ_INT(register_iq("test.lifecycle.iq", &iq), LS_RADIO_OK);
    LS_EQ_INT(register_packet("test.lifecycle.packet", &packet), LS_RADIO_OK);
    LS_EQ_UINT(s_endpoint_events.attached, 2);

    ls_radio_requirements_t requirements = iq_requirements(240000);
    ls_radio_session_t *session = NULL;
    LS_EQ_INT(ls_radio_acquire("p25", &requirements, &session), LS_RADIO_OK);
    LS_EQ_INT(ls_radio_endpoint_recover("test.lifecycle.iq"),
              LS_RADIO_ERR_BUSY);

    /* App parking is the app's session release, not a global pipe teardown. */
    ls_radio_release(session);
    LS_EQ_INT(ls_radio_endpoint_recover("test.lifecycle.iq"), LS_RADIO_OK);
    LS_EQ_UINT(iq.recover_count, 1);
    LS_EQ_UINT(packet.recover_count, 0);

    ls_radio_endpoint_info_t info;
    LS_EQ_INT(ls_radio_endpoint_get("test.lifecycle.packet", &info),
              LS_RADIO_OK);
    LS_CHECK(info.present);
    LS_EQ_INT(ls_radio_endpoint_unregister("test.lifecycle.iq"), LS_RADIO_OK);
    LS_EQ_UINT(s_endpoint_events.detached, 1);
    LS_EQ_STR(s_endpoint_events.last_id, "test.lifecycle.iq");
    LS_EQ_INT(ls_radio_endpoint_get("test.lifecycle.packet", &info),
              LS_RADIO_OK);
    LS_CHECK(info.present);
    LS_EQ_INT(ls_radio_endpoint_unregister("test.lifecycle.packet"),
              LS_RADIO_OK);
    LS_EQ_UINT(s_endpoint_events.detached, 2);
    fake_destroy(&packet);
    fake_destroy(&iq);
}

LS_CASE(actual_values_and_driver_errors_propagate)
{
    fake_radio_t fake;
    fake_init(&fake);
    LS_EQ_INT(register_iq("test.iq.errors", &fake), LS_RADIO_OK);
    ls_radio_requirements_t requirements = iq_requirements(2000000);
    ls_radio_session_t *session = NULL;
    LS_EQ_INT(ls_radio_acquire("adsb", &requirements, &session), LS_RADIO_OK);

    ls_radio_iq_config_t requested = {
        .center_hz = 1090000000,
        .sample_rate_hz = 2000000,
        .gain_mode = LS_RADIO_GAIN_MANUAL,
        .gain_tenths_db = 496,
    };
    ls_radio_iq_config_t actual;
    fake.configure_error = LS_RADIO_ERR_IO;
    LS_EQ_INT(ls_radio_iq_configure(session, &requested, &actual),
              LS_RADIO_ERR_IO);
    fake.configure_error = LS_RADIO_OK;
    LS_EQ_INT(ls_radio_iq_configure(session, &requested, &actual), LS_RADIO_OK);
    LS_EQ_UINT(actual.bandwidth_hz, 2000000);
    int actual_gain = 0;
    LS_EQ_INT(ls_radio_iq_set_gain(session, LS_RADIO_GAIN_MANUAL, 497,
                                   &actual_gain), LS_RADIO_OK);
    LS_EQ_INT(actual_gain, 490);
    uint64_t actual_hz = 0;
    LS_EQ_INT(ls_radio_iq_retune(session, 101100123, true, &actual_hz),
              LS_RADIO_OK);
    LS_EQ_UINT(actual_hz, 101100000);
    LS_CHECK(fake.last_retune_fast);
    LS_EQ_UINT(fake.retune_count, 1);

    LS_EQ_INT(ls_radio_iq_retune(session, 101200123, false, &actual_hz),
              LS_RADIO_OK);
    LS_CHECK(!fake.last_retune_fast);
    LS_EQ_UINT(fake.retune_count, 2);

    LS_EQ_INT(ls_radio_iq_start(session), LS_RADIO_OK);
    unsigned char samples[16];
    size_t read_bytes = 0;
    LS_EQ_INT(ls_radio_iq_read(session, samples, sizeof(samples), 10,
                               &read_bytes), LS_RADIO_OK);
    LS_EQ_UINT(read_bytes, 4);
    fake.read_error = LS_RADIO_ERR_IO;
    LS_EQ_INT(ls_radio_iq_read(session, samples, sizeof(samples), 10,
                               &read_bytes), LS_RADIO_ERR_IO);
    fake.read_error = LS_RADIO_OK;
    LS_EQ_INT(ls_radio_iq_stop(session), LS_RADIO_OK);

    fake.gain_error = LS_RADIO_ERR_IO;
    LS_EQ_INT(ls_radio_iq_set_gain(session, LS_RADIO_GAIN_AUTO, 0,
                                   &actual_gain), LS_RADIO_ERR_IO);
    fake.tune_error = LS_RADIO_ERR_TIMEOUT;
    LS_EQ_INT(ls_radio_iq_retune(session, 102000000, false, &actual_hz),
              LS_RADIO_ERR_TIMEOUT);
    ls_radio_endpoint_info_t info;
    LS_EQ_INT(ls_radio_endpoint_get("test.iq.errors", &info), LS_RADIO_OK);
    LS_EQ_INT(info.last_error, LS_RADIO_ERR_TIMEOUT);
    LS_EQ_UINT(info.bytes_read, 4);
    LS_EQ_STR(ls_radio_err_name(info.last_error), "timeout");

    ls_radio_release(session);
    LS_EQ_INT(ls_radio_endpoint_unregister("test.iq.errors"), LS_RADIO_OK);
    fake_destroy(&fake);
}

typedef struct {
    ls_radio_session_t *session;
    ls_radio_err_t result;
    size_t bytes;
} read_thread_arg_t;

static void *read_thread(void *arg_)
{
    read_thread_arg_t *arg = arg_;
    unsigned char buffer[32];
    arg->result = ls_radio_iq_read(arg->session, buffer, sizeof(buffer),
                                   10000, &arg->bytes);
    return NULL;
}

static void wait_for_read(fake_radio_t *fake)
{
    pthread_mutex_lock(&fake->lock);
    while (!fake->read_entered)
        pthread_cond_wait(&fake->changed, &fake->lock);
    pthread_mutex_unlock(&fake->lock);
}

LS_CASE(removal_cancels_blocked_read_and_invalidates_session)
{
    fake_radio_t fake, packet;
    fake_init(&fake);
    fake_init(&packet);
    fake.blocking = true;
    LS_EQ_INT(register_iq("test.iq.remove", &fake), LS_RADIO_OK);
    LS_EQ_INT(register_packet("test.packet.survives", &packet), LS_RADIO_OK);
    ls_radio_requirements_t requirements = iq_requirements(2000000);
    ls_radio_session_t *session = NULL;
    LS_EQ_INT(ls_radio_acquire("adsb", &requirements, &session), LS_RADIO_OK);
    configure_and_start(session);

    read_thread_arg_t read_arg = {.session = session};
    pthread_t thread;
    pthread_create(&thread, NULL, read_thread, &read_arg);
    wait_for_read(&fake);
    LS_EQ_INT(ls_radio_endpoint_unregister("test.iq.remove"), LS_RADIO_OK);
    pthread_join(thread, NULL);
    LS_EQ_INT(read_arg.result, LS_RADIO_ERR_DISCONNECTED);
    LS_EQ_UINT(read_arg.bytes, 0);
    unsigned char byte;
    size_t bytes = 0;
    LS_EQ_INT(ls_radio_iq_read(session, &byte, 1, 0, &bytes),
              LS_RADIO_ERR_DISCONNECTED);
    ls_radio_endpoint_info_t info;
    LS_EQ_INT(ls_radio_endpoint_get("test.iq.remove", &info), LS_RADIO_OK);
    LS_CHECK(!info.present);
    LS_EQ_INT(info.last_error, LS_RADIO_ERR_DISCONNECTED);
    LS_EQ_INT(ls_radio_endpoint_get("test.packet.survives", &info),
              LS_RADIO_OK);
    LS_CHECK(info.present);
    LS_EQ_INT(register_iq("test.iq.remove", &fake), LS_RADIO_ERR_BUSY);
    ls_radio_release(session);
    LS_EQ_INT(register_iq("test.iq.remove", &fake), LS_RADIO_OK);
    LS_EQ_INT(ls_radio_endpoint_unregister("test.iq.remove"), LS_RADIO_OK);
    LS_EQ_INT(ls_radio_endpoint_unregister("test.packet.survives"),
              LS_RADIO_OK);
    fake_destroy(&packet);
    fake_destroy(&fake);
}

LS_CASE(stop_cancels_blocked_read)
{
    fake_radio_t fake;
    fake_init(&fake);
    fake.blocking = true;
    LS_EQ_INT(register_iq("test.iq.stop", &fake), LS_RADIO_OK);
    ls_radio_requirements_t requirements = iq_requirements(2000000);
    ls_radio_session_t *session = NULL;
    LS_EQ_INT(ls_radio_acquire("adsb", &requirements, &session), LS_RADIO_OK);
    configure_and_start(session);

    read_thread_arg_t read_arg = {.session = session};
    pthread_t thread;
    pthread_create(&thread, NULL, read_thread, &read_arg);
    wait_for_read(&fake);
    LS_EQ_INT(ls_radio_iq_stop(session), LS_RADIO_OK);
    pthread_join(thread, NULL);
    LS_EQ_INT(read_arg.result, LS_RADIO_ERR_STOPPED);
    ls_radio_release(session);
    LS_EQ_INT(ls_radio_endpoint_unregister("test.iq.stop"), LS_RADIO_OK);
    fake_destroy(&fake);
}
