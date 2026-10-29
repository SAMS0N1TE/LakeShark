/* LS_TEST_SOURCES: ${FW}/components/lakeshark/radio/iq_coverage.c */
#include "ls_test.h"
#include "iq_coverage.h"
#include <string.h>

static ls_radio_iq_read_receipt_t read_window(uint64_t begin,uint64_t end)
{
    return (ls_radio_iq_read_receipt_t){.known=true,.read_started_us=begin,
        .read_finished_us=end,.context_epoch=7,.configured_center_hz=100100000,
        .nominal_rate_hz=256000,.transport={.known=true,.fifo_reset_known=true,.epoch=3}};
}
static void guarded(ls_iq_coverage_t *s)
{
    ls_radio_iq_read_receipt_t r=read_window(1000,33000);
    LS_CHECK(!ls_iq_coverage_accept(s,&r,16384));
    r=read_window(34000,66000);LS_CHECK(!ls_iq_coverage_accept(s,&r,16384));
    r=read_window(67000,99000);LS_CHECK(!ls_iq_coverage_accept(s,&r,16384));
}
LS_CASE(qualified_live_blocks_credit_only_read_dsp_windows_and_gaps_remain_unknown)
{
    ls_iq_coverage_t s={0};LS_CHECK(ls_iq_coverage_start(&s,1000));
    ls_iq_coverage_bind_plan(&s,9,1,2);
    ls_iq_coverage_calls_suppressed(&s,true);
    ls_iq_coverage_boundary(&s,LS_IQ_COVERAGE_SETTLING,1000);guarded(&s);
    ls_radio_iq_read_receipt_t r=read_window(100000,133000);
    LS_CHECK(ls_iq_coverage_accept(&s,&r,16384));
    LS_EQ_UINT(s.status.delivered_bytes,0); /* lease/read alone earns no DSP credit */
    LS_CHECK(ls_iq_coverage_delivered(&s,16384,134000));
    LS_CHECK(!ls_iq_coverage_delivered(&s,16384,134000)); /* cannot double credit */
    r=read_window(140000,172000);LS_CHECK(ls_iq_coverage_accept(&s,&r,16384));
    LS_CHECK(ls_iq_coverage_delivered(&s,16384,173000));
    ls_iq_coverage_boundary(&s,LS_IQ_COVERAGE_UNKNOWN,180000);
    ls_iq_coverage_calls_suppressed(&s,false);
    LS_CHECK(ls_iq_coverage_stop(&s,180000));
    LS_CHECK(!s.status.calls_suppressed);
    LS_EQ_UINT(s.status.delivered_bytes,32768);LS_EQ_UINT(s.status.delivered_blocks,2);
    LS_EQ_UINT(s.status.state_us[LS_IQ_COVERAGE_RECEIVING],67000);
    LS_EQ_UINT(s.status.state_us[LS_IQ_COVERAGE_UNKNOWN],13000);
    LS_EQ_UINT(s.status.state_us[LS_IQ_COVERAGE_SETTLING],99000);
    LS_EQ_UINT(s.status.settling_discarded_bytes,3*16384);
    uint64_t total=0;for(unsigned i=0;i<LS_IQ_COVERAGE_STATE_COUNT;i++)total+=s.status.state_us[i];
    LS_EQ_UINT(total,179000);LS_CHECK(!s.status.timing_invalid);
    bool delivered=false;for(unsigned i=0;i<s.status.retained_intervals;i++){
        if(s.intervals[i].state==LS_IQ_COVERAGE_RECEIVING){
            delivered=true;LS_CHECK(s.intervals[i].context_known);
            LS_CHECK(s.intervals[i].calls_suppressed);
            LS_EQ_UINT(s.intervals[i].plan_id,9);LS_EQ_UINT(s.intervals[i].plan_step,1);
        }
    }LS_CHECK(delivered);
}
LS_CASE(crossing_session_or_control_read_is_unknown_without_false_clock_regression)
{
    ls_iq_coverage_t s={0};LS_CHECK(ls_iq_coverage_start(&s,1000));
    ls_radio_iq_read_receipt_t r=read_window(500,2000);
    LS_CHECK(!ls_iq_coverage_accept(&s,&r,16384));LS_CHECK(!s.status.timing_invalid);
    ls_iq_coverage_boundary(&s,LS_IQ_COVERAGE_SETTLING,2200);
    r=read_window(2100,2500);LS_CHECK(!ls_iq_coverage_accept(&s,&r,16384));
    LS_CHECK(!s.status.timing_invalid);LS_EQ_UINT(s.status.provenance_rejected_blocks,2);
    r=read_window(3000,2000);LS_CHECK(!ls_iq_coverage_accept(&s,&r,16384));
    LS_CHECK(s.status.timing_invalid);LS_EQ_UINT(s.status.delivered_bytes,0);
}
LS_CASE(control_boundary_after_accept_cancels_pending_credit_and_lease_cannot_mark_receiving)
{
    ls_iq_coverage_t s={0};LS_CHECK(ls_iq_coverage_start(&s,1000));guarded(&s);
    ls_radio_iq_read_receipt_t r=read_window(100000,133000);
    LS_CHECK(ls_iq_coverage_accept(&s,&r,16384));
    ls_iq_coverage_boundary(&s,LS_IQ_COVERAGE_UNKNOWN,134000);
    LS_CHECK(!ls_iq_coverage_delivered(&s,16384,135000));
    LS_EQ_UINT(s.status.delivered_blocks,0);
    ls_iq_coverage_boundary(&s,LS_IQ_COVERAGE_RECEIVING,140000);
    LS_CHECK(ls_iq_coverage_stop(&s,150000));
    LS_EQ_UINT(s.status.state_us[LS_IQ_COVERAGE_RECEIVING],0);
    LS_CHECK(!s.status.timing_invalid);
}
LS_CASE(unknown_failed_or_changed_provenance_cannot_bypass_settling_or_credit_dsp)
{
    ls_iq_coverage_t s={0};LS_CHECK(ls_iq_coverage_start(&s,1000));guarded(&s);
    ls_radio_iq_read_receipt_t r=read_window(100000,133000);
    r.transport.fifo_reset_known=false;LS_CHECK(!ls_iq_coverage_accept(&s,&r,16384));
    LS_CHECK(!ls_iq_coverage_delivered(&s,16384,134000));
    r=read_window(140000,172000);r.context_epoch=8;
    LS_CHECK(!ls_iq_coverage_accept(&s,&r,16384)); /* restart guard */
    r=read_window(180000,212000);r.transport.epoch=UINT64_MAX;
    LS_CHECK(!ls_iq_coverage_accept(&s,&r,16384));
    LS_EQ_UINT(s.status.delivered_bytes,0);
    ls_iq_coverage_boundary(&s,LS_IQ_COVERAGE_BUSY,220000);
    ls_iq_coverage_boundary(&s,LS_IQ_COVERAGE_UNAVAILABLE,230000);
    LS_CHECK(ls_iq_coverage_stop(&s,240000));
    LS_EQ_UINT(s.status.state_us[LS_IQ_COVERAGE_BUSY],10000);
    LS_EQ_UINT(s.status.state_us[LS_IQ_COVERAGE_UNAVAILABLE],10000);
}
LS_CASE(transport_losses_are_observed_counter_deltas_never_pre_session_or_cross_epoch_subtraction)
{
    ls_iq_coverage_t s={0};LS_CHECK(ls_iq_coverage_start(&s,0));
    ls_radio_iq_read_receipt_t r=read_window(1,10000);r.transport.ring_rejected_bytes=50;
    LS_CHECK(!ls_iq_coverage_accept(&s,&r,16384));LS_EQ_UINT(s.status.ring_rejected_bytes,0);
    r=read_window(10001,20000);r.transport.ring_rejected_bytes=60;r.transport.transfer_error_events=2;
    LS_CHECK(!ls_iq_coverage_accept(&s,&r,16384));
    LS_EQ_UINT(s.status.ring_rejected_bytes,10);LS_EQ_UINT(s.status.transfer_error_events,2);
    r=read_window(20001,30000);r.transport.epoch=4;r.transport.ring_rejected_bytes=100000;
    LS_CHECK(!ls_iq_coverage_accept(&s,&r,16384));LS_EQ_UINT(s.status.ring_rejected_bytes,10);
}
LS_CASE(bounded_retention_preserves_totals_and_reports_overwritten_intervals)
{
    ls_iq_coverage_t s={0};LS_CHECK(ls_iq_coverage_start(&s,1000));guarded(&s);
    for(unsigned i=0;i<100;i++){
        uint64_t begin=100000+(uint64_t)i*40000;
        ls_radio_iq_read_receipt_t r=read_window(begin,begin+33000);
        LS_CHECK(ls_iq_coverage_accept(&s,&r,16384));
        LS_CHECK(ls_iq_coverage_delivered(&s,16384,begin+34000));
    }
    LS_EQ_UINT(s.status.retained_intervals,LS_IQ_COVERAGE_CAPACITY);
    LS_CHECK(s.status.overwritten_intervals>0);LS_EQ_UINT(s.status.delivered_blocks,100);
    LS_EQ_UINT(s.status.delivered_bytes,100*16384);
    ls_iq_coverage_status_t status;ls_iq_coverage_interval_t entries[2];size_t count=0;
    LS_CHECK(ls_iq_coverage_snapshot(&s,&status,entries,2,&count));LS_EQ_UINT(count,2);
    LS_CHECK(entries[0].sequence<entries[1].sequence);
}
