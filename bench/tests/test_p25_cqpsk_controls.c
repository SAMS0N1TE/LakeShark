/* LS_TEST_SOURCES: ${APP}/p25/p25_cqpsk_controls.c ${APP}/p25/dsp_pipeline.c */
/* validation, persistence units, owner latch, and live DSP wiring. */
#include "ls_test.h"

#include "dsp_pipeline.h"
#include "p25_cqpsk_controls.h"

#include <math.h>
#include <stdint.h>
#include <stdarg.h>

void sys_log(unsigned char color, const char *fmt, ...)
{
    (void)color;
    (void)fmt;
}

LS_CASE(tested_defaults_are_the_recovered679_coefficients)
{
    p25_cqpsk_config_t config;
    p25_cqpsk_config_defaults(&config);
    LS_CHECK(p25_cqpsk_config_valid(&config));
    LS_NEAR(config.timing_gain, 0.00015625f, 0.0f);
    LS_NEAR(config.carrier_gain, 0.01f, 0.0f);

    dsp_state_t dsp;
    dsp_init(&dsp);
    LS_NEAR(dsp.g_gain_omega, config.timing_gain, 0.0f);
    LS_NEAR(dsp.cqpsk_afc_alpha, config.carrier_gain, 0.0f);
}

LS_CASE(non_finite_overflowing_and_unsafe_values_are_rejected)
{
    p25_cqpsk_config_t config;
    p25_cqpsk_config_defaults(&config);
    config.timing_gain = NAN;
    LS_CHECK(!p25_cqpsk_config_valid(&config));
    config.timing_gain = INFINITY;
    LS_CHECK(!p25_cqpsk_config_valid(&config));
    config.timing_gain = P25_CQPSK_TIMING_GAIN_MIN * 0.5f;
    LS_CHECK(!p25_cqpsk_config_valid(&config));
    config.timing_gain = P25_CQPSK_TIMING_GAIN_MAX * 2.0f;
    LS_CHECK(!p25_cqpsk_config_valid(&config));

    float parsed = 123.0f;
    LS_CHECK(!p25_cqpsk_gain_parse("nan", P25_CQPSK_CARRIER_GAIN_MIN,
                                   P25_CQPSK_CARRIER_GAIN_MAX, &parsed));
    LS_CHECK(!p25_cqpsk_gain_parse("inf", P25_CQPSK_CARRIER_GAIN_MIN,
                                   P25_CQPSK_CARRIER_GAIN_MAX, &parsed));
    LS_CHECK(!p25_cqpsk_gain_parse("1e9999", P25_CQPSK_CARRIER_GAIN_MIN,
                                   P25_CQPSK_CARRIER_GAIN_MAX, &parsed));
    LS_CHECK(!p25_cqpsk_gain_parse("0.01junk", P25_CQPSK_CARRIER_GAIN_MIN,
                                   P25_CQPSK_CARRIER_GAIN_MAX, &parsed));
    LS_NEAR(parsed, 123.0f, 0.0f);
}

LS_CASE(persistence_units_round_trip_every_tested_ladder_value)
{
    static const float values[] = {
        0.0000390625f, 0.000078125f, 0.00015625f, 0.0003125f,
        0.000625f, 0.0025f, 0.005f, 0.01f, 0.02f, 0.04f,
    };
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        float minimum = values[i] < 0.001f ? P25_CQPSK_TIMING_GAIN_MIN
                                            : P25_CQPSK_CARRIER_GAIN_MIN;
        float maximum = values[i] < 0.001f ? P25_CQPSK_TIMING_GAIN_MAX
                                            : P25_CQPSK_CARRIER_GAIN_MAX;
        uint32_t raw = 0;
        float decoded = 0.0f;
        LS_CHECK(p25_cqpsk_gain_encode(values[i], minimum, maximum, &raw));
        LS_CHECK(p25_cqpsk_gain_decode(raw, minimum, maximum, &decoded));
        LS_NEAR(decoded, values[i], 0.000000001f);
    }
    float decoded = 0.0f;
    LS_CHECK(!p25_cqpsk_gain_decode(UINT32_MAX,
                                    P25_CQPSK_CARRIER_GAIN_MIN,
                                    P25_CQPSK_CARRIER_GAIN_MAX, &decoded));

    p25_cqpsk_config_t saved = { 0.0003125f, 0.02f };
    p25_cqpsk_config_t loaded = { 0.0f, 0.0f };
    uint64_t packed = 0;
    LS_CHECK(p25_cqpsk_config_pack(&saved, &packed));
    LS_CHECK(p25_cqpsk_config_unpack(packed, &loaded));
    LS_NEAR(loaded.timing_gain, saved.timing_gain, 0.0f);
    LS_NEAR(loaded.carrier_gain, saved.carrier_gain, 0.0f);
    LS_CHECK(!p25_cqpsk_config_unpack(UINT64_MAX, &loaded));
}

LS_CASE(latest_command_is_applied_only_when_the_owner_takes_it)
{
    p25_cqpsk_config_t tested;
    p25_cqpsk_config_defaults(&tested);
    p25_cqpsk_control_t control;
    p25_cqpsk_control_init(&control, &tested);

    p25_cqpsk_config_t first = { 0.0003125f, 0.02f };
    p25_cqpsk_config_t last = { 0.000625f, 0.04f };
    LS_CHECK(p25_cqpsk_control_request(&control, &first));
    LS_CHECK(p25_cqpsk_control_request(&control, &last));

    p25_cqpsk_config_t requested, effective;
    bool pending = false;
    p25_cqpsk_control_status(&control, &requested, &effective, &pending);
    LS_CHECK(pending);
    LS_NEAR(requested.timing_gain, last.timing_gain, 0.0f);
    LS_NEAR(effective.timing_gain, tested.timing_gain, 0.0f);

    uint32_t generation = 0;
    p25_cqpsk_config_t taken;
    LS_CHECK(p25_cqpsk_control_take(&control, &taken, &generation));
    LS_EQ_UINT(generation, 2);
    LS_NEAR(taken.carrier_gain, last.carrier_gain, 0.0f);
    LS_CHECK(!p25_cqpsk_control_take(&control, &taken, NULL));
    p25_cqpsk_control_applied(&control, &taken, generation);
    p25_cqpsk_control_status(&control, NULL, &effective, &pending);
    LS_CHECK(!pending);
    LS_NEAR(effective.timing_gain, last.timing_gain, 0.0f);
}

LS_CASE(dsp_apply_changes_used_coefficients_and_clears_loop_history)
{
    dsp_state_t dsp;
    dsp_init(&dsp);
    dsp.g_clock = 7.0f;
    dsp.g_period = 9.7f;
    dsp.g_di[1] = 4.0f;
    dsp.g_dq[2] = -3.0f;
    dsp.cqpsk_afc_phase_err = 0.3f;
    dsp.diff_prev_i = 0.2f;
    dsp.diff_prev_q = 0.7f;
    dsp.cqpsk_polarity = 5;

    p25_cqpsk_config_t config = { 0.0003125f, 0.02f };
    LS_CHECK(dsp_set_cqpsk_loops(&dsp, &config));
    LS_NEAR(dsp.g_gain_omega, config.timing_gain, 0.0f);
    LS_NEAR(dsp.cqpsk_afc_alpha, config.carrier_gain, 0.0f);
    LS_NEAR(dsp.g_clock, 0.0f, 0.0f);
    LS_NEAR(dsp.g_period, (float)DSP_SPS, 0.0f);
    LS_NEAR(dsp.g_di[1], 0.0f, 0.0f);
    LS_NEAR(dsp.g_dq[2], 0.0f, 0.0f);
    LS_NEAR(dsp.cqpsk_afc_phase_err, 0.0f, 0.0f);
    LS_NEAR(dsp.diff_prev_i, 1.0f, 0.0f);
    LS_NEAR(dsp.diff_prev_q, 0.0f, 0.0f);
    LS_EQ_INT(dsp.cqpsk_polarity, 0);

    p25_cqpsk_config_t unsafe = { config.timing_gain, NAN };
    LS_CHECK(!dsp_set_cqpsk_loops(&dsp, &unsafe));
    LS_NEAR(dsp.g_gain_omega, config.timing_gain, 0.0f);
    LS_NEAR(dsp.cqpsk_afc_alpha, config.carrier_gain, 0.0f);
}

LS_CASE(cqpsk_mode_transition_reacquires_without_losing_effective_gains)
{
    dsp_state_t dsp;
    dsp_init(&dsp);
    p25_cqpsk_config_t config = { 0.0003125f, 0.02f };
    LS_CHECK(dsp_set_cqpsk_loops(&dsp, &config));
    dsp.g_period = 10.03f;
    dsp.cqpsk_afc_phase_err = 0.12f;
    dsp_set_mode(&dsp, DEMOD_C4FM);
    dsp_set_mode(&dsp, DEMOD_CQPSK);
    LS_NEAR(dsp.g_period, (float)DSP_SPS, 0.0f);
    LS_NEAR(dsp.cqpsk_afc_phase_err, 0.0f, 0.0f);
    LS_NEAR(dsp.g_gain_omega, config.timing_gain, 0.0f);
    LS_NEAR(dsp.cqpsk_afc_alpha, config.carrier_gain, 0.0f);
}
