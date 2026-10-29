/* LS_TEST_SOURCES: ${FW}/components/apps/tui/ls_coverage_export.c ${FW}/components/lakeshark/radio/iq_coverage.c ${FW}/components/lakeshark/radio/iq_observation.c */
#include "ls_test.h"
#include "ls_coverage_export.h"
#include "fm_state.h"
#include <stdlib.h>
#include <string.h>

static ls_iq_coverage_status_t status;
static ls_iq_coverage_interval_t entries[LS_IQ_COVERAGE_CAPACITY];
static size_t count, snapshot_calls;
static bool available, allocation_failed;
typedef struct { char text[LS_COVERAGE_RECEIPT_BYTES+1];size_t used,lines;bool refuse,nested; } output_t;
static output_t output,inner;
void *__real_calloc(size_t,size_t);
void *__wrap_calloc(size_t n,size_t bytes)
{return allocation_failed?NULL:__real_calloc(n,bytes);}
bool fm_coverage_start(void){return available;}
bool fm_coverage_stop(void){return available;}
bool fm_coverage_snapshot(ls_iq_coverage_status_t *s,ls_iq_coverage_interval_t *e,size_t cap,size_t *n)
{
    snapshot_calls++;
    if(!available)return false;
    *s=status;
    if(n)*n=count;
    for(size_t i=0;i<count&&i<cap;i++)e[i]=entries[i];
    return true;
}
static bool collect(void *context,const char *line,size_t n)
{
    output_t *o=context;
    LS_CHECK(n>0&&n<=LS_COVERAGE_LINE_BYTES&&line[0]=='{'&&line[n-1]=='\n');
    LS_CHECK(o->used+n<sizeof(o->text));
    if(o->refuse)return false;
    memcpy(o->text+o->used,line,n);o->used+=n;o->text[o->used]=0;o->lines++;
    if(o->nested&&o->lines==1){inner=(output_t){0};LS_CHECK(ls_coverage_export(collect,&inner));}
    return true;
}
static void has(const char *part){LS_CHECK_MSG(strstr(output.text,part)!=NULL,"missing '%s'",part);}
static void fixture(void)
{
    status=(ls_iq_coverage_status_t){.session_id=1,.started_us=UINT64_C(9007199254740993),
        .stopped_us=UINT64_C(9007199254741993),.last_update_us=UINT64_C(9007199254741993),
        .retained_intervals=1,.delivered_bytes=480,.delivered_blocks=1};
    status.state_us[LS_IQ_COVERAGE_RECEIVING]=1000;
    memset(entries,0,sizeof(entries));
    entries[0]=(ls_iq_coverage_interval_t){.sequence=1,.started_us=status.started_us,
        .ended_us=status.stopped_us,.state=LS_IQ_COVERAGE_RECEIVING,.context_known=true,.calls_suppressed=true,
        .context_epoch=3,.stream_epoch=4,.configured_center_hz=162550000,.nominal_rate_hz=240000,
        .delivered_bytes=480,.delivered_blocks=1,.plan_id=7,.plan_step=2,.plan_repeat=1};
    count=1;snapshot_calls=0;available=true;allocation_failed=false;output=(output_t){0};
}
LS_CASE(coverage_receipt_preserves_software_windows_native_counts_and_unknown_rf)
{
    fixture();LS_CHECK(ls_coverage_export(collect,&output));LS_EQ_UINT(output.lines,2);
    has("\"schema\":4,\"type\":\"coverage_session\"");
    has("\"scope\":\"software_observed_rtl_fm\"");
    has("\"calls_suppressed\":true");has("\"calls_suppressed\":false");
    has("\"receiving_us\":\"1000\"");has("\"started_us\":\"9007199254740993\"");
    has("\"nominal_sample_duration_us\":\"1000\"");
    has("\"plan_id\":\"7\",\"plan_step\":\"2\",\"plan_repeat\":\"1\"");
    has("\"physical_rf_coverage_us\":null");has("\"hardware_loss_bytes\":null");
    has("\"sample_continuity\":null");has("\"per_sample_rf_frequency_hz\":null");
    has("\"software_loss_scope\":\"observed_transport_counter_deltas_only\"");
    LS_CHECK(ls_coverage_export_workspace_bytes()<=8192);
}
LS_CASE(coverage_receipt_unknown_context_clock_and_saturation_do_not_become_zero)
{
    fixture();status.active=true;status.timing_invalid=true;status.counter_saturated=true;
    entries[0].context_known=false;entries[0].plan_id=0;
    LS_CHECK(ls_coverage_export(collect,&output));
    has("\"stopped_us\":null");has("\"receiving_us\":null");has("\"delivered_bytes\":null");
    has("\"context_epoch\":null,\"stream_epoch\":null,\"configured_center_hz\":null,\"nominal_rate_hz\":null");
    has("\"nominal_sample_duration_us\":null");has("\"plan_id\":null,\"plan_step\":null,\"plan_repeat\":null");
    fixture();entries[0].plan_step=entries[0].plan_repeat=UINT32_MAX;
    LS_CHECK(ls_coverage_export(collect,&output));
    has("\"plan_id\":\"7\",\"plan_step\":null,\"plan_repeat\":null");
}
LS_CASE(coverage_receipt_maximum_native_values_remain_bounded_and_reentrant)
{
    fixture();count=status.retained_intervals=LS_IQ_COVERAGE_CAPACITY;
    status.started_us=status.stopped_us=status.last_update_us=UINT64_MAX;
    status.overwritten_intervals=UINT64_MAX;
    for(unsigned i=0;i<LS_IQ_COVERAGE_CAPACITY;i++){
        entries[i]=entries[0];entries[i].sequence=UINT64_MAX;
        entries[i].started_us=entries[i].ended_us=UINT64_MAX;
        entries[i].context_epoch=entries[i].stream_epoch=UINT64_MAX;
        entries[i].configured_center_hz=UINT64_MAX;
        entries[i].delivered_bytes=entries[i].delivered_blocks=entries[i].settling_discarded_bytes=UINT64_MAX;
    }
    output.nested=true;LS_CHECK(ls_coverage_export(collect,&output));
    LS_EQ_UINT(output.lines,LS_COVERAGE_RECORD_LIMIT);
    LS_CHECK(output.used<=LS_COVERAGE_RECEIPT_BYTES&&inner.used==output.used);
    LS_CHECK(!strcmp(output.text,inner.text));has("18446744073709551615");
}
LS_CASE(coverage_receipt_unavailable_allocation_output_and_snapshot_bounds_fail_explicitly)
{
    fixture();available=false;LS_CHECK(!ls_coverage_export(collect,&output));has("coverage_snapshot_unavailable");
    fixture();allocation_failed=true;LS_CHECK(!ls_coverage_export(collect,&output));allocation_failed=false;
    has("psram_unavailable");LS_EQ_UINT(snapshot_calls,0);
    fixture();count=LS_IQ_COVERAGE_CAPACITY+1;LS_CHECK(!ls_coverage_export(collect,&output));
    has("coverage_snapshot_unavailable");
    fixture();output.refuse=true;LS_CHECK(!ls_coverage_export(collect,&output));LS_EQ_UINT(output.lines,0);
    LS_CHECK(!ls_coverage_export(NULL,NULL));
    char *bad[]={"rx-coverage","invalid"};LS_EQ_INT(ls_coverage_command(2,bad),1);
}
