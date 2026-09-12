/* LS_TEST_SOURCES: ${FW}/components/lakeshark/radio/radio_endpoint.c ${FW}/components/lakeshark/radio/tx_policy.c ${FW}/components/lakeshark/radio/tx_broker.c ${FW}/components/apps/shell/ls_tx_confirmation.c shims/tx_driver_fake.c shims/ls_input_gesture_fake.c */
/* LS_TEST_DEFINE: CONFIG_LS_BOARD_P4_TOUCH_LCD_4B=1 */
#include "ls_test.h"
#include "esp_timer.h"
#include "ls_input_gesture_fake.h"
#include "shell/ls_input_gesture.h"
#include "shell/ls_tx_confirmation.h"
#include "tx_broker_private.h"
#include "tx_driver_fake.h"
#include "tx_policy_private.h"

#include <string.h>

#define TEST_PROFILE 7u
#define TEST_DOMAIN 0x5553u

static uint64_t s_gesture = 100;

static ls_radio_tx_band_policy_t policy_band(void)
{
    const ls_radio_tx_band_policy_t band = {
        .band_id = 23,
        .min_hz = 902000000,
        .max_hz = 928000000,
        .modulation_mask = LS_RADIO_TX_MOD_FSK | LS_RADIO_TX_MOD_LORA,
        .min_bandwidth_hz = 10000,
        .max_bandwidth_hz = 500000,
        .max_power_tenths_dbm = 300,
        .max_time_on_air_us = 100000,
        .max_dwell_us = 80000,
        .duty_window_us = 1000000,
        .duty_airtime_us = 200000,
    };
    return band;
}

static void install_policy(ls_radio_tx_band_policy_t *band)
{
    const ls_radio_tx_policy_t policy = {
        .profile_id = TEST_PROFILE,
        .regulatory_domain_id = TEST_DOMAIN,
        .bands = band,
        .band_count = 1,
    };
    LS_EQ_INT(ls_radio_tx_policy_install(&policy), LS_RADIO_TX_OK);
}

static ls_radio_tx_request_t valid_request(const char *endpoint,
                                            uint8_t payload[3])
{
    const ls_radio_tx_request_t request = {
        .endpoint_id = endpoint,
        .policy_profile_id = TEST_PROFILE,
        .regulatory_domain_id = TEST_DOMAIN,
        .frequency_hz = 915000000,
        .modulation = LS_RADIO_TX_MOD_FSK,
        .bandwidth_hz = 100000,
        .bitrate = 50000,
        .deviation_hz = 25000,
        .power_tenths_dbm = 100,
        .antenna_gain_tenths_dbi = 20,
        .antenna_gain_known = true,
        .payload = payload,
        .payload_bytes = 3,
    };
    return request;
}

static ls_input_gesture_id_t local_gesture(void)
{
    const ls_input_gesture_id_t gesture = {
        .sequence = ++s_gesture,
        .source = LS_INPUT_GESTURE_LOCAL_TOUCH,
    };
    return gesture;
}

static ls_radio_tx_token_t authorize(ls_radio_tx_plan_t *plan)
{
    ls_radio_tx_token_t token = {{0}};
    LS_EQ_INT(ls_tx_confirmation_present(plan), LS_RADIO_TX_OK);
    LS_CHECK(ls_tx_confirmation_pending());
    ls_input_gesture_fake_set(true, local_gesture());
    LS_EQ_INT(ls_tx_confirmation_accept(plan, &token),
              LS_RADIO_TX_OK);
    LS_CHECK(!ls_tx_confirmation_pending());
    return token;
}

static void setup_fake(ls_tx_driver_fake_t *fake, const char *endpoint)
{
    ls_radio_tx_broker_reset_for_test();
    ls_tx_confirmation_dismiss();
    ls_shim_time_set(1000000);
    ls_tx_driver_fake_init(fake, endpoint);
    LS_EQ_INT(ls_tx_driver_fake_register(fake), LS_RADIO_TX_OK);
}

static void teardown_fake(ls_tx_driver_fake_t *fake)
{
    ls_tx_confirmation_dismiss();
    ls_tx_driver_fake_unregister(fake);
    ls_radio_tx_broker_reset_for_test();
}

LS_CASE(no_configured_policy_denies_transmit)
{
    ls_tx_driver_fake_t fake;
    setup_fake(&fake, "tx.none");
    uint8_t payload[3] = {1, 2, 3};
    ls_radio_tx_request_t request = valid_request(fake.endpoint_id, payload);
    ls_radio_tx_plan_t plan;
    LS_EQ_INT(ls_radio_tx_plan(&request, &plan), LS_RADIO_TX_ERR_NO_POLICY);
    LS_EQ_UINT(fake.transmit_calls, 0);
    teardown_fake(&fake);
}

LS_CASE(plan_checks_endpoint_domain_band_modulation_bandwidth_power_and_time)
{
    ls_tx_driver_fake_t fake;
    setup_fake(&fake, "tx.validation");
    ls_radio_tx_band_policy_t band = policy_band();
    install_policy(&band);
    uint8_t payload[3] = {1, 2, 3};
    ls_radio_tx_request_t request = valid_request(fake.endpoint_id, payload);
    ls_radio_tx_plan_t plan;

    request.frequency_hz = 700000000;
    LS_EQ_INT(ls_radio_tx_plan(&request, &plan), LS_RADIO_TX_ERR_ENDPOINT);
    request = valid_request(fake.endpoint_id, payload);
    request.regulatory_domain_id++;
    LS_EQ_INT(ls_radio_tx_plan(&request, &plan), LS_RADIO_TX_ERR_DOMAIN);
    request = valid_request(fake.endpoint_id, payload);
    request.frequency_hz = 900000000;
    LS_EQ_INT(ls_radio_tx_plan(&request, &plan), LS_RADIO_TX_ERR_BAND);
    request = valid_request(fake.endpoint_id, payload);
    request.modulation = LS_RADIO_TX_MOD_OOK;
    LS_EQ_INT(ls_radio_tx_plan(&request, &plan),
              LS_RADIO_TX_ERR_MODULATION);
    request = valid_request(fake.endpoint_id, payload);
    request.bandwidth_hz = 9000;
    LS_EQ_INT(ls_radio_tx_plan(&request, &plan),
              LS_RADIO_TX_ERR_BANDWIDTH);
    request = valid_request(fake.endpoint_id, payload);
    request.antenna_gain_known = false;
    LS_EQ_INT(ls_radio_tx_plan(&request, &plan), LS_RADIO_TX_ERR_POWER);
    request = valid_request(fake.endpoint_id, payload);
    request.power_tenths_dbm = 281;
    LS_EQ_INT(ls_radio_tx_plan(&request, &plan), LS_RADIO_TX_ERR_POWER);

    request = valid_request(fake.endpoint_id, payload);
    fake.estimated_airtime_us = 110000;
    LS_EQ_INT(ls_radio_tx_plan(&request, &plan), LS_RADIO_TX_ERR_AIRTIME);
    fake.estimated_airtime_us = 90000;
    LS_EQ_INT(ls_radio_tx_plan(&request, &plan), LS_RADIO_TX_ERR_DWELL);
    fake.estimated_airtime_us = 50000;
    LS_EQ_INT(ls_radio_tx_plan(&request, &plan), LS_RADIO_TX_OK);
    LS_EQ_UINT(plan.policy_band_id, band.band_id);
    LS_EQ_UINT(plan.time_on_air_us, 50000);
    LS_EQ_UINT(plan.max_dwell_us, band.max_dwell_us);
    ls_radio_tx_cancel(plan.id);
    LS_EQ_UINT(fake.transmit_calls, 0);
    teardown_fake(&fake);
}

LS_CASE(local_exact_confirmation_authorizes_one_fake_packet)
{
    ls_tx_driver_fake_t fake;
    setup_fake(&fake, "tx.allowed");
    ls_radio_tx_band_policy_t band = policy_band();
    install_policy(&band);
    uint8_t payload[3] = {0xa1, 0xb2, 0xc3};
    ls_radio_tx_request_t request = valid_request(fake.endpoint_id, payload);
    ls_radio_tx_plan_t plan;
    LS_EQ_INT(ls_radio_tx_plan(&request, &plan), LS_RADIO_TX_OK);
    payload[0] = 0xff; /* the immutable broker copy remains what was shown */
    ls_radio_tx_token_t token = authorize(&plan);
    LS_EQ_INT(ls_radio_tx_commit(plan.id, token), LS_RADIO_TX_OK);
    LS_EQ_UINT(fake.transmit_calls, 1);
    LS_EQ_UINT(fake.last_payload[0], 0xa1);
    LS_EQ_UINT(fake.last_packet.payload_bytes, 3);
    LS_EQ_INT(ls_radio_tx_commit(plan.id, token), LS_RADIO_TX_ERR_AUTH);

    ls_radio_tx_audit_t audit;
    LS_CHECK(ls_radio_tx_policy_latest_audit(&audit));
    LS_CHECK(audit.rf_started);
    LS_CHECK(audit.completed);
    LS_EQ_UINT(audit.actual_airtime_us, 50000);
    LS_EQ_UINT(audit.charged_airtime_us, 50000);
    teardown_fake(&fake);
}

LS_CASE(remote_mutated_cancelled_exit_and_expired_plans_fail_closed)
{
    ls_tx_driver_fake_t fake;
    setup_fake(&fake, "tx.auth-denials");
    ls_radio_tx_band_policy_t band = policy_band();
    install_policy(&band);
    uint8_t payload[3] = {1, 2, 3};
    ls_radio_tx_request_t request = valid_request(fake.endpoint_id, payload);
    ls_radio_tx_plan_t plan;
    ls_radio_tx_token_t token = {{0}};

    LS_EQ_INT(ls_radio_tx_plan(&request, &plan), LS_RADIO_TX_OK);
    LS_EQ_INT(ls_tx_confirmation_present(&plan), LS_RADIO_TX_OK);
    const ls_input_gesture_id_t remote = {
        .sequence = ++s_gesture, .source = LS_INPUT_GESTURE_REMOTE};
    ls_input_gesture_fake_set(true, remote);
    LS_EQ_INT(ls_tx_confirmation_accept(&plan, &token),
              LS_RADIO_TX_ERR_AUTH);
    LS_EQ_INT(ls_radio_tx_commit(plan.id, token), LS_RADIO_TX_ERR_AUTH);

    LS_EQ_INT(ls_radio_tx_plan(&request, &plan), LS_RADIO_TX_OK);
    LS_EQ_INT(ls_tx_confirmation_present(&plan), LS_RADIO_TX_OK);
    plan.power_tenths_dbm++;
    ls_input_gesture_fake_set(true, local_gesture());
    LS_EQ_INT(ls_tx_confirmation_accept(&plan, &token),
              LS_RADIO_TX_ERR_AUTH);

    LS_EQ_INT(ls_radio_tx_plan(&request, &plan), LS_RADIO_TX_OK);
    token = authorize(&plan);
    ls_radio_tx_cancel(plan.id);
    LS_EQ_INT(ls_radio_tx_commit(plan.id, token), LS_RADIO_TX_ERR_AUTH);

    LS_EQ_INT(ls_radio_tx_plan(&request, &plan), LS_RADIO_TX_OK);
    token = authorize(&plan);
    ls_tx_confirmation_dismiss(); /* app/screen exit invalidates armed plan */
    LS_EQ_INT(ls_radio_tx_commit(plan.id, token), LS_RADIO_TX_ERR_AUTH);

    LS_EQ_INT(ls_radio_tx_plan(&request, &plan), LS_RADIO_TX_OK);
    token = authorize(&plan);
    ls_shim_time_advance(5000001);
    LS_EQ_INT(ls_radio_tx_commit(plan.id, token), LS_RADIO_TX_ERR_EXPIRED);
    LS_EQ_INT(ls_radio_tx_commit(plan.id, token), LS_RADIO_TX_ERR_AUTH);
    LS_EQ_UINT(fake.transmit_calls, 0);
    teardown_fake(&fake);
}

LS_CASE(token_is_bound_to_endpoint_and_lost_on_boot_restore)
{
    ls_tx_driver_fake_t first, second;
    setup_fake(&first, "tx.first");
    ls_tx_driver_fake_init(&second, "tx.second");
    LS_EQ_INT(ls_tx_driver_fake_register(&second), LS_RADIO_TX_OK);
    ls_radio_tx_band_policy_t band = policy_band();
    install_policy(&band);
    uint8_t payload[3] = {1, 2, 3};
    ls_radio_tx_request_t a = valid_request(first.endpoint_id, payload);
    ls_radio_tx_request_t b = valid_request(second.endpoint_id, payload);
    ls_radio_tx_plan_t plan_a, plan_b;
    LS_EQ_INT(ls_radio_tx_plan(&a, &plan_a), LS_RADIO_TX_OK);
    LS_EQ_INT(ls_radio_tx_plan(&b, &plan_b), LS_RADIO_TX_OK);
    ls_radio_tx_token_t token = authorize(&plan_a);
    LS_EQ_INT(ls_radio_tx_commit(plan_b.id, token), LS_RADIO_TX_ERR_AUTH);
    LS_EQ_INT(ls_radio_tx_commit(plan_a.id, token), LS_RADIO_TX_ERR_AUTH);
    ls_radio_tx_cancel(plan_b.id);

    LS_EQ_INT(ls_radio_tx_plan(&a, &plan_a), LS_RADIO_TX_OK);
    token = authorize(&plan_a);
    ls_radio_tx_broker_reset_for_test(); /* static broker state after boot */
    LS_EQ_INT(ls_radio_tx_commit(plan_a.id, token), LS_RADIO_TX_ERR_AUTH);
    LS_EQ_UINT(first.transmit_calls, 0);
    LS_EQ_UINT(second.transmit_calls, 0);
    ls_tx_driver_fake_unregister(&second);
    ls_tx_driver_fake_unregister(&first);
}

LS_CASE(commit_revalidates_policy_and_endpoint_immediately_before_keyup)
{
    ls_tx_driver_fake_t fake;
    setup_fake(&fake, "tx.revalidate");
    ls_radio_tx_band_policy_t band = policy_band();
    install_policy(&band);
    uint8_t payload[3] = {1, 2, 3};
    ls_radio_tx_request_t request = valid_request(fake.endpoint_id, payload);
    ls_radio_tx_plan_t plan;
    LS_EQ_INT(ls_radio_tx_plan(&request, &plan), LS_RADIO_TX_OK);
    ls_radio_tx_token_t token = authorize(&plan);
    band.max_power_tenths_dbm = 50;
    install_policy(&band);
    LS_EQ_INT(ls_radio_tx_commit(plan.id, token), LS_RADIO_TX_ERR_POWER);
    LS_EQ_INT(ls_radio_tx_commit(plan.id, token), LS_RADIO_TX_ERR_AUTH);

    band = policy_band();
    install_policy(&band);
    LS_EQ_INT(ls_radio_tx_plan(&request, &plan), LS_RADIO_TX_OK);
    token = authorize(&plan);
    LS_EQ_INT(ls_radio_endpoint_unregister(fake.endpoint_id), LS_RADIO_OK);
    LS_EQ_INT(ls_radio_tx_commit(plan.id, token), LS_RADIO_TX_ERR_ENDPOINT);
    LS_EQ_UINT(fake.transmit_calls, 0);
    (void)ls_radio_tx_driver_unregister(fake.endpoint_id);
    ls_radio_tx_broker_reset_for_test();
}

LS_CASE(driver_error_and_interruption_are_charged_and_not_retryable)
{
    ls_tx_driver_fake_t fake;
    setup_fake(&fake, "tx.error");
    ls_radio_tx_band_policy_t band = policy_band();
    install_policy(&band);
    uint8_t payload[3] = {1, 2, 3};
    ls_radio_tx_request_t request = valid_request(fake.endpoint_id, payload);
    ls_radio_tx_plan_t plan;
    LS_EQ_INT(ls_radio_tx_plan(&request, &plan), LS_RADIO_TX_OK);
    ls_radio_tx_token_t token = authorize(&plan);
    fake.transmit_error = LS_RADIO_ERR_IO;
    fake.report.completed = false;
    fake.report.rf_end_us = 11000;
    LS_EQ_INT(ls_radio_tx_commit(plan.id, token), LS_RADIO_TX_ERR_DRIVER);
    LS_EQ_INT(ls_radio_tx_commit(plan.id, token), LS_RADIO_TX_ERR_AUTH);
    ls_radio_tx_audit_t audit;
    LS_CHECK(ls_radio_tx_policy_latest_audit(&audit));
    LS_CHECK(audit.rf_started);
    LS_CHECK(!audit.completed);
    LS_EQ_UINT(audit.actual_airtime_us, 10000);
    LS_EQ_UINT(audit.charged_airtime_us, 50000);

    fake.report.rf_started = false;
    LS_EQ_INT(ls_radio_tx_plan(&request, &plan), LS_RADIO_TX_OK);
    token = authorize(&plan);
    LS_EQ_INT(ls_radio_tx_commit(plan.id, token), LS_RADIO_TX_ERR_DRIVER);
    LS_CHECK(ls_radio_tx_policy_latest_audit(&audit));
    LS_CHECK(!audit.rf_started);
    LS_EQ_UINT(audit.actual_airtime_us, 0);
    LS_EQ_UINT(audit.charged_airtime_us, 0);
    teardown_fake(&fake);
}

LS_CASE(rolling_duty_budget_is_reserved_and_expires_by_monotonic_time)
{
    ls_tx_driver_fake_t fake;
    setup_fake(&fake, "tx.duty");
    ls_radio_tx_band_policy_t band = policy_band();
    band.duty_airtime_us = 100000;
    install_policy(&band);
    uint8_t payload[3] = {1, 2, 3};
    ls_radio_tx_request_t request = valid_request(fake.endpoint_id, payload);

    for (unsigned i = 0; i < 2; ++i) {
        ls_radio_tx_plan_t plan;
        LS_EQ_INT(ls_radio_tx_plan(&request, &plan), LS_RADIO_TX_OK);
        ls_radio_tx_token_t token = authorize(&plan);
        LS_EQ_INT(ls_radio_tx_commit(plan.id, token), LS_RADIO_TX_OK);
    }
    ls_radio_tx_plan_t denied;
    LS_EQ_INT(ls_radio_tx_plan(&request, &denied), LS_RADIO_TX_ERR_DUTY);
    ls_shim_time_advance(band.duty_window_us);
    LS_EQ_INT(ls_radio_tx_plan(&request, &denied), LS_RADIO_TX_OK);
    ls_radio_tx_cancel(denied.id);
    LS_EQ_UINT(fake.transmit_calls, 2);
    teardown_fake(&fake);
}
