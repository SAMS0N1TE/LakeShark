/* LS_TEST_SOURCES: ${FW}/components/apps/media_gui/ls_media_lifecycle.c */
#include "ls_test.h"
#include "ls_media_lifecycle.h"

#include <string.h>

/**/

/* Small ordering recorder.  Each call appends a character, so the whole
   transition sequence becomes a string a case can strcmp on. */
#define TRACE_MAX 32
static char s_trace[TRACE_MAX];
static int  s_trace_n;

static void trace_reset(void)
{
    s_trace_n = 0;
    s_trace[0] = '\0';
}

static void trace_put(char c)
{
    if (s_trace_n < TRACE_MAX - 1) {
        s_trace[s_trace_n++] = c;
        s_trace[s_trace_n]   = '\0';
    }
}

static void mock_park(void) { trace_put('P'); }
static void mock_stop(void) { trace_put('S'); }

static void wire_mocks(void)
{
    ls_media_lifecycle_reset();
    trace_reset();
    ls_media_lifecycle_hooks_t h = { mock_park, mock_stop };
    ls_media_lifecycle_configure(&h);
}

LS_CASE(enter_parks_the_radio)
{

    wire_mocks();
    ls_media_lifecycle_enter();
    LS_EQ_STR(s_trace, "P");
    LS_CHECK(ls_media_lifecycle_is_owner());
}

LS_CASE(leave_stops_the_player)
{
    wire_mocks();
    ls_media_lifecycle_enter();
    ls_media_lifecycle_leave();
    /* Park, then stop, then release - in that order.  Anything else means
       audio_player can still be decoding when the next app opens. */
    LS_EQ_STR(s_trace, "PS");
    LS_CHECK(!ls_media_lifecycle_is_owner());
}

LS_CASE(files_to_music_to_fm_transition_order)
{

    wire_mocks();

    /* Files -> Music (radio still running from earlier). */
    ls_media_lifecycle_enter();
    LS_EQ_STR(s_trace, "P");

    /* Music -> FM: leave() must run before FM's run() opens the pipe. */
    ls_media_lifecycle_leave();
    LS_EQ_STR(s_trace, "PS");
}

LS_CASE(background_then_resume_is_park_stop_park)
{
    /* Music -> Files (passive) triggers background(); coming back to Music
       resumes it.  Both directions must be observable through the hooks. */
    wire_mocks();

    ls_media_lifecycle_enter();
    ls_media_lifecycle_leave();   /* background() into Files */
    ls_media_lifecycle_enter();   /* resume() from Files */
    LS_EQ_STR(s_trace, "PSP");
    LS_CHECK(ls_media_lifecycle_is_owner());
}

LS_CASE(second_enter_is_idempotent)
{
    /* run() and resume() both call enter().  A double enter must not park
       twice - the assertion is on the number of hook calls, so a redundant
       app_park() slipping through is caught even though app_park() itself is
       idempotent on the device. */
    wire_mocks();
    ls_media_lifecycle_enter();
    ls_media_lifecycle_enter();
    LS_EQ_STR(s_trace, "P");
    LS_CHECK(ls_media_lifecycle_is_owner());
}

LS_CASE(leave_without_enter_is_a_no_op)
{
    /* close() may follow pause() with no re-enter.  The second leave must not
       call stop_audio again - the guarantee is that leave means "Music was
       the owner, now it is not", so a stray leave from a torn-down instance
       cannot barge in on whatever owns the codec next. */
    wire_mocks();
    ls_media_lifecycle_leave();
    LS_EQ_STR(s_trace, "");
    LS_CHECK(!ls_media_lifecycle_is_owner());
}

LS_CASE(pause_then_close_only_stops_once)
{
    /* The shell calls pause() when switching apps and close() only when
       closeAll() runs.  A tear-down that lands both must not double-stop -
       one leave releases ownership, the next has nothing to do. */
    wire_mocks();
    ls_media_lifecycle_enter();
    ls_media_lifecycle_leave();   /* pause() */
    ls_media_lifecycle_leave();   /* close() with nothing to release */
    LS_EQ_STR(s_trace, "PS");
}

LS_CASE(single_owner_across_many_transitions)
{
    /* Longer walk: run, background to Files, resume, pause into FM, close.
       The invariant is that the trace never has two of the same event in a
       row (would mean park-park or stop-stop, i.e. an unpaired call) and
       ends with the module NOT holding ownership.  Assertion is the strict
       alternating sequence, which is stronger than either half alone. */
    wire_mocks();
    ls_media_lifecycle_enter();   /* run() */
    ls_media_lifecycle_leave();   /* background() -> Files */
    ls_media_lifecycle_enter();   /* resume() */
    ls_media_lifecycle_leave();   /* pause() -> FM */
    ls_media_lifecycle_leave();   /* close() (later) */
    LS_EQ_STR(s_trace, "PSPS");
    LS_CHECK(!ls_media_lifecycle_is_owner());
}

LS_CASE(reset_forgets_hooks_and_ownership)
{
    /* A previous case may have left the module owning the codec.  reset()
       is the between-case hygiene the harness relies on - if a hook fires
       after reset(), a case can hijack the trace of the next. */
    wire_mocks();
    ls_media_lifecycle_enter();
    LS_CHECK(ls_media_lifecycle_is_owner());

    ls_media_lifecycle_reset();
    LS_CHECK(!ls_media_lifecycle_is_owner());

    trace_reset();
    ls_media_lifecycle_enter();   /* hooks are gone, must be silent */
    ls_media_lifecycle_leave();
    LS_EQ_STR(s_trace, "");
}
