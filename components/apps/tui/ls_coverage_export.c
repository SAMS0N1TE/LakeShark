/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ls_coverage_export.h"
#include "fm_state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ls_textpack.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define ALLOC(n) heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define FREE(p) heap_caps_free(p)
#else
#define ALLOC(n) calloc(1, n)
#define FREE(p) free(p)
#endif

typedef struct {
    ls_iq_coverage_status_t status;
    ls_iq_coverage_interval_t entries[LS_IQ_COVERAGE_CAPACITY];
    char line[LS_COVERAGE_LINE_BYTES];
    size_t used, emitted_bytes;
    bool failed;
} workspace_t;
_Static_assert(sizeof(workspace_t) <= 8192, "coverage export PSRAM budget");

static void raw(workspace_t *w, const char *value)
{
    size_t n = strlen(value);
    if (n >= sizeof(w->line)-w->used) { w->failed = true; return; }
    memcpy(w->line+w->used,value,n); w->used += n; w->line[w->used] = 0;
}
static void key(workspace_t *w, const char *name)
{ raw(w,",\""); raw(w,name); raw(w,"\":"); }
static void flag(workspace_t *w,const char *name,bool value)
{ key(w,name); raw(w,value ? "true" : "false"); }
static void count(workspace_t *w,const char *name,bool known,uint64_t value)
{
    key(w,name);
    if (!known) { raw(w,"null"); return; }
    char digits[21]; size_t n=0;
    do { digits[n++]=(char)('0'+value%10u); value/=10u; } while(value);
    raw(w,"\"");
    while(n) { char one[2]={digits[--n],0}; raw(w,one); }
    raw(w,"\"");
}
static void begin(workspace_t *w,const char *kind)
{
    w->used=0;w->failed=false;
    raw(w,LS_TEXT(t_ls_coverage_export_schema_4_type, "{\"schema\":4,\"type\":\""));raw(w,kind);raw(w,"\"");
}
static void unknowns(workspace_t *w)
{
    raw(w,LS_TEXT(t_ls_coverage_export_physical_rf_coverage_us, ",\"physical_rf_coverage_us\":null,\"hardware_loss_bytes\":null,\"sample_continuity\":null,\"device_timestamps\":null,\"per_sample_rf_frequency_hz\":null,\"sample_frequency_verified\":false"));
}
static bool error(ls_coverage_write_t write,void *context,const char *reason)
{
    /* Only fixed internal reasons are passed here. */
    char line[160];
    int n=LS_SNPRINTF(fmt_ls_coverage_export_schema_4_type_error, line,sizeof(line),"{\"schema\":4,\"type\":\"error\",\"reason\":\"%s\",\"partial\":true}\n",reason);
    if (n>0 && (size_t)n<sizeof(line)) (void)write(context,line,(size_t)n);
    return false;
}
static bool emit(workspace_t *w,ls_coverage_write_t write,void *context)
{
    raw(w,"}\n");
    if(w->failed || w->used>LS_COVERAGE_RECEIPT_BYTES-w->emitted_bytes) {
        if(LS_COVERAGE_RECEIPT_BYTES-w->emitted_bytes>=160u)
            (void)error(write,context,"record_bounds");
        return false;
    }
    w->emitted_bytes+=w->used;
    return write(context,w->line,w->used);
}
size_t ls_coverage_export_workspace_bytes(void) { return sizeof(workspace_t); }

bool ls_coverage_export(ls_coverage_write_t write,void *context)
{
    if(!write)return false;
    workspace_t *w=ALLOC(sizeof(*w));
    if(!w)return error(write,context,"psram_unavailable");
    size_t n=0;
    if(!fm_coverage_snapshot(&w->status,w->entries,LS_IQ_COVERAGE_CAPACITY,&n) ||
       n>LS_IQ_COVERAGE_CAPACITY || w->status.retained_intervals!=n) {
        FREE(w);return error(write,context,LS_TEXT(t_ls_coverage_export_coverage_snapshot_unavailable, "coverage_snapshot_unavailable"));
    }
    const ls_iq_coverage_status_t *s=&w->status;
    bool counters=!s->counter_saturated;
    bool timing=counters&&!s->timing_invalid;
    begin(w,"coverage_session");
    raw(w,LS_TEXT(t_ls_coverage_export_scope_software_observed_rtl, ",\"scope\":\"software_observed_rtl_fm\",\"time_scope\":\"current_boot_only\""));
    count(w,"session_id",true,s->session_id);flag(w,"active",s->active);
    flag(w,"calls_suppressed",s->calls_suppressed);
    count(w,"started_us",true,s->started_us);count(w,"stopped_us",!s->active,s->stopped_us);
    count(w,"last_update_us",true,s->last_update_us);
    flag(w,"timing_invalid",s->timing_invalid);flag(w,"counter_saturated",s->counter_saturated);
    count(w,"unknown_us",timing,s->state_us[LS_IQ_COVERAGE_UNKNOWN]);
    count(w,"receiving_us",timing,s->state_us[LS_IQ_COVERAGE_RECEIVING]);
    count(w,"settling_us",timing,s->state_us[LS_IQ_COVERAGE_SETTLING]);
    count(w,"busy_us",timing,s->state_us[LS_IQ_COVERAGE_BUSY]);
    count(w,"unavailable_us",timing,s->state_us[LS_IQ_COVERAGE_UNAVAILABLE]);
    count(w,"delivered_bytes",counters,s->delivered_bytes);count(w,"delivered_blocks",counters,s->delivered_blocks);
    count(w,LS_TEXT(t_ls_coverage_export_settling_discarded_bytes, "settling_discarded_bytes"),counters,s->settling_discarded_bytes);
    count(w,"ring_rejected_bytes",counters,s->ring_rejected_bytes);
    count(w,LS_TEXT(t_ls_coverage_export_transfer_error_events, "transfer_error_events"),counters,s->transfer_error_events);
    count(w,LS_TEXT(t_ls_coverage_export_submission_error_events, "submission_error_events"),counters,s->submission_error_events);
    count(w,LS_TEXT(t_ls_coverage_export_flush_discarded_bytes, "flush_discarded_bytes"),counters,s->flush_discarded_bytes);
    count(w,LS_TEXT(t_ls_coverage_export_provenance_rejected_blocks, "provenance_rejected_blocks"),counters,s->provenance_rejected_blocks);
    count(w,"retained_intervals",true,n);count(w,LS_TEXT(t_ls_coverage_export_overwritten_intervals, "overwritten_intervals"),true,s->overwritten_intervals);
    count(w,"settling_guard_us",true,LS_IQ_COVERAGE_GUARD_US);
    count(w,LS_TEXT(t_ls_coverage_export_settling_guard_blocks, "settling_guard_blocks"),true,LS_IQ_COVERAGE_GUARD_BLOCKS);
    raw(w,LS_TEXT(t_ls_coverage_export_settling_policy_software_policy, ",\"settling_policy\":\"software_policy_not_calibrated\",\"software_loss_scope\":\"observed_transport_counter_deltas_only\",\"boot_id\":null"));unknowns(w);
    if(!emit(w,write,context)) {FREE(w);return false;}
    for(size_t i=0;i<n;i++) {
        const ls_iq_coverage_interval_t *e=&w->entries[i];
        begin(w,"coverage_interval");
        count(w,"session_id",true,s->session_id);count(w,"sequence",true,e->sequence);
        count(w,"started_us",true,e->started_us);count(w,"ended_us",true,e->ended_us);
        raw(w,",\"state\":\"");raw(w,ls_iq_coverage_state_name(e->state));raw(w,"\"");
        flag(w,"context_known",e->context_known);
        flag(w,"calls_suppressed",e->calls_suppressed);
        count(w,"context_epoch",e->context_known,e->context_epoch);
        count(w,"stream_epoch",e->context_known,e->stream_epoch);
        count(w,LS_TEXT(t_ls_coverage_export_configured_center_hz, "configured_center_hz"),e->context_known,e->configured_center_hz);
        count(w,"nominal_rate_hz",e->context_known,e->nominal_rate_hz);
        count(w,"delivered_bytes",counters,e->delivered_bytes);count(w,"delivered_blocks",counters,e->delivered_blocks);
        count(w,LS_TEXT(t_ls_coverage_export_settling_discarded_bytes, "settling_discarded_bytes"),counters,e->settling_discarded_bytes);
        ls_iq_observation_t nominal={.known=e->context_known&&counters,
            .nominal_rate_hz=e->nominal_rate_hz,.delivered_bytes=e->delivered_bytes};
        uint64_t duration=0;bool nominal_known=ls_iq_observation_nominal_us(&nominal,&duration);
        count(w,LS_TEXT(t_ls_coverage_export_nominal_sample_duration_us, "nominal_sample_duration_us"),nominal_known,duration);
        count(w,"plan_id",e->plan_id!=0,e->plan_id);
        count(w,"plan_step",e->plan_id!=0&&e->plan_step<LS_RX_PLAN_STEPS,e->plan_step);
        count(w,"plan_repeat",e->plan_id!=0&&e->plan_repeat<LS_RX_PLAN_REPEATS,e->plan_repeat);
        unknowns(w);
        if(!emit(w,write,context)){FREE(w);return false;}
    }
    FREE(w);return true;
}
static bool console_write(void *context,const char *line,size_t n)
{(void)context;return fwrite(line,1,n,stdout)==n;}
int ls_coverage_command(int argc,char **argv)
{
    const char *action=argc==1 ? "status" : argc==2 ? argv[1] : "";
    if(!strcmp(action,"json"))return ls_coverage_export(console_write,NULL)?0:1;
    if(!strcmp(action,"start")) {bool ok=fm_coverage_start();puts(ok?"software coverage tracking started; physical RF continuity unknown":"coverage start unavailable, active or PSRAM unavailable");return ok?0:1;}
    if(!strcmp(action,"stop")) {bool ok=fm_coverage_stop();puts(ok?"software coverage tracking stopped; receipt retained":"coverage stop unavailable or inactive");return ok?0:1;}
    if(!strcmp(action,"status")) {
        workspace_t *w=ALLOC(sizeof(*w));
        if(!w){LS_PUTS(t_ls_coverage_export_coverage_status_psram_unavailable, "coverage status PSRAM unavailable");return 1;}
        bool ok=fm_coverage_snapshot(&w->status,NULL,0,NULL);
        puts(ok ? (w->status.active?"software coverage tracking active; use rx-coverage json":"software coverage tracking stopped; use rx-coverage json") : "no software coverage session available");
        FREE(w);return ok?0:1;
    }
    LS_PUTS(msg_ls_coverage_export_usage_rx_coverage_start, "usage: rx-coverage start|stop|status|json (software state; no RF continuity claim)");return 1;
}
