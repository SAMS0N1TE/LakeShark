#include "ls_test.h"
#include "ls_compass.h"
#include <math.h>

static ls_compass_fit_t fit;
static const float bias[3] = {70, -95, 120};

static void sphere(bool flat)
{
    ls_compass_begin(&fit);
    for (int i = 0; i < 600; i++) {
        float z = flat ? 0 : 1 - 2 * (i + .5f) / 600;
        float angle = i * 2.39996323f, r = sqrtf(1-z*z);
        float x = r*cosf(angle), y = r*sinf(angle);
        ls_imu_sample_t s = {.ax=x,.ay=y,.az=z,.mx=bias[0]+48*x,
            .my=bias[1]+48*y,.mz=bias[2]+48*z,.mag_valid=true};
        ls_compass_collect(&fit, &s);
    }
}

LS_CASE(calibration_removes_a_bias_larger_than_the_earth_field)
{
    sphere(false); ls_compass_cal_t c;
    LS_CHECK(ls_compass_finish(&fit, &c));
    for (int i = 0; i < 3; i++) LS_NEAR(c.offset[i], bias[i], .01);
    LS_NEAR(c.radius, 48, .01);
    for (int deg = 0; deg < 360; deg += 30) {
        float angle = deg * .01745329252f;
        ls_imu_sample_t s = {.mx=bias[0]-25*cosf(angle),.my=bias[1]+25*sinf(angle),.mag_valid=true};
        float error = fmodf(ls_compass_heading(&s, &c) - deg + 540, 360) - 180;
        LS_NEAR(error, 0, .01);
    }
}
LS_CASE(physical_top_heading_is_clockwise_in_the_p4_magnetic_basis)
{
    const float fields[4][2] = {{-30,0},{0,30},{30,0},{0,-30}};
    for (int i=0; i<4; i++) {
        ls_imu_sample_t s = {.mx=fields[i][0],.my=fields[i][1],.mag_valid=true};
        LS_NEAR(ls_compass_heading(&s, NULL), i*90, .001);
    }
    ls_imu_sample_t west = {.mx=-30,.my=-.01f,.mag_valid=true};
    ls_imu_sample_t east = {.mx=-30,.my=.01f,.mag_valid=true};
    LS_CHECK(ls_compass_heading(&west,NULL)>359);
    LS_CHECK(ls_compass_heading(&east,NULL)<1);
}
LS_CASE(a_flat_circle_cannot_claim_a_three_axis_calibration)
{
    sphere(true); ls_compass_cal_t c = {.radius=33};
    LS_CHECK(!ls_compass_finish(&fit, &c)); LS_NEAR(c.radius, 33, .01);
    fit.faces = 63;
    LS_CHECK(!ls_compass_finish(&fit, &c));
}
LS_CASE(still_or_invalid_samples_cannot_pass)
{
    ls_compass_begin(&fit);
    ls_imu_sample_t s = {.az=1,.mx=20,.my=-50,.mz=90,.mag_valid=true};
    for (int i = 0; i < 400; i++) ls_compass_collect(&fit, &s);
    fit.faces = 63; ls_compass_cal_t c;
    LS_CHECK(!ls_compass_finish(&fit, &c));
    s.mx = NAN; uint32_t count = fit.samples; ls_compass_collect(&fit, &s); LS_EQ_UINT(fit.samples, count);
    LS_CHECK(isnan(ls_compass_heading(&s, NULL)));
}
LS_CASE(needle_takes_the_short_path_across_north)
{
    LS_NEAR(ls_compass_ease(359, 1, .5), 0, .001);
    LS_NEAR(ls_compass_ease(1, 359, .5), 0, .001);
    LS_NEAR(ls_compass_ease(NAN, 90, .1), 90, .001);
    LS_CHECK(isnan(ls_compass_ease(90, NAN, .2)));
}

LS_CASE(guide_requires_a_steady_hold_and_both_flat_sides)
{
    for (int polarity=-1;polarity<=1;polarity+=2) {
        ls_compass_guide_t g; ls_compass_guide_begin(&g);
        ls_imu_sample_t s={.az=(float)polarity,.mx=25,.my=10,.mz=35,.mag_valid=true};
        for(int i=0;i<19;i++)ls_compass_guide_sample(&g,&s);
        LS_EQ_UINT(g.step,0);
        s.gy=60; ls_compass_guide_sample(&g,&s); LS_EQ_UINT(g.hold,0);
        s.gy=0;
        for(int i=0;i<19;i++)ls_compass_guide_sample(&g,&s);
        ls_compass_guide_sample(&g,NULL); LS_EQ_UINT(g.hold,0);
        const float poses[6][3]={{0,0,(float)polarity},{0,1,0},{1,0,0},{0,-1,0},{-1,0,0},{0,0,(float)-polarity}};
        for(unsigned face=0;face<6;face++) {
            s.ax=poses[face][0];s.ay=poses[face][1];s.az=poses[face][2];
            if(face==5) {
                s.az=(float)polarity;
                for(int i=0;i<30;i++)ls_compass_guide_sample(&g,&s);
                LS_EQ_UINT(g.step,5);LS_EQ_UINT(g.hold,0);
                s.az=(float)-polarity;
            }
            for(int i=0;i<20;i++)ls_compass_guide_sample(&g,&s);
            LS_EQ_UINT(g.step,face+1);
        }
    }
}

LS_CASE(waiting_on_one_side_does_not_exhaust_the_fit)
{
    ls_compass_begin(&fit);
    const float poses[6][3]={{0,0,1},{0,1,0},{1,0,0},{0,-1,0},{-1,0,0},{0,0,-1}};
    for(int face=0;face<6;face++) {
        ls_imu_sample_t s={.ax=poses[face][0],.ay=poses[face][1],.az=poses[face][2],
            .mx=bias[0]+48*poses[face][0],.my=bias[1]+48*poses[face][1],.mz=bias[2]+48*poses[face][2],.mag_valid=true};
        for(int i=0;i<(face==0?15000:20);i++)ls_compass_collect(&fit,&s);
    }
    ls_compass_cal_t c;
    LS_CHECK(ls_compass_finish(&fit,&c));
    for(int i=0;i<3;i++)LS_NEAR(c.offset[i],bias[i],.01);
    LS_CHECK(fit.samples<=512);
}
