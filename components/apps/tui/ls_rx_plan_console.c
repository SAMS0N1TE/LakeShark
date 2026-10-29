/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ls_rx_plan_console.h"
#include "fm_state.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ls_textpack.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define ALLOC(n) heap_caps_calloc(1,n,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)
#define FREE(p) heap_caps_free(p)
#else
#define ALLOC(n) calloc(1,n)
#define FREE(p) free(p)
#endif
typedef struct {ls_rx_plan_status_t plan;char line[1536];size_t used;bool failed;} receipt_t;
_Static_assert(sizeof(receipt_t)<=3072,"RX plan receipt PSRAM budget");
static void raw(receipt_t *r,const char *s)
{
    size_t n=strlen(s);if(n>=sizeof(r->line)-r->used){r->failed=true;return;}
    memcpy(r->line+r->used,s,n);r->used+=n;r->line[r->used]=0;
}
static void string(receipt_t *r,const char *s)
{
    raw(r,"\"");for(;*s;++s){char c[2]={*s,0};if(*s=='"'||*s=='\\')raw(r,"\\");raw(r,c);}raw(r,"\"");
}
static void key(receipt_t *r,const char *s){raw(r,",");string(r,s);raw(r,":");}
static void number(receipt_t *r,const char *s,bool known,uint64_t n)
{
    key(r,s);if(!known){raw(r,"null");return;}
    char b[24];snprintf(b,sizeof(b),"\"%" PRIu64 "\"",n);raw(r,b);
}
static void boolean(receipt_t *r,const char *s,bool b){key(r,s);raw(r,b?"true":"false");}
static void field(receipt_t *r,const char *k,const char *s){key(r,k);string(r,s);}
static void begin(receipt_t *r,const char *type)
{r->used=0;r->failed=false;raw(r,"{\"schema\":3,\"type\":");string(r,type);}
static bool emit(receipt_t *r,ls_rx_plan_write_t write,void *context)
{raw(r,"}\n");return !r->failed&&write(context,r->line,r->used);}
bool ls_rx_plan_export(ls_rx_plan_write_t write,void *context)
{
    if(!write)return false;
    receipt_t *r=ALLOC(sizeof(*r));
    if(!r || !fm_rx_plan_snapshot(&r->plan)) {
        const char *error=r?"{\"schema\":3,\"type\":\"error\",\"reason\":\"plan_unavailable\",\"partial\":true}\n"
            :"{\"schema\":3,\"type\":\"error\",\"reason\":\"psram_unavailable\",\"partial\":true}\n";
        (void)write(context,error,strlen(error));if(r)FREE(r);return false;
    }
    const ls_rx_plan_status_t *s=&r->plan;
    begin(r,"rx_plan_session");number(r,"plan_id",true,s->plan_id);
    field(r,"name",s->name);field(r,"scope","bounded_rtl_fm_plan");field(r,"time_scope","current_boot_only");
    field(r,"state",ls_rx_plan_state_name(s->state));field(r,"reason",ls_rx_plan_reason_name(s->reason));
    boolean(r,"active",s->active);number(r,"step_count",true,s->step_count);
    number(r,"repeat_count",true,s->repeat_count);number(r,"current_repeat",true,s->current_repeat);
    number(r,"current_step",true,s->current_step);number(r,"started_us",s->started,s->started_us);
    number(r,"ended_us",s->ended,s->ended_us);boolean(r,"clock_invalid",s->clock_invalid);
    boolean(r,"detail_truncated",s->detail_truncated);
    boolean(r,"calls_suppressed",s->active);boolean(r,"restore_attempted",s->restore_attempted);
    boolean(r,"restored",s->restored);number(r,"restore_center_hz",s->started,s->restore_center_hz);
    field(r,"recording_policy",LS_TEXT(t_ls_rx_plan_console_suppress_fm_auto_calls, "suppress_fm_auto_calls_during_run"));
    raw(r,LS_TEXT(t_ls_rx_plan_console_rf_coverage_us_null, ",\"rf_coverage_us\":null,\"sample_continuity\":null,\"sample_frequency_verified\":false,\"boot_id\":null"));
    if(!emit(r,write,context))goto failed;
    for(size_t i=0;i<s->step_count;++i) {
        const ls_rx_plan_step_status_t *step=&s->steps[i];
        begin(r,"rx_plan_step");number(r,"plan_id",true,s->plan_id);number(r,"step_index",true,i);
        number(r,"planned_center_hz",true,step->planned.center_hz);number(r,"planned_dwell_ms",true,step->planned.dwell_ms);
        number(r,"attempts",true,step->attempts);number(r,"completed",true,step->completed);
        number(r,"first_tune_us",step->attempts!=0,step->first_tune_us);
        number(r,"last_tune_us",step->attempts!=0,step->last_tune_us);
        number(r,LS_TEXT(t_ls_rx_plan_console_last_dwell_started_us, "last_dwell_started_us"),step->dwell_started,step->last_dwell_started_us);
        number(r,"last_dwell_ended_us",step->dwell_ended,step->last_dwell_ended_us);
        number(r,LS_TEXT(t_ls_rx_plan_console_delivered_u8_iq_bytes, "delivered_u8_iq_bytes"),!step->counter_saturated,step->delivered_bytes);
        number(r,"delivered_blocks",!step->counter_saturated,step->delivered_blocks);
        number(r,LS_TEXT(t_ls_rx_plan_console_software_read_dsp_window, "software_read_dsp_window_us"),!step->counter_saturated&&!step->timing_invalid,step->software_window_us);
        number(r,"nominal_rate_hz",step->nominal_rate_hz!=0,step->nominal_rate_hz);
        uint64_t nominal=0;bool nominal_known=ls_rx_plan_nominal_us(step,&nominal);
        number(r,LS_TEXT(t_ls_rx_plan_console_nominal_sample_duration_us, "nominal_sample_duration_us"),nominal_known,nominal);
        number(r,"first_delivery_us",step->delivered_blocks!=0,step->first_delivery_us);
        number(r,"last_delivery_us",step->delivered_blocks!=0,step->last_delivery_us);
        boolean(r,"counter_saturated",step->counter_saturated);boolean(r,"timing_invalid",step->timing_invalid);
        boolean(r,"loss_unknown",true);boolean(r,"detail_truncated",s->detail_truncated&&step->attempts!=0);
        raw(r,LS_TEXT(t_ls_rx_plan_console_rf_coverage_us_null_2, ",\"rf_coverage_us\":null,\"sample_continuity\":null,\"sample_frequency_verified\":false"));
        if(!emit(r,write,context))goto failed;
    }
    FREE(r);return true;
failed:FREE(r);return false;
}
static bool decimal(const char *s,const char *end,uint32_t *out)
{
    if(s==end)return false;
    uint32_t n=0;
    for(;s<end;++s){if(*s<'0'||*s>'9'||n>(UINT32_MAX-(unsigned)(*s-'0'))/10)return false;n=n*10+(unsigned)(*s-'0');}
    *out=n;return true;
}
static bool output(void *unused,const char *line,size_t n)
{(void)unused;return fwrite(line,1,n,stdout)==n;}
int ls_rx_plan_command(int argc,char **argv)
{
    if(argc==2&&!strcmp(argv[1],"context")) {
        fm_rx_plan_context_t *c=ALLOC(sizeof(*c));
        if(!c)return 1;
        bool ok=fm_rx_plan_context(c);
        if(ok)LS_PRINTF(fmt_ls_rx_plan_console_fm_retained_mode_freq, "FM retained mode=%d freq_hz=%u gain_tenths_db=%d squelch_tenths=%d CALLS_FM_runtime=%u active=%u plan_active=%u\nRTL endpoint=%s owner=%s present=%u leased=%u streaming=%u paused=%u; no RF coverage claim\n",
            c->mode,(unsigned)c->frequency_hz,c->gain_tenths_db,c->squelch_tenths,c->calls_fm_enabled,c->fm_active,c->plan_active,
            c->endpoint_known?c->endpoint.endpoint_id:"unknown",c->endpoint_known?c->endpoint.owner:"unknown",
            c->endpoint_known&&c->endpoint.present,c->endpoint_known&&c->endpoint.leased,
            c->endpoint_known&&c->endpoint.streaming,c->endpoint_known&&c->endpoint.paused);
        if(ok)LS_PRINTF(fmt_ls_rx_plan_console_entry_saved_or_default, "entry_saved_or_default_known=%u LISTEN_hz=%u gain_tenths_db=%d CALLS_FM_saved_known=%u saved_enabled=%u archive_ready=%u archive_errors=%u; persistence async/unconfirmed\n",
            c->entry_values_known,(unsigned)c->entry_listen_hz,c->entry_gain_tenths_db,c->calls_saved_known,
            c->calls_saved_enabled,c->calls_ready,(unsigned)c->archive_errors);
        FREE(c);return ok?0:1;
    }
    if(argc==3&&!strcmp(argv[1],"record-fm")&&(!strcmp(argv[2],"on")||!strcmp(argv[2],"off"))) {
        bool ok=fm_rx_plan_record_fm(!strcmp(argv[2],"on"));
        puts(ok?"CALLS_FM runtime verified; persistence queued/unconfirmed; check context saved flag/archive_errors"
            :"CALLS_FM refused: need parked FM, no active plan/archive job, and initialized archive policy");
        return ok?0:1;
    }
    if(argc>=5&&argc<=12&&!strcmp(argv[1],"load")) {
        ls_rx_plan_step_t steps[LS_RX_PLAN_STEPS];uint32_t repeats;
        if(!decimal(argv[3],argv[3]+strlen(argv[3]),&repeats))return 1;
        for(int i=4;i<argc;++i) {
            const char *colon=strchr(argv[i],':');
            if(!colon||!decimal(argv[i],colon,&steps[i-4].center_hz)||
               !decimal(colon+1,argv[i]+strlen(argv[i]),&steps[i-4].dwell_ms))return 1;
        }
        bool ok=fm_rx_plan_load(argv[2],steps,(size_t)argc-4,repeats);
        puts(ok?"RX plan loaded; no receiver started":"RX plan rejected: active, invalid name/steps/budget, or PSRAM unavailable");return ok?0:1;
    }
    if(argc==2&&!strcmp(argv[1],"start")) {
        bool ok=fm_rx_plan_start();
        puts(ok?"RX plan queued; automatic receive-only retunes; auto-CALLS suppressed during run"
            :"RX plan rejected: need active RTL USB FM LISTEN, stable owned context, supported frequencies, no scanner/lock/record job/coverage session");return ok?0:1;
    }
    if(argc==2&&!strcmp(argv[1],"stop")) {
        bool ok=fm_rx_plan_stop();puts(ok?"RX plan stop queued; restore only while original ownership/context remains valid":"no active RX plan");return ok?0:1;
    }
    if(argc==2&&(!strcmp(argv[1],"json")||!strcmp(argv[1],"show")))return ls_rx_plan_export(output,NULL)?0:1;
    LS_PUTS(msg_ls_rx_plan_console_usage_rx_plan_load, "usage: rx-plan load <name> <repeats1..4> <hz:dwell_ms>... | show | start | stop | json | context | record-fm on|off\n"
         "Already-running RTL USB FM LISTEN only; max8 steps, dwell1000..30000ms, total120s including guard budgets. No TX, automatic app start, recording or NVS writes.");return 1;
}
