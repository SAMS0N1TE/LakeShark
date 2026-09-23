/* LS_TEST_SOURCES: ${FW}/components/lakeshark/radio/radio_endpoint.c ${FW}/components/lakeshark/radio/radio_health.c */
#include "ls_test.h"
#include "radio_endpoint.h"
#include "radio_health.h"
#include "esp_timer.h"

#include <string.h>

/* the T-Display-P4 incident of 2026-09-11, on the bench. A dongle
   enumerates, registers, and then fails every control request. Nothing
   streams, so there is never a stall - the watchdog used to sit at "ok"
   while FM failed its open with io every ~150 ms. Each case registers its
   own endpoint id, so health slots from one case do not leak into another. */

typedef struct {
    ls_radio_err_t configure_result;
} fake_t;

static ls_radio_err_t f_configure(void *ctx,
                                  const ls_radio_iq_config_t *requested,
                                  ls_radio_iq_config_t *actual)
{
    fake_t *fake = ctx;
    if (fake->configure_result == LS_RADIO_OK) *actual = *requested;
    return fake->configure_result;
}

static ls_radio_err_t f_gain(void *ctx, ls_radio_gain_mode_t mode, int gain,
                             int *actual)
{
    (void)ctx; (void)mode;
    *actual = gain;
    return LS_RADIO_OK;
}

static ls_radio_err_t f_start(void *ctx) { (void)ctx; return LS_RADIO_OK; }
static ls_radio_err_t f_stop(void *ctx) { (void)ctx; return LS_RADIO_OK; }
static void f_cancel(void *ctx) { (void)ctx; }

static ls_radio_err_t f_read(void *ctx, void *dst, size_t bytes,
                             uint32_t timeout_ms, size_t *read_bytes)
{
    (void)ctx; (void)timeout_ms;
    memset(dst, 0x80, bytes);
    *read_bytes = bytes;
    return LS_RADIO_OK;
}

static ls_radio_err_t f_retune(void *ctx, uint64_t requested, bool fast,
                               uint64_t *actual)
{
    (void)ctx; (void)fast;
    *actual = requested;
    return LS_RADIO_OK;
}

static const ls_radio_driver_ops_t OPS = {
    .iq_configure = f_configure,
    .iq_set_gain = f_gain,
    .iq_start = f_start,
    .iq_read = f_read,
    .iq_retune = f_retune,
    .iq_stop = f_stop,
    .cancel_read = f_cancel,
};

static const ls_radio_range_t FREQ[] = {{24000000, 1766000000}};
static const ls_radio_range_t RATE[] = {{225001, 3200000}};

static void register_as(const char *id, fake_t *fake)
{
    const ls_radio_endpoint_t endpoint = {
        .endpoint_id = id,
        .name = "control fault test",
        .capabilities = LS_RADIO_RX_IQ_U8,
        .iq_formats = LS_RADIO_IQ_FORMAT_U8_INTERLEAVED,
        .frequency_ranges = FREQ,
        .frequency_range_count = 1,
        .sample_rate_ranges = RATE,
        .sample_rate_range_count = 1,
        .ops = &OPS,
        .driver_ctx = fake,
    };
    LS_EQ_INT(ls_radio_endpoint_register(&endpoint), LS_RADIO_OK);
}

/* One open the way FM makes it: acquire, configure, start, release. */
static ls_radio_err_t fm_style_open(const char *id, uint64_t center_hz)
{
    const ls_radio_requirements_t requirements = {
        .required_caps = LS_RADIO_RX_IQ_U8,
        .sample_rate_hz = 240000,
        .iq_format = LS_RADIO_IQ_FORMAT_U8_INTERLEAVED,
        .preferred_endpoint_id = id,
    };
    ls_radio_session_t *session = NULL;
    ls_radio_err_t error = ls_radio_acquire("fm", &requirements, &session);
    if (error != LS_RADIO_OK) return error;
    const ls_radio_iq_config_t requested = {
        .center_hz = center_hz,
        .sample_rate_hz = 240000,
        .gain_mode = LS_RADIO_GAIN_AUTO,
    };
    ls_radio_iq_config_t actual;
    error = ls_radio_iq_configure(session, &requested, &actual);
    if (error == LS_RADIO_OK) error = ls_radio_iq_start(session);
    ls_radio_release(session);
    return error;
}

static uint32_t control_io(const char *id)
{
    ls_radio_endpoint_info_t info;
    LS_EQ_INT(ls_radio_endpoint_get(id, &info), LS_RADIO_OK);
    return info.control_io_errors;
}

static rh_state_t state_of(const char *id)
{
    radio_health_snapshot_t health;
    LS_CHECK(radio_health_get(id, &health));
    return health.state;
}

static unsigned s_cycles;
static unsigned s_recoveries;
static bool s_cycle_accepts;
static char s_cycle_id[LS_RADIO_ENDPOINT_ID_MAX];

static bool power_cycle(const char *endpoint_id)
{
    ++s_cycles;
    strncpy(s_cycle_id, endpoint_id, sizeof(s_cycle_id) - 1);
    return s_cycle_accepts;
}

static void request_recovery(const char *endpoint_id)
{
    (void)endpoint_id;
    ++s_recoveries;
}

static void health_with(const char *power_cycle_id, bool accepts)
{
    s_cycles = 0;
    s_recoveries = 0;
    s_cycle_accepts = accepts;
    memset(s_cycle_id, 0, sizeof(s_cycle_id));
    const radio_health_hooks_t hooks = {
        .request_recovery = request_recovery,
        .power_cycle = power_cycle_id ? power_cycle : NULL,
        .power_cycle_endpoint_id = power_cycle_id,
    };
    radio_health_init(&hooks);
}

/* Register, then let the watchdog settle it to "ok" with nothing streaming:
   the state the incident's `rtl` line reported. */
static void settle_idle(const char *id, fake_t *fake)
{
    register_as(id, fake);
    ls_shim_time_advance(3000000);
    radio_health_tick();
    LS_EQ_INT(state_of(id), RH_OK);
}

LS_CASE(control_io_is_a_run_that_success_and_a_new_attach_both_end)
{
    ls_shim_time_set(1000);
    fake_t fake = {.configure_result = LS_RADIO_ERR_IO};
    const uint32_t before = ls_radio_endpoint_generation();
    register_as("cf.count", &fake);
    LS_EQ_UINT(ls_radio_endpoint_generation(), before + 1);

    LS_EQ_INT(fm_style_open("cf.count", 100000000), LS_RADIO_ERR_IO);
    LS_EQ_INT(fm_style_open("cf.count", 100000000), LS_RADIO_ERR_IO);
    LS_EQ_UINT(control_io("cf.count"), 2);

    /* A refusal before the driver says nothing about the device. */
    LS_EQ_INT(fm_style_open("cf.count", 5000000000ull),
              LS_RADIO_ERR_UNSUPPORTED);
    LS_EQ_UINT(control_io("cf.count"), 2);

    /* Stop is host-side and always succeeds; it must not clear the run, or
       FM's release between opens would reset it every time. */
    {
        const ls_radio_requirements_t requirements = {
            .required_caps = LS_RADIO_RX_IQ_U8,
            .preferred_endpoint_id = "cf.count",
        };
        ls_radio_session_t *session = NULL;
        LS_EQ_INT(ls_radio_acquire("fm", &requirements, &session), LS_RADIO_OK);
        LS_EQ_INT(ls_radio_iq_stop(session), LS_RADIO_OK);
        ls_radio_release(session);
    }
    LS_EQ_UINT(control_io("cf.count"), 2);

    fake.configure_result = LS_RADIO_OK;
    LS_EQ_INT(fm_style_open("cf.count", 100000000), LS_RADIO_OK);
    LS_EQ_UINT(control_io("cf.count"), 0);

    fake.configure_result = LS_RADIO_ERR_IO;
    LS_EQ_INT(fm_style_open("cf.count", 100000000), LS_RADIO_ERR_IO);
    LS_EQ_UINT(control_io("cf.count"), 1);
    LS_EQ_INT(ls_radio_endpoint_unregister("cf.count"), LS_RADIO_OK);
    LS_EQ_UINT(ls_radio_endpoint_generation(), before + 2);
    register_as("cf.count", &fake);
    LS_EQ_UINT(control_io("cf.count"), 0);
    LS_EQ_INT(ls_radio_endpoint_unregister("cf.count"), LS_RADIO_OK);
}

LS_CASE(an_idle_dongle_that_answers_nothing_is_power_cycled_not_left_ok)
{
    ls_shim_time_set(1000);
    health_with("cf.wedged", true);
    fake_t fake = {.configure_result = LS_RADIO_ERR_IO};
    settle_idle("cf.wedged", &fake);

    /* Short of the threshold: still ok, nothing asked for. */
    for (unsigned i = 1; i < RADIO_HEALTH_CONTROL_IO_FAULTS; ++i)
        LS_EQ_INT(fm_style_open("cf.wedged", 100000000), LS_RADIO_ERR_IO);
    radio_health_tick();
    LS_EQ_INT(state_of("cf.wedged"), RH_OK);
    LS_EQ_UINT(s_cycles, 0);

    LS_EQ_INT(fm_style_open("cf.wedged", 100000000), LS_RADIO_ERR_IO);
    radio_health_tick();
    LS_EQ_INT(state_of("cf.wedged"), RH_POWER_CYCLING);
    LS_EQ_UINT(s_cycles, 1);
    LS_EQ_STR(s_cycle_id, "cf.wedged");
    /* In-place recovery re-claims an interface on the host; it cannot reach
       a device that ignores its control pipe, so it is skipped. */
    LS_EQ_UINT(s_recoveries, 0);

    char report[160];
    radio_health_report("cf.wedged", report, sizeof(report));
    LS_CHECK_MSG(strstr(report, "powercycle") && strstr(report, "ctlio=3"),
                 "report: %s", report);

    /* The port reset tears the endpoint down and it enumerates afresh. */
    LS_EQ_INT(ls_radio_endpoint_unregister("cf.wedged"), LS_RADIO_OK);
    LS_EQ_INT(state_of("cf.wedged"), RH_ABSENT);
    register_as("cf.wedged", &fake);
    LS_EQ_INT(state_of("cf.wedged"), RH_SETTLING);
    LS_EQ_UINT(control_io("cf.wedged"), 0);
    LS_EQ_INT(ls_radio_endpoint_unregister("cf.wedged"), LS_RADIO_OK);
}

LS_CASE(with_no_power_cycle_the_endpoint_says_failed_and_asks_again_later)
{
    ls_shim_time_set(1000);
    health_with(NULL, true);
    fake_t fake = {.configure_result = LS_RADIO_ERR_IO};
    settle_idle("cf.nohook", &fake);

    for (unsigned i = 0; i < RADIO_HEALTH_CONTROL_IO_FAULTS; ++i)
        LS_EQ_INT(fm_style_open("cf.nohook", 100000000), LS_RADIO_ERR_IO);
    radio_health_tick();
    LS_EQ_INT(state_of("cf.nohook"), RH_FAILED);
    LS_EQ_UINT(s_recoveries, 0);

    /* Quiet while failed, then one retry per RH_FAILED_RETRY_S (120 s). */
    ls_shim_time_advance(1000000);
    radio_health_tick();
    LS_EQ_INT(state_of("cf.nohook"), RH_FAILED);
    ls_shim_time_advance(120000000);
    radio_health_tick();
    LS_EQ_INT(state_of("cf.nohook"), RH_STALLED);
    radio_health_tick();
    LS_EQ_INT(state_of("cf.nohook"), RH_FAILED);
    LS_EQ_UINT(s_recoveries, 0);
    LS_EQ_INT(ls_radio_endpoint_unregister("cf.nohook"), LS_RADIO_OK);
}

LS_CASE(a_refused_power_cycle_ends_failed_and_is_not_asked_again_at_once)
{
    ls_shim_time_set(1000);
    health_with("cf.refused", false);
    fake_t fake = {.configure_result = LS_RADIO_ERR_IO};
    settle_idle("cf.refused", &fake);

    for (unsigned i = 0; i < RADIO_HEALTH_CONTROL_IO_FAULTS; ++i)
        LS_EQ_INT(fm_style_open("cf.refused", 100000000), LS_RADIO_ERR_IO);
    radio_health_tick();
    LS_EQ_INT(state_of("cf.refused"), RH_FAILED);
    LS_EQ_UINT(s_cycles, 1);
    radio_health_tick();
    radio_health_tick();
    LS_EQ_UINT(s_cycles, 1);
    LS_EQ_INT(ls_radio_endpoint_unregister("cf.refused"), LS_RADIO_OK);
}

LS_CASE(only_the_named_endpoint_is_ever_power_cycled)
{
    ls_shim_time_set(1000);
    health_with("cf.someone-else", true);
    fake_t fake = {.configure_result = LS_RADIO_ERR_IO};
    settle_idle("cf.unnamed", &fake);

    for (unsigned i = 0; i < RADIO_HEALTH_CONTROL_IO_FAULTS; ++i)
        LS_EQ_INT(fm_style_open("cf.unnamed", 100000000), LS_RADIO_ERR_IO);
    radio_health_tick();
    LS_EQ_INT(state_of("cf.unnamed"), RH_FAILED);
    LS_EQ_UINT(s_cycles, 0);
    LS_EQ_INT(ls_radio_endpoint_unregister("cf.unnamed"), LS_RADIO_OK);
}
