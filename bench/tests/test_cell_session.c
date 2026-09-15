#include "ls_test.h"
#include "cell_session.h"
LS_CASE(repeated_session_keeps_attempts_and_rejects_failed_decode)
{
    cell_session_t s; cell_session_begin(&s, true);
    for (unsigned i=0;i<13;i++) cell_session_finish(&s,true,true,1280000);
    LS_CHECK(s.attempts==13 && s.decoded==13 && s.channel==1);
    cell_session_finish(&s,false,true,0);
    LS_CHECK(s.decoded==13 && s.wait_ms==2000);
}
LS_CASE(retries_back_off_and_recover_without_stalling)
{
    cell_session_t s; cell_session_begin(&s,false);
    for (int i=0;i<100;i++) cell_session_finish(&s,false,false,0);
    LS_CHECK(s.wait_ms==30000 && s.channel==0);
    cell_session_finish(&s,true,false,0);
    LS_CHECK(s.wait_ms==1000 && s.failures==0 && s.decoded==0);
}
LS_CASE(raw_budget_has_exact_bounds_and_metadata_can_continue)
{
    cell_session_t s; cell_session_begin(&s,false);
    s.raw_bytes=CELL_SESSION_RAW_LIMIT-100;
    LS_CHECK(cell_session_keep_raw(&s,100));
    LS_CHECK(!cell_session_keep_raw(&s,101));
    cell_session_finish(&s,true,true,100);
    LS_CHECK(!cell_session_keep_raw(&s,1));
    cell_session_finish(&s,true,true,0);
    LS_CHECK(s.running && s.decoded==2);
}
LS_CASE(new_session_resets_counters)
{
    cell_session_t s; cell_session_begin(&s,true);
    cell_session_finish(&s,false,false,128);
    cell_session_begin(&s,false);
    LS_CHECK(s.running && !s.multi && !s.attempts && !s.raw_bytes);
}

