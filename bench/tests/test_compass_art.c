/* Exercise the production cache against an uncached render, and count the
   expensive trig calls instead of inferring savings from desktop timing. */
#include "ls_test.h"
#include <time.h>
static unsigned trig_calls;
static float counted_sin(float x) { trig_calls++; return sinf(x); }
static float counted_cos(float x) { trig_calls++; return cosf(x); }
static float counted_atan(float x, float y) { trig_calls++; return atan2f(x,y); }
#define sinf counted_sin
#define cosf counted_cos
#define atan2f counted_atan
#include "../../components/apps/tui/ls_compass_art.c"
#undef sinf
#undef cosf
#undef atan2f
static int font_w=10, font_h=17;
void ls_tui_geometry(int *w, int *h, int *cw, int *ch)
{ if(w)*w=52; if(h)*h=67; if(cw)*cw=font_w; if(ch)*ch=font_h; }
static tui_cell actual[120*70], reference[120*70];
static ls_compass_scene_t scene(void)
{
    ls_compass_scene_t sc={0}; sc.valid=true;
    sc.target=sc.sun=sc.lock=sc.df=sc.df_spread=NAN;
    return sc;
}
LS_CASE(cached_art_matches_fresh_pose_and_layout)
{
    ls_compass_scene_t sc=scene();
    for(int layout=0;layout<2;layout++) {
        tui_surface a={.w=120,.h=70,.back=actual}, b=a; b.back=reference;
        tui_rect area=tui_rect_make(2,6,layout?58:48,layout?15:32);
        for(int frame=0;frame<80;frame++) {
            sc.heading=frame*4.5f; sc.pitch=frame<40?0:12; sc.roll=frame<60?0:8;
            sc.valid=frame%9!=0; sc.white_letters=frame%11==0;
            sc.lock=frame%3?NAN:90; sc.seconds=frame;
            font_w=frame<70?10:9; font_h=frame<70?17:16;
            tui_frame_begin(&a); tui_frame_begin(&b);
            ls_compass_art_draw(&a,area,&sc);
            s_pose_valid=false; art_draw(&b,area,&sc);
            for(int i=0;i<a.w*a.h;i++) {
                LS_CHECK(actual[i].ch==reference[i].ch);
                LS_CHECK(actual[i].attr==reference[i].attr);
            }
        }
    }
}
LS_CASE(static_cache_and_yaw_trig_budget)
{
    font_w=10; font_h=17;
    tui_surface a={.w=120,.h=70,.back=actual};
    tui_rect area=tui_rect_make(2,6,48,32);
    ls_compass_scene_t sc=scene();
    s_pose_valid=s_art_valid=false; trig_calls=0;
    tui_frame_begin(&a); ls_compass_art_draw(&a,area,&sc);
    unsigned cold=trig_calls;
    trig_calls=0;
    for(int i=0;i<100;i++) { sc.seconds=i; tui_frame_begin(&a); ls_compass_art_draw(&a,area,&sc); }
    LS_CHECK(trig_calls==0);
    trig_calls=0;
    for(int i=1;i<=100;i++) { sc.heading=i*2; tui_frame_begin(&a); ls_compass_art_draw(&a,area,&sc); }
    printf("    trig cold=%u, stationary/frame=0, yaw/frame=%.1f\n",cold,trig_calls/100.0);
    LS_CHECK(trig_calls/100 < cold/5);
}

LS_CASE(render_cost_profile)
{
    font_w=10; font_h=17;
    tui_surface a={.w=120,.h=70,.back=actual};
    tui_rect area=tui_rect_make(2,6,48,32);
    ls_compass_scene_t sc=scene();
    const int n=1000;
    clock_t start=clock();
    for(int i=0;i<n;i++) {
        sc.heading=(i%180)*2; s_pose_valid=false;
        tui_frame_begin(&a); art_draw(&a,area,&sc);
    }
    double uncached=(double)(clock()-start)*1e6/CLOCKS_PER_SEC/n;
    start=clock();
    for(int i=0;i<n;i++) {
        sc.heading=(i%180)*2;
        tui_frame_begin(&a); ls_compass_art_draw(&a,area,&sc);
    }
    double yaw=(double)(clock()-start)*1e6/CLOCKS_PER_SEC/n;
    start=clock();
    for(int i=0;i<n;i++) { tui_frame_begin(&a); ls_compass_art_draw(&a,area,&sc); }
    double still=(double)(clock()-start)*1e6/CLOCKS_PER_SEC/n;
    printf("    dial us/frame uncached=%.1f yaw=%.1f still=%.1f (host only)\n",uncached,yaw,still);
}
