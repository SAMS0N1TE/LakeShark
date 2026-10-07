#include "ls_test.h"
#include "ls_route_live.h"
#include "ls_gps.h"
#include "ls_haptic.h"
#include "ls_notify.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static ls_gps_state_t gps;
static int64_t now;
static int banners,buzzes;
static ls_haptic_effect_t effect;
void ls_gps_get(ls_gps_state_t *out) { *out=gps; }
esp_err_t ls_gps_start(void) { return ESP_OK; }
int64_t __wrap_esp_timer_get_time(void) { return now; }
bool ls_haptic_play(ls_haptic_effect_t e) { buzzes++;effect=e;return true; }
void ls_notify_post_quiet(const ls_notice_t *n) { LS_CHECK(n->accent!=0);banners++; }
/* An accidental audible post fails the test even with notification ringing on. */
void ls_notify_post(const ls_notice_t *n) { (void)n;LS_CHECK(false); }
char ls_motion_pip(bool live) { return live ? '>' : ' '; }
static void load(void)
{
    const char *path="bench/build/route-live-test.gpx";
    FILE *f=fopen(path,"wb");LS_CHECK(f!=NULL);
    fputs("<gpx><rte><rtept lat='0' lon='0'/><rtept lat='0' lon='.01'/></rte></gpx>",f);fclose(f);
    LS_CHECK(ls_route_live_load(path,false));remove(path);
    memset(&gps,0,sizeof(gps));gps.fix=true;gps.lon_deg=.005;gps.lat_deg=.001;
    now=1000000;gps.last_fix_us=now;banners=buzzes=0;
}
static void fix(void) { now+=1000000;gps.last_fix_us=now;ls_route_live_poll(); }
LS_CASE(shared_route_context_keeps_the_same_menu_and_defaults)
{
    LS_CHECK(ls_route_options() == &LS_ROUTE_OPTIONS);
    LS_EQ_STR(LS_ROUTE_OPTIONS.name, "ROUTE");
    LS_EQ_INT(LS_ROUTE_OPTIONS.n, 3);
    const ls_opt_t *guidance = &LS_ROUTE_OPTIONS.opt[0];
    LS_EQ_INT(guidance->kind, LS_OPT_MENU);
    LS_EQ_STR(guidance->sub->name, "GUIDANCE");
    LS_EQ_INT(guidance->sub->n, 3);
}
LS_CASE(route_poll_does_not_count_repeated_frames_or_stale_fixes)
{
    load();ls_route_live_poll();
    for(int i=0;i<100;i++) ls_route_live_poll();
    LS_EQ_INT(0,banners);LS_EQ_INT(0,buzzes);
    fix();LS_EQ_INT(0,banners);fix();LS_EQ_INT(1,banners);LS_EQ_INT(1,buzzes);LS_EQ_INT(LS_HAPTIC_WARN,effect);
    now+=10000001;ls_route_live_poll();LS_CHECK(!ls_route_live()->located);LS_CHECK(isnan(ls_route_live()->bearing));
    gps.lat_deg=0;fix();fix();LS_EQ_INT(2,banners);
    gps.lon_deg=.01;fix();fix();LS_EQ_INT(3,banners);LS_EQ_INT(LS_HAPTIC_CONFIRM,effect);
    for(int i=0;i<5;i++) fix();
    LS_EQ_INT(3,banners);
}
LS_CASE(layered_options_step_distances_disable_haptics_reverse_and_clear)
{
    load();
    const ls_opt_ctx_t *options=ls_route_options();LS_EQ_INT(LS_OPT_MENU,options->opt[0].kind);
    const ls_opt_ctx_t *guide=options->opt[0].sub;
    const ls_opt_t *off=&guide->opt[0],*arrival=&guide->opt[1],*haptic=&guide->opt[2];
    LS_EQ_INT(LS_OPT_LEVEL,off->kind);LS_EQ_INT(LS_OPT_LEVEL,arrival->kind);
    off->set_num(off,45);arrival->set_num(arrival,20);LS_NEAR(45,off->num(off),0);LS_NEAR(20,arrival->num(arrival),0);
    haptic->set(haptic,0);fix();fix();fix();LS_EQ_INT(1,banners);LS_EQ_INT(0,buzzes);
    const ls_opt_t *reverse=&options->opt[1];reverse->set(reverse,1);
    LS_CHECK(ls_route_live()->reversed);LS_NEAR(.01,ls_route_live()->point[0].lon,1e-9);
    char text[96];ls_route_guidance(text,sizeof(text));LS_CHECK(strstr(text,"WAIT GPS")!=NULL);
    options->opt[2].act(&options->opt[2]);LS_CHECK(!ls_route_live()->valid);
    ls_route_guidance(text,sizeof(text));LS_EQ_STR("",text);
    haptic->set(haptic,1);
}
