#include "ls_test.h"
#include "ls_compass_live.h"
#include "ls_compass_learn.h"
#include "ls_compass_still.h"

static float delta(float a, float b) { return fmodf(a-b+540,360)-180; }
typedef struct { float ms, heading, dip, field; ls_imu_sample_t p; } row;
static row trace[100];
static int read_trace(void)
{
    FILE *f=fopen(COMPASS_TRACE, "r"); LS_CHECK(f); if (!f) return 0;
    char line[400], fresh; int n=0;
    while (fgets(line,sizeof(line),f)) {
        row r={0}; float an,tilt; int back;
        if (sscanf(line,"T %f %c %f %f %f %f %f %f %f %f %f %f %f %f %f %f %d",
            &r.ms,&fresh,&r.p.ax,&r.p.ay,&r.p.az,&an,&r.p.gx,&r.p.gy,&r.p.gz,
            &r.p.mx,&r.p.my,&r.p.mz,&r.field,&r.heading,&r.dip,&tilt,&back)==17) {
            r.p.mag_valid = fresh=='F'; trace[n++]=r;
        }
    }
    fclose(f); LS_EQ_INT(n,68); return n;
}
LS_CASE(lr_capture_replays_without_gyro_manufactured_motion)
{
    const int n=read_trace();
    ls_compass_steady_t s={0}; ls_compass_still_t stats={0};
    float elapsed=0;
    for (int i=0; elapsed<60; i++) {
        const row *r=&trace[i%n];
        const float dt=i%n ? (r->ms-trace[i%n-1].ms)/1000 : .07f;
        const float h=ls_compass_steady_sample(&s,r->heading,0,0,&r->p,dt);
        const float v[5]={r->p.gx,r->p.gy,r->p.gz,r->field,r->dip};
        ls_compass_still_add(&stats,h,v,true); /* independent range assertion includes acquisition */
        LS_CHECK(!s.gyro_used);
        if (elapsed >= 1.1f) LS_CHECK(s.stationary);
        elapsed+=dt;
        if (i==n-1) {
            ls_note("original 4.997 s: range %.3f, net %.3f",stats.hi-stats.lo,stats.angle-stats.first);
            LS_CHECK(stats.hi-stats.lo<=2); LS_CHECK(fabsf(stats.angle-stats.first)<=1);
        }
    }
    ls_note("repeated 60 s stress: range %.3f, net %.3f",stats.hi-stats.lo,stats.angle-stats.first);
    LS_CHECK(ls_compass_still_pass(&stats,elapsed));
    LS_CHECK(!s.rest_known);
}
static ls_imu_sample_t sample_at(float h, float rate)
{
    const float rad=h*.01745329252f;
    return (ls_imu_sample_t){.az=-1,.gx=-6,.gy=-13,.gz=2+rate,
        .mx=-21*cosf(rad),.my=21*sinf(rad),.mz=47.5f,.mag_valid=true};
}
static void fuse_observation(ls_compass_steady_t *s, const ls_imu_sample_t *p, float target)
{
    ls_compass_reading_t r; bool back=false;
    LS_CHECK(ls_compass_solve(p,NULL,&back,&r));
    LS_NEAR(delta(r.magnetic,target),0,.001f);
    ls_compass_steady_sample(s,r.magnetic,r.pitch,r.roll,p,.05f);
}
LS_CASE(turns_90_180_360_at_30_60_90_dps_both_directions)
{
    const int angles[]={90,180,360}, rates[]={30,60,90};
    float worst = 0;
    for (int unqualified=0;unqualified<2;unqualified++)
    for (int sign=-1;sign<=1;sign+=2)
    for (int i=0;i<3;i++) for (int j=0;j<3;j++) {
        ls_compass_steady_t s={0}; float h=350;
        if (!unqualified) for (int k=0;k<100;k++) {
            ls_imu_sample_t p=sample_at(h,0); fuse_observation(&s,&p,h);
        }
        for (float moved=0;moved<angles[i]-.01f;) {
            const float step=fminf(rates[j]*.05f,angles[i]-moved);
            moved+=step; h=fmodf(h+sign*step+360,360);
            ls_imu_sample_t p=sample_at(h,sign*rates[j]);
            fuse_observation(&s,&p,h);
        }
        for (int k=0;k<100;k++) {
            ls_imu_sample_t p=sample_at(h,0); fuse_observation(&s,&p,h);
        }
        worst = fmaxf(worst, fabsf(delta(s.heading,h)));
        LS_CHECK_MSG(fabsf(delta(s.heading,h))<=3,"turn %d at %d sign %d startup %d: error %.3f",
                     angles[i],rates[j],sign,unqualified,delta(s.heading,h));
    }
    ls_note("36 turn cases: worst settled error %.3f deg", worst);
}
LS_CASE(gyro_must_converge_and_agree_over_seconds)
{
    ls_compass_steady_t s={0}; ls_imu_sample_t p=sample_at(0,0);
    for(int i=0;i<120;i++) ls_compass_steady_sample(&s,0,0,0,&p,.05f);
    LS_CHECK(s.rest_known); LS_CHECK(s.gyro_healthy);
    LS_NEAR(s.rest[0],-6,.01); LS_NEAR(s.rest[1],-13,.01); LS_NEAR(s.rest[2],2,.01);
    p.gz=42;
    for(int i=0;i<45;i++) ls_compass_steady_sample(&s,0,0,0,&p,.05f);
    LS_CHECK(!s.rest_known); LS_CHECK(!s.gyro_used); LS_NEAR(s.heading,0,.01);
}
LS_CASE(flat_constant_turn_is_not_learnt_as_rest_bias)
{
    ls_compass_steady_t s={0};
    const float a[3]={0,0,-1},g[3]={-6,-13,32};
    for(int i=0;i<240;i++) {
        ls_compass_steady_imu(&s,fmodf(i*1.5f,360),0,0,g,a,.05f);
        LS_CHECK(!s.rest_known); LS_CHECK(!s.gyro_used);
    }
}
LS_CASE(still_usb_learning_never_applies)
{
    ls_compass_learn_t l; ls_compass_learn_reset(&l,52);
    const float up[3]={0,0,1};
    for(int i=0;i<10000;i++) {
        const float f[3]={21+22+.1f*sinf(i),.1f*cosf(i),-47.5f};
        ls_compass_learn_step(&l,f,up,20,.4f,.1f);
        float c[3]; LS_CHECK(!ls_compass_learn_correction(&l,.4f,c));
    }
}
LS_CASE(physical_gates_judge_the_applied_field_and_dip)
{
    const float up[3]={0,0,1}, f[3]={21,0,-47.5f}, zero[3]={0};
    LS_CHECK(ls_compass_learn_plausible(f,up,zero,52,66));
    const float collapse[3]={13,0,-29};
    LS_CHECK(!ls_compass_learn_plausible(f,up,collapse,52,NAN));
    const float wrong_dip[3]={-31,0,-47.5f};
    LS_CHECK(!ls_compass_learn_plausible(f,up,wrong_dip,52,66));
    LS_CHECK(!ls_compass_learn_plausible(f,up,zero,NAN,66));
    ls_compass_learn_t l; ls_compass_learn_reset(&l,52);
    l.started=true; l.applied=7; memcpy(l.field,f,sizeof(f));memcpy(l.up,up,sizeof(up));
    for(int i=0;i<3;i++){l.x[i]=collapse[i];l.P[i][i]=.25f;}
    float c[3]; LS_CHECK(!ls_compass_learn_correction(&l,0,c));
}
LS_CASE(calibration_quality_precedes_generation)
{
    ls_compass_cal_t old={.radius=52.6f,.error=.023f}, bad={.radius=39.6f,.error=.041f};
    LS_CHECK(ls_compass_cal_upgrade(&bad,&old,52));
    LS_CHECK(ls_compass_cal_upgrade(&bad,&old,NAN));
    bad.radius=52.5f; LS_CHECK(ls_compass_cal_upgrade(&bad,&old,52));
    bad.error=.022f; LS_CHECK(!ls_compass_cal_upgrade(&bad,&old,52));
}
LS_CASE(still_statistics_wrap_north_and_refuse_short_or_missing_runs)
{
    ls_compass_still_t s={0}; const float v[5]={0,0,0,52,66};
    for(int i=0;i<1200;i++) ls_compass_still_add(&s,i%2?0.2f:359.8f,v,true);
    LS_CHECK(ls_compass_still_pass(&s,60)); LS_CHECK(!ls_compass_still_pass(&s,5));
    LS_NEAR(s.hi-s.lo,.4,.001); s.missing++; LS_CHECK(!ls_compass_still_pass(&s,60));
}

LS_CASE(lr_capture_resolved_view_preserves_still_board_criteria)
{
    const int n=read_trace();
    ls_compass_steady_t steady={0}; ls_compass_still_t stats={0};
    float elapsed=0; bool back=false;
    for(int i=0;elapsed<60;i++) {
        const row *v=&trace[i%n]; ls_compass_reading_t r;
        LS_CHECK(ls_compass_solve(&v->p,NULL,&back,&r));
        const float dt=i%n ? (v->ms-trace[i%n-1].ms)/1000 : .07f;
        const float h=ls_compass_steady_sample(&steady,r.magnetic,r.pitch,r.roll,&v->p,dt);
        const float values[5]={v->p.gx,v->p.gy,v->p.gz,r.field_ut,r.dip};
        ls_compass_still_add(&stats,h,values,true); elapsed+=dt;
    }
    ls_note("resolved view LR 60 s: range %.3f net %.3f",stats.hi-stats.lo,stats.angle-stats.first);
    LS_CHECK(ls_compass_still_pass(&stats,elapsed));
}
