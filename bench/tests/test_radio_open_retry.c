/* LS_TEST_SOURCES: ${FW}/components/lakeshark/radio/radio_open_retry.c */
#include "ls_test.h"
#include "radio_open_retry.h"
#include "radio_health.h"

/* FM's radio open, bounded for io. The generation argument is what
   ls_radio_endpoint_generation() read before the attempt. */

static void fail_io(radio_open_retry_t *r, unsigned times, uint32_t gen)
{
    for (unsigned i = 0; i < times; ++i)
        LS_EQ_INT(radio_open_retry_failed(r, LS_RADIO_ERR_IO, gen),
                  RADIO_OPEN_RETRY_AGAIN);
}

LS_CASE(io_failures_under_the_limit_keep_retrying)
{
    radio_open_retry_t r;
    radio_open_retry_reset(&r);
    for (unsigned i = 1; i < RADIO_OPEN_IO_ATTEMPTS; ++i) {
        LS_EQ_INT(radio_open_retry_failed(&r, LS_RADIO_ERR_IO, 7),
                  RADIO_OPEN_RETRY_AGAIN);
        LS_CHECK(radio_open_retry_may_try(&r, 7));
    }
}

LS_CASE(the_limiting_io_failure_holds_once_and_then_the_app_stays_quiet)
{
    /* The observed flood was one attempt and one log line every ~150 ms for
       as long as FM was left. Holding means no attempt at all. */
    radio_open_retry_t r;
    radio_open_retry_reset(&r);
    fail_io(&r, RADIO_OPEN_IO_ATTEMPTS - 1, 7);
    LS_EQ_INT(radio_open_retry_failed(&r, LS_RADIO_ERR_IO, 7),
              RADIO_OPEN_RETRY_HOLD);
    for (int i = 0; i < 1000; ++i)
        LS_CHECK(!radio_open_retry_may_try(&r, 7));
}

LS_CASE(an_attach_or_detach_lifts_the_hold_with_a_fresh_count)
{
    radio_open_retry_t r;
    radio_open_retry_reset(&r);
    fail_io(&r, RADIO_OPEN_IO_ATTEMPTS - 1, 7);
    LS_EQ_INT(radio_open_retry_failed(&r, LS_RADIO_ERR_IO, 7),
              RADIO_OPEN_RETRY_HOLD);

    /* The root-port reset unregisters and re-registers: two bumps. */
    LS_CHECK(radio_open_retry_may_try(&r, 9));
    fail_io(&r, RADIO_OPEN_IO_ATTEMPTS - 1, 9);
    LS_CHECK(radio_open_retry_may_try(&r, 9));
    LS_EQ_INT(radio_open_retry_failed(&r, LS_RADIO_ERR_IO, 9),
              RADIO_OPEN_RETRY_HOLD);
    LS_CHECK(!radio_open_retry_may_try(&r, 9));
    LS_CHECK(radio_open_retry_may_try(&r, 10));
}

LS_CASE(a_change_during_the_failing_attempt_is_not_taken_for_the_failure)
{
    /* Generation 7 was read before the attempt; the reset landed while it
       was failing, so by the time the hold is checked it is 9. */
    radio_open_retry_t r;
    radio_open_retry_reset(&r);
    fail_io(&r, RADIO_OPEN_IO_ATTEMPTS - 1, 7);
    LS_EQ_INT(radio_open_retry_failed(&r, LS_RADIO_ERR_IO, 7),
              RADIO_OPEN_RETRY_HOLD);
    LS_CHECK(radio_open_retry_may_try(&r, 9));
}

LS_CASE(a_failure_of_another_kind_restarts_the_count)
{
    radio_open_retry_t r;
    radio_open_retry_reset(&r);
    fail_io(&r, RADIO_OPEN_IO_ATTEMPTS - 1, 7);
    /* The dongle left: acquire finds nothing, which is quiet and not io. */
    LS_EQ_INT(radio_open_retry_failed(&r, LS_RADIO_ERR_UNAVAILABLE, 8),
              RADIO_OPEN_RETRY_AGAIN);
    fail_io(&r, RADIO_OPEN_IO_ATTEMPTS - 1, 9);
    LS_CHECK(radio_open_retry_may_try(&r, 9));
}

LS_CASE(no_memory_is_never_held_here)
{
    /* P25 bounds its own no-memory case; this bound is io only. */
    radio_open_retry_t r;
    radio_open_retry_reset(&r);
    for (int i = 0; i < 50; ++i)
        LS_EQ_INT(radio_open_retry_failed(&r, LS_RADIO_ERR_NO_MEMORY, 7),
                  RADIO_OPEN_RETRY_AGAIN);
    LS_CHECK(radio_open_retry_may_try(&r, 7));
}

LS_CASE(a_successful_open_clears_the_count)
{
    radio_open_retry_t r;
    radio_open_retry_reset(&r);
    fail_io(&r, RADIO_OPEN_IO_ATTEMPTS - 1, 7);
    radio_open_retry_succeeded(&r);
    fail_io(&r, RADIO_OPEN_IO_ATTEMPTS - 1, 7);
    LS_CHECK(radio_open_retry_may_try(&r, 7));
}

LS_CASE(the_hold_never_comes_before_the_watchdog_can_act)
{
    /* Each failed open is at least one control io error on the endpoint.
       Holding sooner than the watchdog's threshold would leave it nothing
       to count, and the wedged dongle would sit there with no reset. */
    LS_CHECK(RADIO_OPEN_IO_ATTEMPTS >= RADIO_HEALTH_CONTROL_IO_FAULTS);
}
