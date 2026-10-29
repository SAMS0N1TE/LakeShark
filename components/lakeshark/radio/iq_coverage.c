/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "iq_coverage.h"
#include <string.h>
#include "ls_cold.h"
_Static_assert(sizeof(ls_iq_coverage_t)<=4096,"coverage PSRAM budget");

static uint64_t add(ls_iq_coverage_t *s,uint64_t a,uint64_t b)
{
    if (UINT64_MAX-a<b) {s->status.counter_saturated=true;return UINT64_MAX;}
    return a+b;
}
static void interval(ls_iq_coverage_t *s,ls_iq_coverage_state_t state,
    uint64_t begin,uint64_t end,const ls_radio_iq_read_receipt_t *r,
    uint64_t delivered,uint64_t discarded)
{
    if(end<begin){s->status.timing_invalid=true;return;}
    if(end==begin && !delivered && !discarded)return;
    s->status.state_us[state]=add(s,s->status.state_us[state],end-begin);
    ls_iq_coverage_interval_t *e=NULL;
    if(s->status.retained_intervals){
        size_t last=(s->head+(size_t)s->status.retained_intervals-1)%LS_IQ_COVERAGE_CAPACITY;
        ls_iq_coverage_interval_t *p=&s->intervals[last];
        if(p->state==state && p->ended_us==begin && p->context_known==(r&&r->known) &&
           (!r || (p->context_epoch==r->context_epoch && p->stream_epoch==r->transport.epoch)) &&
           p->calls_suppressed==s->status.calls_suppressed &&
           p->plan_id==s->plan_id && p->plan_step==s->plan_step && p->plan_repeat==s->plan_repeat)e=p;
    }
    if(!e){
        size_t at;
        if(s->status.retained_intervals<LS_IQ_COVERAGE_CAPACITY){
            at=(s->head+(size_t)s->status.retained_intervals++)%LS_IQ_COVERAGE_CAPACITY;
        }else{at=s->head;s->head=(s->head+1)%LS_IQ_COVERAGE_CAPACITY;
            s->status.overwritten_intervals=add(s,s->status.overwritten_intervals,1);}
        e=&s->intervals[at];memset(e,0,sizeof(*e));
        s->sequence=add(s,s->sequence,1);e->sequence=s->sequence;
        e->started_us=begin;e->state=state;e->context_known=r&&r->known;
        e->calls_suppressed=s->status.calls_suppressed;
        e->plan_id=s->plan_id;e->plan_step=s->plan_step;e->plan_repeat=s->plan_repeat;
        if(r){e->context_epoch=r->context_epoch;e->stream_epoch=r->transport.epoch;
            e->configured_center_hz=r->configured_center_hz;e->nominal_rate_hz=r->nominal_rate_hz;}
    }
    e->ended_us=end;e->delivered_bytes=add(s,e->delivered_bytes,delivered);
    if(delivered)e->delivered_blocks=add(s,e->delivered_blocks,1);
    e->settling_discarded_bytes=add(s,e->settling_discarded_bytes,discarded);
}
static bool advance(ls_iq_coverage_t *s,uint64_t now)
{
    if(now<s->cursor_us){s->status.timing_invalid=true;return false;}
    interval(s,s->state,s->cursor_us,now,NULL,0,0);
    s->cursor_us=s->status.last_update_us=now;return true;
}
bool ls_iq_coverage_start(ls_iq_coverage_t *s,uint64_t now)
{
    if(!s || s->status.active || s->status.session_id==UINT32_MAX)return false;
    uint32_t id=s->status.session_id+1;memset(s,0,sizeof(*s));
    s->status.session_id=id;s->status.active=true;
    s->status.started_us=s->status.last_update_us=s->cursor_us=now;
    return true;
}
LS_COLD bool ls_iq_coverage_stop(ls_iq_coverage_t *s,uint64_t now)
{
    if(!s || !s->status.active)return false;
    (void)advance(s,now);s->status.active=false;s->status.stopped_us=now;
    s->pending_accepted=false;return true;
}
void ls_iq_coverage_bind_plan(ls_iq_coverage_t *s,uint32_t id,uint32_t step,uint32_t repeat)
{if(s){s->plan_id=id;s->plan_step=step;s->plan_repeat=repeat;}}
void ls_iq_coverage_calls_suppressed(ls_iq_coverage_t *s,bool suppressed)
{if(s)s->status.calls_suppressed=suppressed;}
void ls_iq_coverage_boundary(ls_iq_coverage_t *s,ls_iq_coverage_state_t state,uint64_t now)
{
    if(!s || !s->status.active)return;
    if((unsigned)state>=LS_IQ_COVERAGE_STATE_COUNT || state==LS_IQ_COVERAGE_RECEIVING)state=LS_IQ_COVERAGE_UNKNOWN;
    (void)advance(s,now);s->state=state;s->guard_epoch=s->guard_context=0;
    s->guard_blocks=0;s->guard_started_us=0;s->pending_accepted=false;
}
static void transport_delta(ls_iq_coverage_t *s,const ls_radio_iq_transport_t *t)
{
    ls_radio_iq_transport_t *a=&s->transport_baseline;
    if(t->counters_saturated){s->status.counter_saturated=true;*a=*t;return;}
    if(a->known && a->epoch==t->epoch){
#define COUNT(field) do {if(t->field<a->field)s->status.counter_saturated=true; \
 else s->status.field=add(s,s->status.field,t->field-a->field);}while(0)
        COUNT(ring_rejected_bytes);COUNT(transfer_error_events);
        COUNT(submission_error_events);COUNT(flush_discarded_bytes);
#undef COUNT
    }
    /* New transport counter epoch: baseline it, never include pre-session
     * losses or subtract across a transport reset. Hardware loss unknown. */
    *a=*t;
}
bool ls_iq_coverage_accept(ls_iq_coverage_t *s,const ls_radio_iq_read_receipt_t *r,size_t bytes)
{
    if(!s || !s->status.active)return true; /* ordinary FM remains unaffected */
    s->pending_accepted=false;
    if(!r || r->read_finished_us<r->read_started_us){
        s->status.timing_invalid=true;return false;
    }
    /* Console start/control boundaries can legitimately occur during an
     * already in-flight read. Discard that window; it is not clock reversal. */
    if(r->read_started_us<s->cursor_us){
        s->status.provenance_rejected_blocks=add(s,s->status.provenance_rejected_blocks,1);
        if(r->read_finished_us>s->cursor_us){
            interval(s,LS_IQ_COVERAGE_UNKNOWN,s->cursor_us,r->read_finished_us,NULL,0,0);
            s->cursor_us=s->status.last_update_us=r->read_finished_us;
        }
        s->state=LS_IQ_COVERAGE_UNKNOWN;return false;
    }
    if(!advance(s,r->read_started_us))return false;
    bool known=r->known && r->context_epoch && r->context_epoch!=UINT64_MAX &&
        r->transport.known && r->transport.fifo_reset_known &&
        !r->transport.counters_saturated && r->transport.epoch &&
        r->transport.epoch!=UINT64_MAX && r->nominal_rate_hz &&
        r->configured_center_hz && bytes && !(bytes&1u);
    if(!known){
        s->status.provenance_rejected_blocks=add(s,s->status.provenance_rejected_blocks,1);
        interval(s,LS_IQ_COVERAGE_UNKNOWN,r->read_started_us,r->read_finished_us,NULL,0,0);
        s->cursor_us=s->status.last_update_us=r->read_finished_us;s->state=LS_IQ_COVERAGE_UNKNOWN;
        s->guard_epoch=s->guard_context=0;return false;
    }
    transport_delta(s,&r->transport);
    if(s->guard_epoch!=r->transport.epoch || s->guard_context!=r->context_epoch){
        s->guard_epoch=r->transport.epoch;s->guard_context=r->context_epoch;
        s->guard_started_us=r->read_finished_us;s->guard_blocks=0;
    }
    bool guard=r->read_finished_us>=s->guard_started_us &&
        r->read_finished_us-s->guard_started_us>=LS_IQ_COVERAGE_GUARD_US &&
        s->guard_blocks>=LS_IQ_COVERAGE_GUARD_BLOCKS;
    if(!guard){
        if(s->guard_blocks!=UINT32_MAX)++s->guard_blocks;
        interval(s,LS_IQ_COVERAGE_SETTLING,r->read_started_us,r->read_finished_us,r,0,bytes);
        s->status.settling_discarded_bytes=add(s,s->status.settling_discarded_bytes,bytes);
        s->cursor_us=s->status.last_update_us=r->read_finished_us;s->state=LS_IQ_COVERAGE_SETTLING;
        return false;
    }
    s->pending=*r;s->pending_accepted=true;return true;
}
bool ls_iq_coverage_delivered(ls_iq_coverage_t *s,size_t bytes,uint64_t now)
{
    if(!s || !s->status.active || !s->pending_accepted)return false;
    s->pending_accepted=false;
    if(now<s->pending.read_finished_us || now<s->cursor_us || !bytes || (bytes&1u)){
        s->status.timing_invalid=true;return false;
    }
    interval(s,LS_IQ_COVERAGE_RECEIVING,s->pending.read_started_us,now,&s->pending,bytes,0);
    s->status.delivered_bytes=add(s,s->status.delivered_bytes,bytes);
    s->status.delivered_blocks=add(s,s->status.delivered_blocks,1);
    s->cursor_us=s->status.last_update_us=now;s->state=LS_IQ_COVERAGE_UNKNOWN;
    return true;
}
LS_COLD bool ls_iq_coverage_snapshot(const ls_iq_coverage_t *s,ls_iq_coverage_status_t *out,
    ls_iq_coverage_interval_t *entries,size_t cap,size_t *count)
{
    if(count)*count=0;
    if(out)memset(out,0,sizeof(*out));
    if(!s || !out || !s->status.session_id || (cap&&!entries))return false;
    *out=s->status;size_t n=cap<s->status.retained_intervals?cap:(size_t)s->status.retained_intervals;
    for(size_t i=0;i<n;i++)entries[i]=s->intervals[(s->head+i)%LS_IQ_COVERAGE_CAPACITY];
    if(count)*count=n;
    return true;
}
const char *ls_iq_coverage_state_name(ls_iq_coverage_state_t state)
{
    static const char *const names[]={"unknown","receiving","settling","busy","unavailable"};
    return (unsigned)state<LS_IQ_COVERAGE_STATE_COUNT?names[state]:"unknown";
}
