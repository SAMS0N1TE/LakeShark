/* LS_TEST_SOURCES: ${FW}/components/lakeshark/radio/rx_plan.c */
#include "ls_test.h"
#include "rx_plan.h"
#include <string.h>
static ls_rx_plan_t plan;
static ls_rx_plan_step_t steps[8];
static uint32_t hz;
static void load(unsigned n,unsigned repeats)
{
    memset(&plan,0,sizeof(plan));
    for(unsigned i=0;i<8;++i)steps[i]=(ls_rx_plan_step_t){162400000+25000*i,1000};
    LS_CHECK(ls_rx_plan_load(&plan,"weather",steps,n,repeats));
}
LS_CASE(rx_plan_executes_tune_guard_dwell_repeat_and_conditional_restore)
{
    load(2,2);LS_CHECK(ls_rx_plan_start(&plan,154785000,100));
    uint64_t now=100;
    for(unsigned visit=0;visit<4;++visit) {
        LS_CHECK(ls_rx_plan_poll(&plan,now,true,&hz)==LS_RX_PLAN_ACTION_TUNE);
        LS_CHECK(hz==steps[visit%2].center_hz && plan.status.state==LS_RX_PLAN_TUNING);
        ls_rx_plan_tuned(&plan,true,now+10);
        LS_CHECK(ls_rx_plan_poll(&plan,now+50000,true,&hz)==LS_RX_PLAN_ACTION_NONE);
        LS_CHECK(plan.status.state==LS_RX_PLAN_SETTLING);
        ls_rx_plan_qualified(&plan,now+100000);
        LS_CHECK(plan.status.state==LS_RX_PLAN_DWELLING);
        LS_CHECK(ls_rx_plan_poll(&plan,now+1099999,true,&hz)==LS_RX_PLAN_ACTION_NONE);
        now+=1100000;
        if(visit!=3) {
            /* The next poll both completes the dwell and starts the next tune. */
            LS_CHECK(ls_rx_plan_poll(&plan,now,true,&hz)==LS_RX_PLAN_ACTION_TUNE);
            ls_rx_plan_tuned(&plan,true,now+1);ls_rx_plan_qualified(&plan,now+100000);
            now+=1100000;visit++;
        }
    }
    LS_CHECK(ls_rx_plan_poll(&plan,now,true,&hz)==LS_RX_PLAN_ACTION_RESTORE && hz==154785000);
    LS_CHECK(plan.status.steps[0].completed==2 && plan.status.steps[1].completed==2);
    ls_rx_plan_restore_result(&plan,true,now+10);
    LS_CHECK(!plan.status.active && plan.status.state==LS_RX_PLAN_COMPLETED && plan.status.restored);
}
LS_CASE(rx_plan_dwell_cannot_start_from_tune_success_or_time_alone)
{
    load(1,1);LS_CHECK(ls_rx_plan_start(&plan,154785000,0));
    LS_CHECK(ls_rx_plan_poll(&plan,0,true,&hz)==LS_RX_PLAN_ACTION_TUNE);
    ls_rx_plan_tuned(&plan,true,1);
    LS_CHECK(ls_rx_plan_poll(&plan,1000000,true,&hz)==LS_RX_PLAN_ACTION_NONE);
    LS_CHECK(!plan.status.steps[0].dwell_started && plan.status.steps[0].completed==0);
    LS_CHECK(ls_rx_plan_poll(&plan,2000001,true,&hz)==LS_RX_PLAN_ACTION_RESTORE);
    ls_rx_plan_restore_result(&plan,true,2000100);
    LS_CHECK(plan.status.state==LS_RX_PLAN_FAILED && plan.status.reason==LS_RX_PLAN_SETTLE_TIMEOUT);
}
LS_CASE(rx_plan_stop_restores_but_manual_or_ownership_loss_never_restores)
{
    load(1,1);LS_CHECK(ls_rx_plan_start(&plan,154785000,0));LS_CHECK(ls_rx_plan_stop(&plan));
    LS_CHECK(ls_rx_plan_poll(&plan,1,true,&hz)==LS_RX_PLAN_ACTION_NONE);
    LS_CHECK(plan.status.state==LS_RX_PLAN_STOPPED && !plan.status.restore_attempted);
    load(1,1);LS_CHECK(ls_rx_plan_start(&plan,154785000,0));
    LS_CHECK(ls_rx_plan_poll(&plan,1,true,&hz)==LS_RX_PLAN_ACTION_TUNE);
    ls_rx_plan_tuned(&plan,true,2);ls_rx_plan_qualified(&plan,3);LS_CHECK(ls_rx_plan_stop(&plan));
    LS_CHECK(ls_rx_plan_poll(&plan,4,true,&hz)==LS_RX_PLAN_ACTION_RESTORE);
    LS_CHECK(plan.status.steps[0].completed==0 && plan.status.steps[0].last_dwell_ended_us==4);
    ls_rx_plan_cancel(&plan,LS_RX_PLAN_MANUAL_OVERRIDE,5);
    LS_CHECK(ls_rx_plan_poll(&plan,6,true,&hz)==LS_RX_PLAN_ACTION_NONE && !plan.status.restored);
    load(1,1);LS_CHECK(ls_rx_plan_start(&plan,154785000,0));
    LS_CHECK(ls_rx_plan_poll(&plan,1,false,&hz)==LS_RX_PLAN_ACTION_NONE);
    LS_CHECK(plan.status.state==LS_RX_PLAN_CANCELLED && plan.status.reason==LS_RX_PLAN_CONTEXT_LOST);
}
LS_CASE(rx_plan_bounds_clock_and_control_failure_do_not_credit_completion)
{
    load(8,1);LS_CHECK(!ls_rx_plan_load(&plan,"bad\nname",steps,1,1));
    LS_CHECK(!ls_rx_plan_load(&plan,"too long",steps,9,1));
    LS_CHECK(!ls_rx_plan_load(&plan,"over budget",steps,8,4));
    steps[0].dwell_ms=999;LS_CHECK(!ls_rx_plan_load(&plan,"short",steps,1,1));
    steps[0].dwell_ms=30001;LS_CHECK(!ls_rx_plan_load(&plan,"long",steps,1,1));
    load(1,1);LS_CHECK(ls_rx_plan_start(&plan,154785000,100));
    LS_CHECK(ls_rx_plan_poll(&plan,99,true,&hz)==LS_RX_PLAN_ACTION_NONE);
    LS_CHECK(plan.status.clock_invalid && plan.status.state==LS_RX_PLAN_FAILED && !plan.status.restore_attempted);
    load(1,1);LS_CHECK(ls_rx_plan_start(&plan,154785000,0));
    LS_CHECK(ls_rx_plan_poll(&plan,1,true,&hz)==LS_RX_PLAN_ACTION_TUNE);ls_rx_plan_tuned(&plan,false,2);
    LS_CHECK(plan.status.reason==LS_RX_PLAN_TUNE_FAILED && !plan.status.active && !plan.status.steps[0].completed);
}
