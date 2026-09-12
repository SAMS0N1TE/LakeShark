/* LS_TEST_SOURCES: ${APP}/p25/p25_profile.c */
#include "ls_test.h"

#include "p25_profile.h"
#include "p25_profile_fixture.h"

#include <stdio.h>
#include <string.h>

static const ls_radio_range_t s_tune_ranges[] = {
    {24000000, 1766000000},
};
static const p25_profile_parse_config_t s_config = {
    s_tune_ranges, sizeof(s_tune_ranges) / sizeof(s_tune_ranges[0]),
};

static bool parse_text(p25_profile_t *profile,
                       p25_profile_parse_scratch_t *scratch,
                       const char *text, p25_profile_diagnostic_t *diagnostic)
{
    return p25_profile_parse(profile, scratch, text, strlen(text),
                             &s_config, diagnostic);
}

LS_CASE(shipped_example_parses_and_matches_version_1_defaults)
{
    static char input[P25_PROFILE_MAX_FILE_BYTES + 1U];
    FILE *file = fopen(P25_PROFILE_EXAMPLE_PATH, "rb");
    LS_CHECK_MSG(file != NULL, "cannot open shipped profile: %s",
                 P25_PROFILE_EXAMPLE_PATH);
    if (!file) return;

    size_t input_len = fread(input, 1, sizeof(input), file);
    bool read_ok = !ferror(file);
    LS_CHECK_MSG(read_ok, "cannot read shipped profile: %s",
                 P25_PROFILE_EXAMPLE_PATH);
    fclose(file);
    if (!read_ok) return;

    p25_profile_t profile;
    p25_profile_parse_scratch_t scratch;
    p25_profile_diagnostic_t diagnostic;
    bool parsed = p25_profile_parse(&profile, &scratch, input, input_len,
                                    &s_config, &diagnostic);
    LS_CHECK_MSG(parsed,
                 "shipped profile rejected at line %lu: %s",
                 (unsigned long)diagnostic.line, diagnostic.reason);
    if (!parsed) return;

    LS_EQ_UINT(profile.format_version, P25_PROFILE_FORMAT_VERSION);
    LS_EQ_UINT(profile.preferred_control_hz, profile.control_channels[0]);
    LS_CHECK(profile.auto_follow);
    LS_CHECK(profile.encrypted_skip_enabled);
    LS_EQ_UINT(profile.encrypted_skip_ms, P25_GRANT_DEFAULT_ENCRYPTED_SKIP_MS);
    LS_EQ_INT(profile.demod_preference, P25_DEMOD_AUTO);
    LS_NEAR(profile.cqpsk.timing_gain, P25_CQPSK_TIMING_GAIN_DEFAULT, 0.0f);
    LS_NEAR(profile.cqpsk.carrier_gain, P25_CQPSK_CARRIER_GAIN_DEFAULT, 0.0f);
}

LS_CASE(valid_minimal_profile_uses_policy_defaults)
{
    p25_profile_t profile;
    p25_profile_parse_scratch_t scratch;
    p25_profile_diagnostic_t diagnostic;
    LS_CHECK(parse_text(&profile, &scratch, P25_PROFILE_MINIMAL_FIXTURE,
                        &diagnostic));
    LS_EQ_UINT(profile.format_version, P25_PROFILE_FORMAT_VERSION);
    LS_EQ_STR(profile.system_name, "Example System");
    LS_EQ_STR(profile.site_name, "Example Site");
    LS_EQ_UINT(profile.control_count, 1);
    LS_EQ_UINT(profile.control_channels[0], 851012500);
    LS_EQ_UINT(profile.preferred_control_hz, 851012500);
    LS_CHECK(profile.auto_follow);
    LS_CHECK(profile.encrypted_skip_enabled);
    LS_EQ_UINT(profile.encrypted_skip_ms, P25_GRANT_DEFAULT_ENCRYPTED_SKIP_MS);
    LS_EQ_INT(profile.demod_preference, P25_DEMOD_AUTO);
    LS_NEAR(profile.cqpsk.timing_gain, P25_CQPSK_TIMING_GAIN_DEFAULT, 0.0f);
    LS_NEAR(profile.cqpsk.carrier_gain, P25_CQPSK_CARRIER_GAIN_DEFAULT, 0.0f);
    LS_EQ_UINT(profile.talkgroup_count, 0);
    LS_EQ_INT(diagnostic.code, P25_PROFILE_ERROR_NONE);
    LS_EQ_STR(diagnostic.reason, "ok");
}

LS_CASE(valid_full_profile_accepts_comments_crlf_and_final_line_without_lf)
{
    p25_profile_t profile;
    p25_profile_parse_scratch_t scratch;
    p25_profile_diagnostic_t diagnostic;
    LS_CHECK(parse_text(&profile, &scratch, P25_PROFILE_FULL_FIXTURE,
                        &diagnostic));
    LS_EQ_UINT(profile.control_count, 2);
    LS_EQ_UINT(profile.preferred_control_hz, 852237500);
    LS_CHECK(!profile.auto_follow);
    LS_CHECK(!profile.encrypted_skip_enabled);
    LS_EQ_UINT(profile.encrypted_skip_ms, 45000);
    LS_EQ_INT(profile.demod_preference, DEMOD_CQPSK);
    LS_NEAR(profile.cqpsk.timing_gain, 0.0003125f, 0.0f);
    LS_NEAR(profile.cqpsk.carrier_gain, 0.02f, 0.0f);
    LS_EQ_UINT(profile.talkgroup_count, 2);
    LS_EQ_UINT(profile.talkgroups[0].id, 1201);
    LS_EQ_STR(profile.talkgroups[0].alias, "Operations");
    LS_CHECK(profile.talkgroups[0].enabled);
    LS_EQ_UINT(profile.talkgroups[0].priority, 7);
    LS_CHECK(!profile.talkgroups[1].enabled);
}

static void expect_error(const char *text, p25_profile_error_t error,
                         size_t line)
{
    p25_profile_t profile;
    p25_profile_parse_scratch_t scratch;
    p25_profile_diagnostic_t diagnostic;
    memset(&profile, 0xa5, sizeof(profile));
    p25_profile_t before = profile;
    LS_CHECK(!parse_text(&profile, &scratch, text, &diagnostic));
    LS_EQ_INT(diagnostic.code, error);
    LS_EQ_UINT(diagnostic.line, line);
    LS_EQ_STR(diagnostic.reason, p25_profile_error_reason(error));
    LS_CHECK(memcmp(&profile, &before, sizeof(profile)) == 0);
}

LS_CASE(duplicates_are_rejected_instead_of_silently_overriding)
{
    expect_error("version=1\nversion=1\nsystem=S\nsite=X\ncontrol=851000000\n",
                 P25_PROFILE_ERROR_DUPLICATE_FIELD, 2);
    expect_error("version=1\nsystem=S\nsite=X\ncontrol=851000000\ncontrol=851000000\n",
                 P25_PROFILE_ERROR_DUPLICATE_CONTROL, 5);
    expect_error("version=1\nsystem=S\nsite=X\ncontrol=851000000\n"
                 "tg=10|One|true|0\ntg=10|Two|true|0\n",
                 P25_PROFILE_ERROR_DUPLICATE_TALKGROUP, 6);
}

LS_CASE(unknown_fields_and_versions_are_rejected)
{
    expect_error("version=2\nsystem=S\nsite=X\ncontrol=851000000\n",
                 P25_PROFILE_ERROR_UNSUPPORTED_VERSION, 1);
    expect_error("version=1\nsystem=S\nsite=X\ncolour=blue\ncontrol=851000000\n",
                 P25_PROFILE_ERROR_UNKNOWN_FIELD, 4);
    expect_error("version=1\nsystem=S\nsite=X\ncontrol=851000000\ndemod=magic\n",
                 P25_PROFILE_ERROR_INVALID_DEMOD, 5);
}

LS_CASE(numeric_overflow_is_reported_without_touching_destination)
{
    expect_error("version=18446744073709551616\nsystem=S\nsite=X\ncontrol=851000000\n",
                 P25_PROFILE_ERROR_NUMBER_OVERFLOW, 1);
    expect_error("version=1\nsystem=S\nsite=X\ncontrol=851000000\n"
                 "encrypted_skip_ms=4294967296\n",
                 P25_PROFILE_ERROR_NUMBER_OVERFLOW, 5);
    expect_error("version=1\nsystem=S\nsite=X\ncontrol=851000000\n"
                 "tg=12|Ops|true|256\n",
                 P25_PROFILE_ERROR_NUMBER_OVERFLOW, 5);
}

LS_CASE(cqpsk_loop_fields_reject_non_finite_overflow_and_unsafe_values)
{
    expect_error("version=1\nsystem=S\nsite=X\ncontrol=851000000\n"
                 "cqpsk_timing_gain=nan\n",
                 P25_PROFILE_ERROR_UNSAFE_LOOP_GAIN, 5);
    expect_error("version=1\nsystem=S\nsite=X\ncontrol=851000000\n"
                 "cqpsk_carrier_gain=inf\n",
                 P25_PROFILE_ERROR_UNSAFE_LOOP_GAIN, 5);
    expect_error("version=1\nsystem=S\nsite=X\ncontrol=851000000\n"
                 "cqpsk_carrier_gain=1e9999\n",
                 P25_PROFILE_ERROR_UNSAFE_LOOP_GAIN, 5);
    expect_error("version=1\nsystem=S\nsite=X\ncontrol=851000000\n"
                 "cqpsk_timing_gain=0.000001\n",
                 P25_PROFILE_ERROR_UNSAFE_LOOP_GAIN, 5);
    expect_error("version=1\nsystem=S\nsite=X\ncontrol=851000000\n"
                 "cqpsk_carrier_gain=0.5\n",
                 P25_PROFILE_ERROR_UNSAFE_LOOP_GAIN, 5);
}

LS_CASE(malformed_frequency_and_talkgroup_rows_are_rejected)
{
    expect_error("version=1\nsystem=S\nsite=X\ncontrol=851.0125\n",
                 P25_PROFILE_ERROR_INVALID_NUMBER, 4);
    expect_error("version=1\nsystem=S\nsite=X\ncontrol=851000000\n"
                 "tg=12|Missing priority|true\n",
                 P25_PROFILE_ERROR_MALFORMED_TALKGROUP, 5);
    expect_error("version=1\nsystem=S\nsite=X\ncontrol=851000000\n"
                 "tg=0|Reserved|true|0\n",
                 P25_PROFILE_ERROR_MALFORMED_TALKGROUP, 5);
    expect_error("version=1\nsystem=S\nsite=X\ncontrol=851000000\n"
                 "tg=12|Ops|yes|0\n",
                 P25_PROFILE_ERROR_INVALID_BOOLEAN, 5);
}

LS_CASE(endpoint_ranges_not_a_p25_band_constant_control_frequency_validation)
{
    p25_profile_t profile;
    p25_profile_parse_scratch_t scratch;
    p25_profile_diagnostic_t diagnostic;
    const char low_band[] =
        "version=1\nsystem=S\nsite=Low band\ncontrol=460012500\n";
    LS_CHECK(parse_text(&profile, &scratch, low_band, &diagnostic));
    LS_EQ_UINT(profile.control_channels[0], 460012500);
    expect_error("version=1\nsystem=S\nsite=X\ncontrol=1766000001\n",
                 P25_PROFILE_ERROR_FREQUENCY_OUT_OF_RANGE, 4);
}

LS_CASE(preferred_control_must_name_an_entry)
{
    expect_error("version=1\nsystem=S\nsite=X\ncontrol=851000000\n"
                 "preferred=852000000\n",
                 P25_PROFILE_ERROR_PREFERRED_NOT_FOUND, 5);
}

LS_CASE(overlong_names_aliases_and_lines_are_rejected)
{
    expect_error("version=1\nsystem=12345678901234567890123456789012\n"
                 "site=X\ncontrol=851000000\n",
                 P25_PROFILE_ERROR_VALUE_TOO_LONG, 2);
    expect_error("version=1\nsystem=S\nsite=X\ncontrol=851000000\n"
                 "tg=12|123456789012345678901234|true|0\n",
                 P25_PROFILE_ERROR_VALUE_TOO_LONG, 5);

    char text[P25_PROFILE_MAX_LINE_LEN + 96];
    strcpy(text, "version=1\nsystem=");
    size_t used = strlen(text);
    memset(text + used, 'A', P25_PROFILE_MAX_LINE_LEN + 1);
    used += P25_PROFILE_MAX_LINE_LEN + 1;
    strcpy(text + used, "\nsite=X\ncontrol=851000000\n");
    expect_error(text, P25_PROFILE_ERROR_LINE_TOO_LONG, 2);
}

LS_CASE(control_and_talkgroup_capacities_are_hard_failures)
{
    static char text[12000];
    size_t used = (size_t)snprintf(text, sizeof(text),
        "version=1\nsystem=S\nsite=X\n");
    for (unsigned i = 0; i <= P25_PROFILE_CONTROL_MAX; ++i)
        used += (size_t)snprintf(text + used, sizeof(text) - used,
                                "control=%u\n", 100000000U + i);
    expect_error(text, P25_PROFILE_ERROR_CONTROL_CAPACITY,
                 4 + P25_PROFILE_CONTROL_MAX);

    used = (size_t)snprintf(text, sizeof(text),
        "version=1\nsystem=S\nsite=X\ncontrol=851000000\n");
    for (unsigned i = 0; i <= P25_PROFILE_TALKGROUP_MAX; ++i)
        used += (size_t)snprintf(text + used, sizeof(text) - used,
                                "tg=%u|TG %u|true|0\n", 1000U + i, i);
    expect_error(text, P25_PROFILE_ERROR_TALKGROUP_CAPACITY,
                 5 + P25_PROFILE_TALKGROUP_MAX);
}

LS_CASE(priority_entry_capacity_reuses_scan_controller_limit)
{
    static char text[4096];
    size_t used = (size_t)snprintf(text, sizeof(text),
        "version=1\nsystem=S\nsite=X\ncontrol=851000000\n");
    for (unsigned i = 0; i <= P25_PROFILE_PRIORITY_MAX; ++i)
        used += (size_t)snprintf(text + used, sizeof(text) - used,
                                "tg=%u|Priority %u|true|%u\n",
                                2000U + i, i, i + 1U);
    expect_error(text, P25_PROFILE_ERROR_PRIORITY_CAPACITY,
                 5 + P25_PROFILE_PRIORITY_MAX);
}

LS_CASE(missing_required_and_truncated_entries_report_the_failure_line)
{
    expect_error("version=1\nsystem=S\ncontrol=851000000\n",
                 P25_PROFILE_ERROR_MISSING_FIELD, 4);
    expect_error("version=1\nsystem=S\nsite=X\ncontrol=851000000\n"
                 "tg=22|Dispatch|true",
                 P25_PROFILE_ERROR_MALFORMED_TALKGROUP, 5);
}

LS_CASE(file_byte_limit_and_invalid_range_config_are_bounded)
{
    static char oversized[P25_PROFILE_MAX_FILE_BYTES + 1U];
    memset(oversized, '\n', sizeof(oversized));
    p25_profile_t profile;
    p25_profile_parse_scratch_t scratch;
    p25_profile_diagnostic_t diagnostic;
    memset(&profile, 0x3c, sizeof(profile));
    p25_profile_t before = profile;
    LS_CHECK(!p25_profile_parse(&profile, &scratch, oversized,
                                sizeof(oversized), &s_config, &diagnostic));
    LS_EQ_INT(diagnostic.code, P25_PROFILE_ERROR_INPUT_TOO_LONG);
    LS_EQ_UINT(diagnostic.line, 0);
    LS_CHECK(memcmp(&profile, &before, sizeof(profile)) == 0);

    const ls_radio_range_t bad_range = {900, 100};
    const p25_profile_parse_config_t bad_config = {&bad_range, 1};
    LS_CHECK(!p25_profile_parse(&profile, &scratch,
                                P25_PROFILE_MINIMAL_FIXTURE,
                                strlen(P25_PROFILE_MINIMAL_FIXTURE),
                                &bad_config, &diagnostic));
    LS_EQ_INT(diagnostic.code, P25_PROFILE_ERROR_ARGUMENT);
    LS_CHECK(memcmp(&profile, &before, sizeof(profile)) == 0);
}
