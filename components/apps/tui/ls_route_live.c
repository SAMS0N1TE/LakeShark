#include "ls_route_live.h"
#include "ls_gps.h"
#include "ls_haptic.h"
#include "ls_motion.h"
#include "ls_notify.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

EXT_RAM_BSS_ATTR static ls_route_point_t s_points[LS_ROUTE_CAP];
EXT_RAM_BSS_ATTR static ls_gps_state_t s_gps;
static ls_route_t s_route;
static bool s_init, s_haptics=true;
static int64_t s_fix;

const ls_route_t *ls_route_live(void)
{
    if (!s_init) { ls_route_init(&s_route,s_points,LS_ROUTE_CAP); s_init=true; }
    return &s_route;
}
bool ls_route_live_load(const char *path, bool backtrack)
{
    ls_route_live();
    if (!backtrack && !path) return false;
    FILE *f=fopen(backtrack ? "/sdcard/lakeshark/track.log" : path,"rb");
    if (!f) return false;
    bool ok=backtrack ? ls_route_track(&s_route,f) : ls_route_gpx(&s_route,f);
    fclose(f);
    s_fix=0;
    if (ok && ls_gps_start()!=ESP_OK) {
        ls_route_clear(&s_route); return false;
    }
    return ok;
}
void ls_route_live_poll(void)
{
    if (!s_route.valid) return;
    ls_gps_get(&s_gps);
    int64_t now=esp_timer_get_time();
    bool fresh=s_gps.fix && s_gps.last_fix_us>0 && now>=s_gps.last_fix_us &&
        now-s_gps.last_fix_us<=10000000;
    if (fresh && s_gps.last_fix_us==s_fix) return;
    s_fix=fresh ? s_gps.last_fix_us : 0;
    ls_route_event_t event=ls_route_update(&s_route,fresh,s_gps.lat_deg,s_gps.lon_deg);
    if (event==LS_ROUTE_NONE) return;
    const bool arrived=event==LS_ROUTE_ARRIVE, off=event==LS_ROUTE_OFF;
    ls_notice_t n={ .screen=-1, .accent=off ? TUI_YELLOW : TUI_CYAN };
    snprintf(n.title,sizeof(n.title),"ROUTE");
    snprintf(n.body,sizeof(n.body),"%s",arrived ? "Arrived at route end" : off ?
        "Off route; follow the bearing back to the route" : "Back on route");
    /* The route owns its haptic choice; the banner has no audio side effect. */
    ls_notify_post_quiet(&n);
    if (s_haptics) ls_haptic_play(off ? LS_HAPTIC_WARN : LS_HAPTIC_CONFIRM);
}
static double route_num(const ls_opt_t *o)
{
    ls_route_live(); return o->arg ? s_route.arrival_m : s_route.off_m;
}
static void route_set_num(const ls_opt_t *o, double v)
{
    if (o->arg) s_route.arrival_m=v; else s_route.off_m=v;
    s_route.outside=s_route.inside=s_route.near=0;
}
static int haptic_get(const ls_opt_t *o) { (void)o; return s_haptics; }
static void haptic_set(const ls_opt_t *o,int v) { (void)o; s_haptics=v!=0; }
static int reverse_get(const ls_opt_t *o) { (void)o; return s_route.reversed; }
static void reverse_set(const ls_opt_t *o,int v)
{
    (void)o;
    if (s_route.reversed!=(v!=0)) { ls_route_reverse(&s_route); s_fix=0; }
}
void ls_route_live_clear(void) { ls_route_live(); ls_route_clear(&s_route); s_fix=0; }
static void route_clear(const ls_opt_t *o) { (void)o; ls_route_live_clear(); }
static const char *route_required(const ls_opt_t *o) { (void)o; return s_route.valid ? NULL : "load a route first"; }
static const ls_opt_t GUIDE_ROWS[]={
    { .label="Off-route distance", .kind=LS_OPT_LEVEL, .num=route_num, .set_num=route_set_num,
      .lo=5, .hi=500, .step=5, .unit="m" },
    { .label="Arrival radius", .kind=LS_OPT_LEVEL, .arg=1, .num=route_num, .set_num=route_set_num,
      .lo=1, .hi=100, .step=1, .unit="m" },
    { .label="Haptics", .kind=LS_OPT_TOGGLE, .get=haptic_get, .set=haptic_set },
};
static const ls_opt_ctx_t GUIDE={ .name="GUIDANCE", .job=-1, .radio=LS_RSEL_NONE, LS_OPT_ROWS(GUIDE_ROWS) };
static const char *const DIRECTIONS[]={ "forward", "reverse" };
static const ls_opt_t ROUTE_ROWS[]={
    { .label="Guidance", .kind=LS_OPT_MENU, .sub=&GUIDE },
    { .label="Reverse route", .kind=LS_OPT_TOGGLE, .names=DIRECTIONS, .get=reverse_get, .set=reverse_set, .why_not=route_required },
    { .label="Clear route", .kind=LS_OPT_ACTION, .act=route_clear, .why_not=route_required },
};
const ls_opt_ctx_t LS_ROUTE_OPTIONS={ .name="ROUTE", .job=-1, .radio=LS_RSEL_NONE, LS_OPT_ROWS(ROUTE_ROWS) };
const ls_opt_ctx_t *ls_route_options(void) { ls_route_live(); return &LS_ROUTE_OPTIONS; }
void ls_route_guidance(char *out, size_t cap)
{
    const ls_route_t *r=ls_route_live();
    if (!r->valid) { if (cap) out[0]=0; return; }
    if (r->arrived) { snprintf(out,cap,"ROUTE  ARRIVED  [O] options"); return; }
    if (!r->located) { snprintf(out,cap,"%c ROUTE  WAIT GPS  [O] options",ls_motion_pip(true)); return; }
    snprintf(out,cap,"%c %s %03d  %.0fm left  XTE %.0fm",ls_motion_pip(true),
        r->off_route ? "OFF ROUTE" : "ROUTE", (int)lround(r->bearing)%360, r->remaining_m,r->cross_m);
}
