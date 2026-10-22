#include "ls_test.h"
#include "../../components/apps/tui/ls_detector.c"
static unsigned deliveries;
static ls_detector_event_t delivered;
void ls_sweep_init(void) {}
void ls_sweep_wake(void) {}
const char *ls_sweep_category(unsigned c) {static const char *const names[]={"CAMERA","BODYCAM","DRONE","TRACKER","ATTACK"};return c<5?names[c]:"UNKNOWN";}
const char *ls_detector_air_source(void) {return NULL;}
void ls_detector_air_gps(ls_detector_air_gps_t *o,int64_t t) {(void)t;memset(o,0,sizeof(*o));}
bool ls_detector_air_read(int slot,int64_t t,const ls_detector_air_gps_t *g,ls_detector_air_t *o) {(void)slot;(void)t;(void)g;(void)o;return false;}
bool ls_detector_air_match(const ls_detector_rule_t *r,const ls_detector_air_t *a) {(void)r;(void)a;return false;}
const char *ls_detector_air_category_name(unsigned i) {(void)i;return "rotorcraft";}
uint8_t ls_detector_deliver(const ls_detector_event_t *e,uint8_t mask) {deliveries++;delivered=*e;return mask;}
void ls_detector_log_begin(void) {}
bool ls_detector_log_end(void) {return true;}
bool ls_detector_write(const ls_detector_event_t *e,uint64_t boot) {(void)e;(void)boot;return true;}
LS_CASE(watch_default_rule_is_independent_bounded_and_uses_detector_rate_limit) {
    ls_detector_init();ls_detector_start(false);reload_requested=false;
    ls_shim_time_set(10000000);LS_CHECK(ls_detector_watch_notice(7,-60,true,900,false,10000000));
    ls_detector_tick();LS_EQ_INT(deliveries,1);LS_EQ_INT(delivered.source,DET_DRONE_WATCH);
    LS_EQ_STR(delivered.reason,"drone-link-nearby");LS_CHECK(!delivered.relative_rssi);
    LS_CHECK(!ls_detector_running());
    for(unsigned i=0;i<8;i++)LS_CHECK(ls_detector_watch_notice(20+i,-30,false,5800,false,10001000));
    LS_CHECK(!ls_detector_watch_notice(99,-30,false,5800,false,10001000));
    ls_detector_tick();LS_EQ_INT(deliveries,1); /* shared rule cooldown */
    ls_shim_time_set(13000000);LS_CHECK(ls_detector_watch_notice(7,-50,true,900,true,13000000));
    ls_detector_tick();LS_EQ_INT(deliveries,2);LS_CHECK(strstr(delivered.reason,"approaching"));
    const char *off="{\"version\":1,\"rules\":[{\"id\":\"drone-link-nearby\",\"enabled\":false,\"sources\":[\"drone.watch\"],\"match\":{\"category_in\":[\"DRONE\"]},\"sinks\":[\"banner\"]}]}";
    LS_CHECK(ls_detector_parse(off,strlen(off)));ls_shim_time_set(20000000);
    LS_CHECK(ls_detector_watch_notice(44,-60,true,900,false,20000000));ls_detector_tick();LS_EQ_INT(deliveries,2);
}
