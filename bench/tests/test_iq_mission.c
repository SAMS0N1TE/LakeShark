/* LS_TEST_SOURCES: ${FW}/components/lakeshark/radio/iq_mission.c ${FW}/components/lakeshark/radio/iq_observation.c */
#include "ls_test.h"
#include "iq_mission.h"
#include <string.h>
static ls_iq_mission_t mission;
static ls_iq_mission_entry_t entries[LS_IQ_MISSION_CAPACITY];
static ls_iq_mission_status_t status;
static ls_iq_observation_t observation;
static size_t count;
static void reset(void)
{
    memset(&mission, 0, sizeof(mission)); observation = (ls_iq_observation_t){0};
    ls_iq_observation_begin(&observation, 256000, 154785000);
}
static void snapshot(void)
{ LS_CHECK(ls_iq_mission_snapshot(&mission, &status, entries, LS_IQ_MISSION_CAPACITY, &count)); }
static void deliver(uint64_t at)
{
    LS_CHECK(ls_iq_observation_deliver(&observation, 16384, at));
    ls_iq_mission_observe(&mission, &observation, at);
}
LS_CASE(mission_start_midsegment_excludes_previous_samples_and_coalesces)
{
    reset(); deliver(10);
    LS_CHECK(ls_iq_mission_start(&mission, "field \"A\"", &observation, 20));
    for (uint64_t i = 0; i < 1000; ++i) deliver(30 + i);
    snapshot();
    LS_CHECK(count == 2 && status.active);
    LS_CHECK(status.delivered_bytes == 16384000 && status.delivered_blocks == 1000);
    LS_CHECK(entries[1].kind == LS_IQ_MISSION_SEGMENT);
    LS_CHECK(entries[1].at_us == 30 && entries[1].last_delivery_us == 1029);
    LS_CHECK(entries[1].sample_accounting_known && entries[1].delivered_blocks == 1000);
    ls_iq_mission_observe(&mission, &observation, 1030); snapshot();
    LS_CHECK(status.delivered_blocks == 1000 && count == 2);
}
LS_CASE(mission_preserves_empty_unknown_intervals_and_exact_transition_evidence)
{
    reset(); LS_CHECK(ls_iq_mission_start(&mission, "survey", &observation, 100));
    ls_iq_observation_close(&observation, LS_IQ_OBS_TUNE);
    ls_iq_mission_boundary(&mission, &observation, 200);
    ls_iq_observation_begin(&observation, 256000, 162550000);
    deliver(500000); deliver(510000);
    ls_iq_observation_close(&observation, LS_IQ_OBS_READ_ERROR);
    ls_iq_mission_boundary(&mission, &observation, 520000);
    LS_CHECK(ls_iq_mission_stop(&mission, 900000)); snapshot();
    LS_CHECK(count == 5 && !status.active && status.stopped_us == 900000);
    LS_CHECK(entries[1].kind == LS_IQ_MISSION_BOUNDARY && entries[1].state == LS_IQ_OBS_TUNE);
    LS_CHECK(!entries[1].sample_accounting_known && !entries[1].delivered_bytes);
    LS_CHECK(entries[2].generation == 2 && entries[2].delivered_blocks == 2);
    LS_CHECK(entries[2].configured_center_hz == 162550000);
    LS_CHECK(entries[3].state == LS_IQ_OBS_READ_ERROR && entries[3].at_us == 520000);
    ls_iq_observation_begin(&observation, 256000, 99000000); deliver(1000000); snapshot();
    LS_CHECK(count == 5 && status.delivered_blocks == 2);
}
LS_CASE(mission_overwrites_oldest_entries_without_losing_session_totals)
{
    reset(); LS_CHECK(ls_iq_mission_start(&mission, "bounded", &observation, 0));
    for (unsigned i = 0; i < 50; ++i) {
        deliver(10 + 2 * i);
        ls_iq_observation_close(&observation, LS_IQ_OBS_MODE);
        ls_iq_mission_boundary(&mission, &observation, 11 + 2 * i);
        ls_iq_observation_begin(&observation, 256000, 154785000);
    }
    LS_CHECK(ls_iq_mission_stop(&mission, 200)); snapshot();
    LS_CHECK(count == 32 && status.retained_entries == 32 && status.overwritten_entries == 70);
    LS_CHECK(status.delivered_blocks == 50 && entries[0].sequence == 71 && entries[31].sequence == 102);
    LS_CHECK(ls_iq_mission_snapshot(&mission, &status, entries, 2, &count));
    LS_CHECK(count == 2 && status.retained_entries == 32);
}
LS_CASE(mission_clock_reversal_and_saturation_do_not_become_durations)
{
    reset(); LS_CHECK(ls_iq_mission_start(&mission, "clock", &observation, 100));
    deliver(200); deliver(150); snapshot();
    LS_CHECK(status.clock_regressed && status.clock_regressions == 1 && entries[1].clock_regressed);
    observation.saturated = true; ls_iq_mission_observe(&mission, &observation, 250); snapshot();
    LS_CHECK(status.counter_saturated && entries[1].counter_saturated);
    LS_CHECK(!entries[1].sample_accounting_known);
    mission.sequence = UINT64_MAX;
    ls_iq_mission_boundary(&mission, &observation, 300); snapshot();
    LS_CHECK(status.counter_saturated && entries[count-1].sequence == UINT64_MAX);
}
LS_CASE(mission_name_lifecycle_and_absence_are_explicit)
{
    reset();
    LS_CHECK(!ls_iq_mission_snapshot(&mission, &status, entries, 32, &count));
    LS_CHECK(!status.active && count == 0);
    LS_CHECK(!ls_iq_mission_start(&mission, "", &observation, 0));
    LS_CHECK(!ls_iq_mission_start(&mission, "bad\nname", &observation, 0));
    LS_CHECK(!ls_iq_mission_start(&mission, "01234567890123456789012345678901", &observation, 0));
    LS_CHECK(ls_iq_mission_start(&mission, "0123456789012345678901234567890", &observation, 0));
    LS_CHECK(!ls_iq_mission_start(&mission, "active", &observation, 1));
    LS_CHECK(ls_iq_mission_stop(&mission, 2) && !ls_iq_mission_stop(&mission, 3));
    LS_CHECK(ls_iq_mission_start(&mission, "replacement", &observation, 4));
    snapshot(); LS_CHECK(status.mission_id == 2 && count == 1 && !strcmp(status.name, "replacement"));
    LS_CHECK(ls_iq_mission_stop(&mission, 5));
    mission.status.mission_id = UINT32_MAX;
    LS_CHECK(!ls_iq_mission_start(&mission, "exhausted", &observation, 6));
}
