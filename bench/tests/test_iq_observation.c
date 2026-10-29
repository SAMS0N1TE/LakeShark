/* LS_TEST_SOURCES: ${FW}/components/lakeshark/radio/iq_observation.c */
#include "ls_test.h"
#include "iq_observation.h"
#include <limits.h>

LS_CASE(iq_observation_unknown_is_not_known_zero_or_elapsed_coverage)
{
    ls_iq_observation_t s = {0}; uint64_t duration = 123;
    LS_CHECK(!ls_iq_observation_nominal_us(&s, &duration));
    LS_CHECK(!ls_iq_observation_deliver(&s, 16384, 100));
    ls_iq_observation_begin(&s, 256000, 154785000);
    LS_CHECK(s.known && s.accepting);
    LS_CHECK(ls_iq_observation_nominal_us(&s, &duration));
    LS_CHECK(duration == 0 && s.delivered_bytes == 0);
    LS_CHECK(ls_iq_observation_deliver(&s, 16384, 9000000));
    LS_CHECK(ls_iq_observation_nominal_us(&s, &duration));
    LS_CHECK(duration == 32000 && s.delivered_blocks == 1);
    LS_CHECK(s.first_delivery_us == 9000000 && s.last_delivery_us == 9000000);
    LS_CHECK(ls_iq_observation_deliver(&s, 16384, 99000000));
    LS_CHECK(ls_iq_observation_nominal_us(&s, &duration));
    LS_CHECK(duration == 64000); /* Not the 90-second wall interval. */
}

LS_CASE(iq_observation_boundaries_preserve_last_segment_until_new_delivery)
{
    ls_iq_observation_t s = {0};
    ls_iq_observation_begin(&s, 256000, 154785000);
    LS_CHECK(!ls_iq_observation_deliver(&s, 3, 1));
    LS_CHECK(!ls_iq_observation_deliver(&s, 0, 1));
    LS_CHECK(ls_iq_observation_deliver(&s, 16384, 2));
    const ls_iq_observation_state_t boundaries[] = {LS_IQ_OBS_TUNE, LS_IQ_OBS_MODE,
        LS_IQ_OBS_READ_ERROR, LS_IQ_OBS_RECEIVER_ERROR, LS_IQ_OBS_STOPPED, LS_IQ_OBS_UNSUPPORTED};
    for (unsigned i = 0; i < sizeof(boundaries)/sizeof(boundaries[0]); i++) {
        ls_iq_observation_close(&s, boundaries[i]);
        LS_CHECK(!s.accepting && s.state == boundaries[i]);
        LS_CHECK(!ls_iq_observation_deliver(&s, 16384, 3));
        LS_CHECK(s.delivered_bytes == 16384);
        uint32_t generation = s.generation;
        ls_iq_observation_begin(&s, 125000, 433920000);
        LS_CHECK(s.generation == generation + 1 && s.delivered_bytes == 0);
        LS_CHECK(ls_iq_observation_deliver(&s, 16384, 4));
    }
    ls_iq_observation_begin(&s, 0, 0);
    LS_CHECK(!s.known && !s.accepting);
}

LS_CASE(iq_observation_integer_bounds_never_wrap_or_invent_duration)
{
    ls_iq_observation_t s = {0}; uint64_t duration;
    LS_CHECK(sizeof(s) <= 256);
    ls_iq_observation_begin(&s, 3, 0);
    LS_CHECK(ls_iq_observation_deliver(&s, 2, 0));
    LS_CHECK(ls_iq_observation_nominal_us(&s, &duration));
    LS_CHECK(duration == 333333);
    s.delivered_bytes = UINT64_MAX - 1;
    LS_CHECK(ls_iq_observation_deliver(&s, 2, 1));
    LS_CHECK(s.saturated && s.delivered_bytes == UINT64_MAX);
    LS_CHECK(!ls_iq_observation_nominal_us(&s, &duration));
    ls_iq_observation_begin(&s, 1, 0); s.delivered_bytes = UINT64_MAX - 1;
    LS_CHECK(!ls_iq_observation_nominal_us(&s, &duration));
    s.generation = UINT32_MAX;
    ls_iq_observation_begin(&s, 1, 0);
    LS_CHECK(s.generation == UINT32_MAX && s.saturated);
}
