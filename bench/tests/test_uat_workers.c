/* Compile the device worker branch against task/timer observation shims. */
#include "ls_test.h"
#include "uat_stream_rx.h"
#include "radio_decode_worker.h"
#include "freertos/task.h"
#include "esp_timer.h"
static bool deny_decoder;
static unsigned dec_priority,dec_core,release_calls;
bool ls_radio_decode_worker_start(TaskFunction_t fn,const char *name,UBaseType_t pri,BaseType_t core)
{ LS_CHECK(fn != NULL); LS_EQ_STR(name,"uat_decode"); dec_priority=pri;dec_core=core;return !deny_decoder; }
bool ls_radio_decode_worker_release(unsigned tries,uint32_t ms)
{ LS_CHECK(tries>0 && ms>0);++release_calls;return true; }
uint32_t ls_lora_fsk_bw_snap(uint32_t hz) { return hz; }
int ls_lora_fsk_stream_read(uint8_t *buf,size_t n,bool *restart)
{ (void)buf;(void)n;*restart=false;return 0; }
LS_CASE(reader_and_decoder_have_separate_cores_and_safe_lifecycle)
{
    ls_shim_task_reset();ls_shim_task_set_state(eSuspended);
    deny_decoder=false;release_calls=0;
    unsigned timers=ls_shim_timer_count();
    LS_CHECK(uat_stream_rx_start(2000000));
    LS_EQ_UINT(dec_core,1);LS_EQ_UINT(dec_priority,10);
    LS_EQ_UINT(ls_shim_task_last_core_id(),0);
    LS_CHECK(ls_shim_task_last_priority()>dec_priority);
    LS_EQ_STR(ls_shim_task_last_name(),"uat_read");
    LS_EQ_UINT(ls_shim_task_last_stack_depth(),4096);
    LS_CHECK(ls_shim_task_last_static_stack()!=NULL);
    LS_EQ_UINT(ls_shim_timer_count(),timers+1);
    LS_CHECK(!uat_stream_rx_start(1500000));
    ls_shim_task_set_state(eSuspended);uat_stream_rx_stop();
    LS_EQ_UINT(release_calls,1);LS_EQ_UINT(ls_shim_timer_count(),timers);
    LS_EQ_UINT(ls_shim_task_running_delete_count(),0);
    LS_CHECK(uat_stream_rx_start(1250000));ls_shim_task_set_state(eSuspended);uat_stream_rx_stop();
}
LS_CASE(partial_start_failure_releases_timer_and_can_retry)
{
    ls_shim_task_reset();ls_shim_task_set_state(eSuspended);
    unsigned timers=ls_shim_timer_count();
    deny_decoder=true;LS_CHECK(!uat_stream_rx_start(2000000));
    LS_EQ_UINT(ls_shim_timer_count(),timers);
    deny_decoder=false;ls_shim_task_fail_create(pdTRUE);
    LS_CHECK(!uat_stream_rx_start(2000000));LS_EQ_UINT(ls_shim_timer_count(),timers);
    ls_shim_task_fail_create(pdFALSE);LS_CHECK(uat_stream_rx_start(1500000));ls_shim_task_set_state(eSuspended);uat_stream_rx_stop();
    LS_EQ_UINT(ls_shim_timer_count(),timers);
    LS_CHECK(!uat_stream_rx_start(1000000));
}
