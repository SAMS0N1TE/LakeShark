#include "ls_test.h"
#include "ls_rx_plan_console.h"
#include "rx_plan.h"
#include "fm_state.h"
#include <stdlib.h>
#include <string.h>
static ls_rx_plan_t plan;
static char text[16384];static size_t used,lines;static bool fail_alloc,refuse;
void *__real_calloc(size_t,size_t);
void *__wrap_calloc(size_t n,size_t size){return fail_alloc?NULL:__real_calloc(n,size);}
bool fm_rx_plan_load(const char *name,const ls_rx_plan_step_t *s,size_t n,uint32_t repeats)
{return ls_rx_plan_load(&plan,name,s,n,repeats);}
bool fm_rx_plan_start(void){return ls_rx_plan_start(&plan,162550000,100);}
bool fm_rx_plan_stop(void){return ls_rx_plan_stop(&plan);}
bool fm_rx_plan_snapshot(ls_rx_plan_status_t *s){*s=plan.status;return plan.status.plan_id!=0;}
bool fm_rx_plan_context(fm_rx_plan_context_t *out){memset(out,0,sizeof(*out));return true;}
bool fm_rx_plan_record_fm(bool enabled){(void)enabled;return !plan.status.active;}
static bool collect(void *ctx,const char *line,size_t n)
{
    (void)ctx;LS_CHECK(n<=1536 && n>1 && line[n-1]=='\n');if(refuse)return false;
    LS_CHECK(used+n<sizeof(text));memcpy(text+used,line,n);used+=n;text[used]=0;lines++;return true;
}
static void reset(void){memset(&plan,0,sizeof(plan));used=lines=0;text[0]=0;fail_alloc=refuse=false;}
LS_CASE(rx_plan_console_validates_bounded_exact_steps_and_no_implicit_start)
{
    reset();char *a[]={"rx-plan","load","field \"A\"","2","162550000:1000","162400000:2000"};
    LS_CHECK(ls_rx_plan_command(6,a)==0 && plan.status.state==LS_RX_PLAN_LOADED && !plan.status.active);
    LS_CHECK(ls_rx_plan_export(collect,NULL) && lines==3);
    LS_CHECK(strstr(text,"\"name\":\"field \\\"A\\\"\"") && strstr(text,"\"started_us\":null"));
    LS_CHECK(strstr(text,"\"planned_center_hz\":\"162400000\",\"planned_dwell_ms\":\"2000\""));
    char *bad[]={"rx-plan","load","bad","1","162550000:1000:2"};
    LS_CHECK(ls_rx_plan_command(5,bad)==1 && !strcmp(plan.status.name,"field \"A\""));
    bad[4]="-162550000:1000";LS_CHECK(ls_rx_plan_command(5,bad)==1);
    bad[4]="4294967296:1000";LS_CHECK(ls_rx_plan_command(5,bad)==1);
}
LS_CASE(rx_plan_receipt_large_integer_names_and_max_steps_fit_1024_byte_lines)
{
    reset();ls_rx_plan_step_t steps[8];for(unsigned i=0;i<8;++i)steps[i]=(ls_rx_plan_step_t){162550000,1000};
    LS_CHECK(fm_rx_plan_load("\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"",steps,8,1));
    LS_CHECK(ls_rx_plan_start(&plan,162550000,UINT64_MAX-LS_RX_PLAN_MAX_US));
    LS_CHECK(ls_rx_plan_export(collect,NULL) && lines==9);
    LS_CHECK(strstr(text,"\"schema\":3,\"type\":\"rx_plan_session\""));
    LS_CHECK(strstr(text,"\"rf_coverage_us\":null,\"sample_continuity\":null,\"sample_frequency_verified\":false"));
    LS_CHECK(strstr(text,"18446744073589551615"));
}
LS_CASE(rx_plan_receipt_failure_does_not_fallback_or_keep_writing)
{
    reset();LS_CHECK(!ls_rx_plan_export(collect,NULL) && lines==1 && strstr(text,"plan_unavailable"));
    reset();fail_alloc=true;LS_CHECK(!ls_rx_plan_export(collect,NULL) && lines==1 && strstr(text,"psram_unavailable"));
    reset();ls_rx_plan_step_t step={162550000,1000};LS_CHECK(fm_rx_plan_load("x",&step,1,1));
    refuse=true;LS_CHECK(!ls_rx_plan_export(collect,NULL) && !lines);
    LS_CHECK(!ls_rx_plan_export(NULL,NULL));
}
