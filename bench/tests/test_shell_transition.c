/* LS_TEST_SOURCES: ${FW}/components/apps/shell/ls_transition.c */
#include "ls_test.h"
#include "ls_transition.h"
#include <string.h>

LS_CASE(app_worker_and_radio_must_both_stop)
{
    LS_EQ_INT(ls_transition_dependencies_stopped(1, false), 0);
    LS_EQ_INT(ls_transition_dependencies_stopped(0, true), 0);
    LS_EQ_INT(ls_transition_dependencies_stopped(-1, false), -1);
    LS_EQ_INT(ls_transition_dependencies_stopped(-1, true), -1);
    LS_EQ_INT(ls_transition_dependencies_stopped(1, true), 1);
}

typedef struct {
    ls_transition_t s;
    char trace[32];
    int used, status, built;
    bool stop_ok, unload_ok, build_ok, retarget_in_build;
} fixture_t;
static void record(fixture_t *f, char c) { f->trace[f->used++] = c; }
static uint32_t stop(void *v) { fixture_t *f=v; record(f,'S'); return f->stop_ok ? 1 : 0; }
static int stopped(void *v,uint32_t token) { (void)token; return ((fixture_t *)v)->status; }
static bool unload(void *v,int idx) { (void)idx; fixture_t *f=v; record(f,'U'); return f->unload_ok; }
static bool build(void *v,int idx) {
    fixture_t *f=v; record(f,'B'); f->built=idx;
    if(f->retarget_in_build) { f->retarget_in_build=false; ls_transition_request(&f->s,3); }
    return f->build_ok;
}
static void init(fixture_t *f) {
    memset(f,0,sizeof(*f)); ls_transition_init(&f->s);
    f->stop_ok=f->unload_ok=f->build_ok=true; f->s.current=0;
}
static void tick(fixture_t *f,uint32_t now) {
    ls_transition_hooks_t h={stop,stopped,unload,build,f};
    ls_transition_tick(&f->s,&h,now,1000);
}
LS_CASE(unload_waits_for_ack_and_runs_after_request_callback)
{
    fixture_t f; init(&f); ls_transition_request(&f.s,1);
    LS_EQ_STR(f.trace,""); tick(&f,0); LS_EQ_STR(f.trace,"S");
    tick(&f,10); LS_EQ_STR(f.trace,"S"); f.status=1;
    tick(&f,20); LS_EQ_STR(f.trace,"S"); tick(&f,30);
    LS_EQ_STR(f.trace,"SU"); LS_EQ_INT(f.s.current,-1);
    tick(&f,40); LS_EQ_STR(f.trace,"SUB"); LS_EQ_INT(f.s.current,1);
    LS_EQ_INT(f.s.phase,LS_TRANS_IDLE);
}
LS_CASE(app_worker_retirement_precedes_replacement_build)
{
    fixture_t f; init(&f); ls_transition_request(&f.s, 1);
    tick(&f, 0);
    f.status = ls_transition_dependencies_stopped(1, false);
    tick(&f, 10); tick(&f, 20);
    LS_EQ_STR(f.trace, "S");
    LS_EQ_INT(f.s.phase, LS_TRANS_WAIT);
    f.status = ls_transition_dependencies_stopped(1, true);
    tick(&f, 30); tick(&f, 40); tick(&f, 50);
    LS_EQ_STR(f.trace, "SUB");
    LS_EQ_INT(f.s.phase, LS_TRANS_IDLE);
}

LS_CASE(latest_repeated_taps_do_not_duplicate_teardown)
{
    fixture_t f; init(&f); ls_transition_request(&f.s,1); tick(&f,0);
    ls_transition_request(&f.s,2); ls_transition_request(&f.s,2);
    f.status=1; tick(&f,10); tick(&f,20); tick(&f,30);
    LS_EQ_STR(f.trace,"SUB"); LS_EQ_INT(f.built,2);
    ls_transition_request(&f.s,2); tick(&f,40); LS_EQ_STR(f.trace,"SUB");
}
LS_CASE(timeout_and_stop_failure_never_destroy_live_tree)
{
    fixture_t f; init(&f); ls_transition_request(&f.s,1); tick(&f,UINT32_MAX-500);
    tick(&f,600); LS_EQ_INT(f.s.phase,LS_TRANS_FAILED); LS_EQ_STR(f.trace,"S");
    ls_transition_request(&f.s,2); f.status=-1; tick(&f,700); tick(&f,710);
    LS_EQ_INT(f.s.phase,LS_TRANS_FAILED); LS_EQ_STR(f.trace,"SS");
    f.stop_ok=false; ls_transition_request(&f.s,1); tick(&f,800);
    LS_EQ_INT(f.s.phase,LS_TRANS_FAILED); LS_EQ_STR(f.trace,"SSS");
}
LS_CASE(close_failure_and_build_failure_fail_closed)
{
    fixture_t f; init(&f); f.status=1; f.unload_ok=false;
    ls_transition_request(&f.s,1); tick(&f,0); tick(&f,1); tick(&f,2);
    LS_EQ_STR(f.trace,"SU"); LS_EQ_INT(f.s.current,0);
    LS_EQ_INT(f.s.phase,LS_TRANS_FAILED);
    f.unload_ok=true; f.build_ok=false; ls_transition_request(&f.s,2);
    tick(&f,3); tick(&f,4); tick(&f,5); tick(&f,6);
    LS_EQ_INT(f.s.phase,LS_TRANS_FAILED); LS_EQ_INT(f.s.current,-1);
}
LS_CASE(reentrant_request_during_build_is_not_lost)
{
    fixture_t f; init(&f); f.status=1; f.retarget_in_build=true;
    ls_transition_request(&f.s,1); tick(&f,0); tick(&f,1); tick(&f,2); tick(&f,3);
    LS_EQ_INT(f.s.current,1); LS_EQ_INT(f.s.target,3); LS_EQ_INT(f.s.phase,LS_TRANS_STOP);
    tick(&f,4); tick(&f,5); tick(&f,6); tick(&f,7);
    LS_EQ_STR(f.trace,"SUBSUB"); LS_EQ_INT(f.s.current,3);
}
LS_CASE(close_all_unloads_without_a_target_app)
{
    fixture_t f; init(&f); f.status=1; ls_transition_request(&f.s,-1);
    tick(&f,0); tick(&f,1); tick(&f,2); tick(&f,3);
    LS_EQ_INT(f.s.current,-1); LS_EQ_INT(f.built,-1); LS_EQ_INT(f.s.phase,LS_TRANS_IDLE);
}
LS_CASE(minimal_memory_floor_rejects_measured_fragmentation)
{
    LS_CHECK(!ls_transition_memory_admit(30000000,20000000,84));
    LS_CHECK(!ls_transition_memory_admit(1023,512,512));
    LS_CHECK(!ls_transition_memory_admit(4096,255,512));
    LS_CHECK(!ls_transition_memory_admit(4096,512,255));
    LS_CHECK(ls_transition_memory_admit(1024,256,256));
}
