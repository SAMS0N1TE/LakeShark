/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "rx_plan.h"
#include <limits.h>
#include <string.h>
_Static_assert(sizeof(ls_rx_plan_t) <= 1536, "RX plan PSRAM budget");
static bool name_ok(const char *s)
{
    if (!s || !*s) return false;
    for (size_t i=0;i<32;++i) { unsigned char c=(unsigned char)s[i]; if(!c)return true; if(c<32||c>126)return false; }
    return false;
}
bool ls_rx_plan_load(ls_rx_plan_t *p,const char *name,const ls_rx_plan_step_t *steps,size_t n,uint32_t repeats)
{
    if (!p || p->status.active || p->status.plan_id==UINT32_MAX || !name_ok(name) ||
        !steps || !n || n>LS_RX_PLAN_STEPS || !repeats || repeats>LS_RX_PLAN_REPEATS) return false;
    uint64_t total=0;
    for(size_t i=0;i<n;++i) {
        if(steps[i].center_hz<24000000 || steps[i].center_hz>1766000000 ||
           steps[i].dwell_ms<1000 || steps[i].dwell_ms>30000)return false;
        total+=(uint64_t)steps[i].dwell_ms*1000+2*LS_RX_PLAN_PHASE_US;
    }
    if(total*repeats+LS_RX_PLAN_PHASE_US>LS_RX_PLAN_MAX_US)return false;
    uint32_t id=p->status.plan_id+1;
    memset(p,0,sizeof(*p)); p->status.plan_id=id;
    memcpy(p->status.name,name,strlen(name)+1);
    p->status.step_count=(uint32_t)n; p->status.repeat_count=repeats;
    for(size_t i=0;i<n;++i)p->status.steps[i].planned=steps[i];
    return true;
}
static void finish(ls_rx_plan_t *p,ls_rx_plan_state_t state,ls_rx_plan_reason_t why,uint64_t now)
{
    if(p->status.state==LS_RX_PLAN_DWELLING) {
        ls_rx_plan_step_status_t *s=&p->status.steps[p->status.current_step];
        s->last_dwell_ended_us=now;s->dwell_ended=true;
    }
    p->status.active=false; p->status.state=state; p->status.reason=why;
    p->status.ended=true; p->status.ended_us=now;
}
static bool clock_ok(ls_rx_plan_t *p,uint64_t now)
{
    if(now<p->last_us || now>UINT64_MAX-LS_RX_PLAN_MAX_US) {
        p->status.clock_invalid=true; finish(p,LS_RX_PLAN_FAILED,LS_RX_PLAN_CLOCK_INVALID,now); return false;
    }
    p->last_us=now; return true;
}
bool ls_rx_plan_start(ls_rx_plan_t *p,uint32_t restore,uint64_t now)
{
    if(!p || !p->status.plan_id || p->status.state!=LS_RX_PLAN_LOADED ||
       restore<24000000 || restore>1766000000 || now>UINT64_MAX-LS_RX_PLAN_MAX_US)return false;
    p->status.active=true; p->status.started=true; p->status.started_us=p->last_us=now;
    p->status.restore_center_hz=restore; p->status.state=LS_RX_PLAN_QUEUED;
    return true;
}
bool ls_rx_plan_stop(ls_rx_plan_t *p)
{ if(!p || !p->status.active)return false; p->stop_requested=true; return true; }
void ls_rx_plan_cancel(ls_rx_plan_t *p,ls_rx_plan_reason_t why,uint64_t now)
{ if(p && p->status.active)finish(p,LS_RX_PLAN_CANCELLED,why,now); }
static ls_rx_plan_action_t restore(ls_rx_plan_t *p,ls_rx_plan_state_t after,
    ls_rx_plan_reason_t why,uint64_t now,uint32_t *hz)
{
    p->status.reason=why;
    if(p->status.state==LS_RX_PLAN_DWELLING) {
        ls_rx_plan_step_status_t *s=&p->status.steps[p->status.current_step];
        s->last_dwell_ended_us=now;s->dwell_ended=true;
    }
    if(!p->changed_receiver){finish(p,after,why,now);return LS_RX_PLAN_ACTION_NONE;}
    p->after_restore=after; p->status.state=LS_RX_PLAN_RESTORING;
    p->status.restore_attempted=true; p->phase_deadline_us=now+LS_RX_PLAN_PHASE_US;
    *hz=p->status.restore_center_hz;return LS_RX_PLAN_ACTION_RESTORE;
}
ls_rx_plan_action_t ls_rx_plan_poll(ls_rx_plan_t *p,uint64_t now,bool context,uint32_t *hz)
{
    if(!p || !p->status.active || !hz)return LS_RX_PLAN_ACTION_NONE;
    if(!clock_ok(p,now))return LS_RX_PLAN_ACTION_NONE;
    if(!context){ls_rx_plan_cancel(p,LS_RX_PLAN_CONTEXT_LOST,now);return LS_RX_PLAN_ACTION_NONE;}
    if(p->status.state==LS_RX_PLAN_RESTORING) {
        if(now>=p->phase_deadline_us)finish(p,LS_RX_PLAN_FAILED,LS_RX_PLAN_RESTORE_FAILED,now);
        return LS_RX_PLAN_ACTION_NONE;
    }
    if(p->stop_requested)return restore(p,LS_RX_PLAN_STOPPED,LS_RX_PLAN_OPERATOR_STOP,now,hz);
    if(now-p->status.started_us>=LS_RX_PLAN_MAX_US-LS_RX_PLAN_PHASE_US)
        return restore(p,LS_RX_PLAN_FAILED,LS_RX_PLAN_DEADLINE,now,hz);
    if(p->status.state==LS_RX_PLAN_TUNING || p->status.state==LS_RX_PLAN_SETTLING) {
        if(now>=p->phase_deadline_us)return restore(p,LS_RX_PLAN_FAILED,LS_RX_PLAN_SETTLE_TIMEOUT,now,hz);
        return LS_RX_PLAN_ACTION_NONE;
    }
    if(p->status.state==LS_RX_PLAN_DWELLING) {
        if(now<p->phase_deadline_us)return LS_RX_PLAN_ACTION_NONE;
        ls_rx_plan_step_status_t *s=&p->status.steps[p->status.current_step];
        s->completed++; s->last_dwell_ended_us=now; s->dwell_ended=true;
        if(++p->status.current_step==p->status.step_count) {
            p->status.current_step=0;
            if(++p->status.current_repeat==p->status.repeat_count) {
                p->status.current_step=p->status.step_count-1;
                p->status.current_repeat=p->status.repeat_count-1;
                return restore(p,LS_RX_PLAN_COMPLETED,LS_RX_PLAN_OK,now,hz);
            }
        }
    }
    ls_rx_plan_step_status_t *s=&p->status.steps[p->status.current_step];
    if(!s->attempts)s->first_tune_us=now;
    s->attempts++;s->last_tune_us=now;
    p->status.state=LS_RX_PLAN_TUNING;p->phase_deadline_us=now+LS_RX_PLAN_PHASE_US;
    *hz=s->planned.center_hz;
    return LS_RX_PLAN_ACTION_TUNE;
}
void ls_rx_plan_tuned(ls_rx_plan_t *p,bool success,uint64_t now)
{
    if(!p || !p->status.active || p->status.state!=LS_RX_PLAN_TUNING || !clock_ok(p,now))return;
    if(!success){finish(p,LS_RX_PLAN_FAILED,LS_RX_PLAN_TUNE_FAILED,now);return;}
    p->changed_receiver=true;
    p->status.state=LS_RX_PLAN_SETTLING;p->phase_deadline_us=now+LS_RX_PLAN_PHASE_US;
}
void ls_rx_plan_qualified(ls_rx_plan_t *p,uint64_t now)
{
    if(!p || !p->status.active || p->status.state!=LS_RX_PLAN_SETTLING || !clock_ok(p,now))return;
    if(now>=p->phase_deadline_us)return;
    ls_rx_plan_step_status_t *s=&p->status.steps[p->status.current_step];
    s->last_dwell_started_us=now;s->dwell_started=true;s->dwell_ended=false;
    p->status.state=LS_RX_PLAN_DWELLING;p->phase_deadline_us=now+(uint64_t)s->planned.dwell_ms*1000;
}
void ls_rx_plan_restore_result(ls_rx_plan_t *p,bool success,uint64_t now)
{
    if(!p || !p->status.active || p->status.state!=LS_RX_PLAN_RESTORING || !clock_ok(p,now))return;
    p->status.restored=success;
    finish(p,success?p->after_restore:LS_RX_PLAN_FAILED,success?p->status.reason:LS_RX_PLAN_RESTORE_FAILED,now);
}
static uint64_t add(ls_rx_plan_step_status_t *s,uint64_t a,uint64_t b)
{if(b>UINT64_MAX-a){s->counter_saturated=true;return UINT64_MAX;}return a+b;}
void ls_rx_plan_qualified_block(ls_rx_plan_t *p,uint64_t now,size_t bytes,uint64_t read_started,uint32_t rate)
{
    if(!p || !p->status.active || !bytes || (bytes&1u) || !rate ||
       (p->status.state!=LS_RX_PLAN_SETTLING && p->status.state!=LS_RX_PLAN_DWELLING))return;
    if(p->status.state==LS_RX_PLAN_SETTLING)ls_rx_plan_qualified(p,now);
    else if(!clock_ok(p,now))return;
    if(!p->status.active || (p->status.state!=LS_RX_PLAN_DWELLING && p->status.state!=LS_RX_PLAN_SETTLING))return;
    ls_rx_plan_step_status_t *s=&p->status.steps[p->status.current_step];
    if(read_started>now || (s->delivered_blocks && read_started<s->last_delivery_us))s->timing_invalid=true;
    else s->software_window_us=add(s,s->software_window_us,now-read_started);
    if(!s->delivered_blocks)s->first_delivery_us=now;
    s->last_delivery_us=now;
    s->delivered_bytes=add(s,s->delivered_bytes,bytes);s->delivered_blocks=add(s,s->delivered_blocks,1);
    if(s->nominal_rate_hz && s->nominal_rate_hz!=rate){s->rate_mixed=true;s->nominal_rate_hz=0;}
    else if(!s->rate_mixed)s->nominal_rate_hz=rate;
}
bool ls_rx_plan_nominal_us(const ls_rx_plan_step_status_t *s,uint64_t *out)
{
    if(!s || !out || !s->nominal_rate_hz || s->counter_saturated || s->rate_mixed)return false;
    uint64_t pairs=s->delivered_bytes/2,whole=pairs/s->nominal_rate_hz;
    if(whole>UINT64_MAX/1000000u)return false;
    uint64_t base=whole*1000000u,fraction=(pairs%s->nominal_rate_hz)*1000000u/s->nominal_rate_hz;
    if(fraction>UINT64_MAX-base)return false;
    *out=base+fraction;return true;
}
const char *ls_rx_plan_state_name(ls_rx_plan_state_t state)
{
    static const char *const names[]={"loaded","queued","tuning","settling","dwelling","restoring","completed","stopped","cancelled","failed"};
    return (unsigned)state<sizeof(names)/sizeof(*names)?names[state]:"failed";
}
const char *ls_rx_plan_reason_name(ls_rx_plan_reason_t why)
{
    static const char *const names[]={"none","operator_stop","manual_override","context_lost","tune_failed","settle_timeout","deadline","clock_invalid","restore_failed"};
    return (unsigned)why<sizeof(names)/sizeof(*names)?names[why]:"context_lost";
}
