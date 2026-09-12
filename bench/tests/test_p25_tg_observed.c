/* LS_TEST_SOURCES: ${APP}/p25/p25_tg_observed.c ${APP}/p25/p25_lcw.c */
#include "ls_test.h"
#include "p25_tg_observed_frame.h"
#include <string.h>
static dsd_state state;
static p25_tg_observed_snapshot_t view;
LS_CASE(real_lcw_fec_rejection_and_fresh_publication)
{
    memset(&state, 0, sizeof(state));
    state.p25_frame_valid = 1; state.p25_frame_duid = 5; state.nac = 0x123;
    uint8_t payload[P25_LCW_LCINFO_BYTES] = {0, 0, 0x04, 0xd2, 0, 0, 7};
    LS_CHECK(p25_tg_observed_clear());
    LS_EQ_INT(p25_lcw_dispatch(&state, 0, 0, payload, 0, 1000), 0);
    p25_tg_observed_frame(&state, 0, 155000000, 1);
    LS_CHECK(p25_tg_observed_read(&view)); LS_EQ_INT(view.count, 0);
    LS_EQ_INT(p25_lcw_dispatch(&state, 0, 0, payload, 1, 2000), 1);
    p25_tg_observed_frame(&state, 0, 155000000, 2);
    LS_CHECK(p25_tg_observed_read(&view)); LS_EQ_INT(view.count, 1);
    LS_EQ_INT(view.rows[0].talkgroup, 1234);
    uint32_t prior = state.p25_lcw_ok_count;
    LS_EQ_INT(p25_lcw_dispatch(&state, 0, 0, payload, 0, 3000), 0);
    p25_tg_observed_frame(&state, prior, 155000000, 3);
    LS_CHECK(p25_tg_observed_read(&view)); LS_EQ_UINT(view.rows[0].hits, 1);
}
LS_CASE(duplicates_and_channel_scope)
{
    LS_CHECK(p25_tg_observed_clear());
    LS_CHECK(p25_tg_observed_record(155000000, 0x123, 7, P25_TG_SEEN_LCW, 10));
    LS_CHECK(p25_tg_observed_record(155000000, 0x123, 7, P25_TG_SEEN_GRANT, 20));
    LS_CHECK(p25_tg_observed_record(156000000, 0x123, 7, P25_TG_SEEN_LCW, 30));
    LS_CHECK(p25_tg_observed_record(155000000, 0x124, 7, P25_TG_SEEN_LCW, 40));
    LS_CHECK(p25_tg_observed_read(&view));
    LS_EQ_INT(view.count, 3);
    LS_EQ_UINT(view.rows[0].hits, 2);
    LS_EQ_UINT(view.rows[0].last_seen_ms, 20);
    LS_EQ_UINT(view.rows[0].sources, 5);
}
LS_CASE(invalid_and_bounded_eviction)
{
    LS_CHECK(p25_tg_observed_clear());
    LS_CHECK(!p25_tg_observed_record(0, 1, 2, 1, 0));
    LS_CHECK(!p25_tg_observed_record(155000000, 4096, 2, 1, 0));
    LS_CHECK(!p25_tg_observed_record(155000000, 1, 0, 1, 0));
    LS_CHECK(!p25_tg_observed_record(155000000, 1, 65535, 1, 0));
    LS_CHECK(!p25_tg_observed_record(155000000, 1, 2, 8, 0));
    for (unsigned i = 1; i <= P25_TG_OBSERVED_MAX + 1; ++i)
        LS_CHECK(p25_tg_observed_record(155000000, 1, i, 1, i));
    LS_CHECK(p25_tg_observed_read(&view));
    LS_EQ_INT(view.count, P25_TG_OBSERVED_MAX);
    LS_EQ_INT(view.rows[0].talkgroup, P25_TG_OBSERVED_MAX + 1);
}
LS_CASE(validated_frame_only_not_raw_lasttg)
{
    LS_CHECK(p25_tg_observed_clear());
    memset(&state, 0, sizeof(state));
    state.nac = 0x123; state.lasttg = 88;
    p25_tg_observed_frame(&state, 0, 155000000, 1);
    state.p25_frame_valid = 1; state.p25_frame_duid = 5;
    state.p25_lcw_valid = 1; state.p25_lcw_talkgroup = 42;
    p25_tg_observed_frame(&state, 0, 155000000, 2); /* stale LC */
    LS_CHECK(p25_tg_observed_read(&view)); LS_EQ_INT(view.count, 0);
    state.p25_lcw_ok_count = 1;
    state.p25_lcw_is_unit_to_unit = 1;
    p25_tg_observed_frame(&state, 0, 155000000, 3);
    LS_CHECK(p25_tg_observed_read(&view)); LS_EQ_INT(view.count, 0);
    state.p25_lcw_is_unit_to_unit = 0;
    p25_tg_observed_frame(&state, 0, 155000000, 4);
    LS_CHECK(p25_tg_observed_read(&view)); LS_EQ_INT(view.rows[0].talkgroup, 42);
    state.p25_frame_duid = 0; state.p25_ess_valid = 0;
    p25_tg_observed_frame(&state, 1, 155000000, 5); /* rejected HDU */
    LS_CHECK(p25_tg_observed_read(&view)); LS_EQ_INT(view.count, 1);
    state.p25_ess_valid = 1;
    p25_tg_observed_frame(&state, 1, 155000000, 6);
    LS_CHECK(p25_tg_observed_read(&view)); LS_EQ_INT(view.rows[1].talkgroup, 88);
    state.p25_frame_duid = 7; state.p25_grant_count = 1;
    state.p25_grants[0].talkgroup = 91;
    p25_tg_observed_frame(&state, 1, 155000000, 7);
    LS_CHECK(p25_tg_observed_read(&view)); LS_EQ_INT(view.count, 2);
    state.p25_grant_batch_valid = 1;
    p25_tg_observed_frame(&state, 1, 155000000, 8);
    LS_CHECK(p25_tg_observed_read(&view)); LS_EQ_INT(view.rows[2].talkgroup, 91);
}
