/* LS_TEST_SOURCES: ${APP}/fm/fm_mode_label.c */
#include "ls_test.h"
#include "fm_mode_label.h"

#include <stdio.h>

LS_CASE(every_fm_mode_has_the_correct_label)
{
    static const char *const expected[] = {
        "NFM", "SWEEP", "POCSAG", "WFM", "ACARS", "FLEX", "UNKNOWN", "AM", "SAME", "APRS", "AIS"
    };
    const int expected_count = (int)(sizeof(expected) / sizeof(expected[0]));

    /* A new enum value must extend this explicit contract instead of silently
       inheriting a shifted or null VFO label. */
    LS_EQ_INT(FM_MODE_COUNT, expected_count);
    for (int mode = 0; mode < FM_MODE_COUNT; ++mode)
        LS_EQ_STR(expected[mode], fm_mode_label((fm_mode_t)mode));
}

LS_CASE(every_fm_mode_has_a_round_trip_console_name)
{
    static const char *const expected[] = {
        "listen", "scan", "pocsag", "wfm", "acars", "flex", "unknown", "am", "same", "aprs", "ais"
    };
    const int expected_count = (int)(sizeof(expected) / sizeof(expected[0]));

    LS_EQ_INT(FM_MODE_COUNT, expected_count);
    for (int mode = 0; mode < FM_MODE_COUNT; ++mode) {
        fm_mode_t parsed = FM_MODE_COUNT;
        LS_EQ_STR(expected[mode], fm_mode_command_name((fm_mode_t)mode));
        if (mode == 6) {
            LS_CHECK(!fm_mode_parse(expected[mode], &parsed));
            LS_EQ_INT(FM_MODE_COUNT, parsed);
            continue;
        }
        LS_CHECK(fm_mode_parse(expected[mode], &parsed));
        LS_EQ_INT(mode, parsed);
    }

    fm_mode_t parsed = FM_MODE_COUNT;
    LS_CHECK(fm_mode_parse("ACARS", &parsed));
    LS_EQ_INT(FM_MODE_ACARS, parsed);
    LS_CHECK(fm_mode_parse("FLEX", &parsed));
    LS_EQ_INT(FM_MODE_FLEX, parsed);
    LS_CHECK(!fm_mode_parse("TRAP", &parsed));
    LS_EQ_INT(FM_MODE_FLEX, parsed);
}

LS_CASE(console_alias_numeric_and_invalid_inputs_are_bounded)
{
    fm_mode_t parsed = FM_MODE_FLEX;
    LS_CHECK(fm_mode_parse("nbfm", &parsed));
    LS_EQ_INT(FM_MODE_LISTEN, parsed);

    for (int mode = 0; mode < FM_MODE_COUNT; ++mode) {
        char numeric[4];
        snprintf(numeric, sizeof(numeric), "%d", mode);
        parsed = FM_MODE_COUNT;
        if (mode == 6) {
            LS_CHECK(!fm_mode_parse(numeric, &parsed));
            LS_EQ_INT(FM_MODE_COUNT, parsed);
            continue;
        }
        LS_CHECK(fm_mode_parse(numeric, &parsed));
        LS_EQ_INT(mode, parsed);
    }

    parsed = FM_MODE_FLEX;
    LS_CHECK(!fm_mode_parse("bogus", &parsed));
    LS_EQ_INT(FM_MODE_FLEX, parsed);
    LS_CHECK(!fm_mode_parse("-1", &parsed));
    LS_EQ_INT(FM_MODE_FLEX, parsed);
    LS_CHECK(!fm_mode_parse("11", &parsed));
    LS_EQ_INT(FM_MODE_FLEX, parsed);
    LS_CHECK(!fm_mode_parse("999999999999999999999", &parsed));
    LS_EQ_INT(FM_MODE_FLEX, parsed);
    LS_CHECK(!fm_mode_parse(NULL, &parsed));
    LS_CHECK(!fm_mode_parse("", &parsed));
    LS_CHECK(!fm_mode_parse("listen", NULL));
}

LS_CASE(invalid_fm_modes_are_bounded)
{
    LS_EQ_STR("UNKNOWN", fm_mode_label((fm_mode_t)-1));
    LS_EQ_STR("UNKNOWN", fm_mode_label(FM_MODE_COUNT));
    LS_EQ_STR("UNKNOWN", fm_mode_label((fm_mode_t)(FM_MODE_COUNT + 10)));
    LS_EQ_STR("unknown", fm_mode_command_name((fm_mode_t)-1));
    LS_EQ_STR("unknown", fm_mode_command_name(FM_MODE_COUNT));
    LS_EQ_STR("unknown", fm_mode_command_name((fm_mode_t)(FM_MODE_COUNT + 10)));
}

LS_CASE(same_preserves_reserved_id_and_surviving_modes)
{
    LS_EQ_INT(FM_MODE_LISTEN, 0);
    LS_EQ_INT(FM_MODE_SCAN, 1);
    LS_EQ_INT(FM_MODE_POCSAG, 2);
    LS_EQ_INT(FM_MODE_WFM, 3);
    LS_EQ_INT(FM_MODE_ACARS, 4);
    LS_EQ_INT(FM_MODE_FLEX, 5);
    fm_mode_t parsed = FM_MODE_LISTEN;
    static const char *const removed[] = {"trap", "TRAP", "TargetTag"};
    for (unsigned i = 0; i < sizeof(removed) / sizeof(removed[0]); ++i) {
        LS_CHECK(!fm_mode_parse(removed[i], &parsed));
        LS_EQ_INT(parsed, FM_MODE_LISTEN);
    }
    LS_EQ_INT(FM_MODE_AM, 7);
    LS_EQ_INT(FM_MODE_SAME, 8);
    LS_EQ_INT(FM_MODE_APRS, 9);
    LS_EQ_INT(FM_MODE_AIS, 10);
    LS_EQ_INT(FM_MODE_COUNT, 11);
    LS_CHECK(!fm_mode_parse("6", &parsed));
    LS_EQ_INT(FM_MODE_LISTEN, parsed);
    LS_EQ_STR("UNKNOWN", fm_mode_label((fm_mode_t)6));
    LS_EQ_STR("unknown", fm_mode_command_name((fm_mode_t)6));
    LS_CHECK(fm_mode_parse("SAME", &parsed));
    LS_EQ_INT(FM_MODE_SAME, parsed);
}
