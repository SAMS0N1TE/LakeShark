#include "ls_test.h"
#include "ls_compass.h"
#include <math.h>
#include <string.h>

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

/* ---- physical poses: the board turned in a real field -------------------- */

#define RAD 0.017453292519943295
typedef struct { double n, e, d; } ned;
static ned cross3(ned a, ned b) { return (ned){ a.e*b.d - a.d*b.e, a.d*b.n - a.n*b.d, a.n*b.e - a.e*b.n }; }
static double dot3(ned a, ned b) { return a.n*b.n + a.e*b.e + a.d*b.d; }
static const ned EARTH = { 20, 0, 45 };   /* about New England: 66 degrees of dip */
/* AK09916 noise is about 0.6 uT; the accelerometer in a hand, a few
   hundredths of a g. Deterministic so a failure repeats. */
static uint32_t noise_state;
static bool quiet;
static double noise(double amplitude)
{
    if (quiet) return 0;
    noise_state = noise_state * 1664525u + 1013904223u;
    return amplitude * ((noise_state >> 8) / 8388608.0 - 1);
}
static const double SOFT[3][3] = {{1.25, .10, 0}, {.10, .85, .05}, {0, .05, 1.0}};

/* The board with its top at `yaw`, raised by `pitch`, rolled by `roll`
   (degrees). `datasheet` stores the field the way the ICM-20948 figure has
   the AK09916 (screen = {my, -mx, mz}) instead of the firmware's default. */
static ls_imu_sample_t pose(double yaw, double pitch, double roll, bool datasheet, bool soft)
{
    const double y = yaw*RAD, p = pitch*RAD, r = roll*RAD;
    const ned top = { cos(y)*cos(p), sin(y)*cos(p), -sin(p) };
    const ned r0 = { -sin(y), cos(y), 0 }, tr = cross3(top, r0);
    const ned right = { r0.n*cos(r) + tr.n*sin(r), r0.e*cos(r) + tr.e*sin(r), r0.d*cos(r) + tr.d*sin(r) };
    const ned out = cross3(right, top), up = { 0, 0, -1 };
    const double bx = dot3(EARTH, right), by = dot3(EARTH, top), bz = dot3(EARTH, out);
    double t[3];
    if (datasheet) { t[0] = -by; t[1] = bx; t[2] = bz; }
    else           { t[0] = -by; t[1] = -bx; t[2] = -bz; }
    /* The accelerometer as ls_imu delivers it on this board: x and z reversed. */
    ls_imu_sample_t s = {.ax=(float)(-dot3(up, right) + noise(.03)), .ay=(float)(dot3(up, top) + noise(.03)),
                         .az=(float)(-dot3(up, out) + noise(.03)), .mag_valid=true};
    float *m[3] = {&s.mx, &s.my, &s.mz};
    for (int i = 0; i < 3; i++) {
        double v = t[i];
        if (soft) v = SOFT[i][0]*t[0] + SOFT[i][1]*t[1] + SOFT[i][2]*t[2];
        *m[i] = (float)(bias[i] + v + noise(1.5));
    }
    return s;
}

/* Six faces turned through a circle each, then (optionally) a figure eight. */
static void physical_fit(bool datasheet, bool soft, bool tumble)
{
    ls_compass_begin(&fit);
    noise_state = 12345;
    static const double faces[6][2] = {{0,0},{0,180},{90,0},{-90,0},{0,90},{0,-90}};
    for (int f = 0; f < 6; f++)
        for (int i = 0; i < 64; i++) {
            ls_imu_sample_t s = pose(i * 360.0 / 64, faces[f][0], faces[f][1], datasheet, soft);
            ls_compass_collect(&fit, &s);
        }
    if (!tumble) return;
    LS_CHECK(ls_compass_tumble_begin(&fit));
    for (int i = 0; i < 400 && !ls_compass_tumble_done(&fit); i++) {
        ls_imu_sample_t s = pose(i * 137.508, asin(1 - 2*(i + .5)/192) / RAD, fmod(i * 57.3, 360), datasheet, soft);
        ls_compass_collect(&fit, &s);
    }
    LS_CHECK(ls_compass_tumble_done(&fit));
}

static double worst_flat_error(const ls_compass_cal_t *c, bool datasheet, bool soft)
{
    double worst = 0;
    quiet = true;   /* judge the calibration, not one noisy sample */
    for (int deg = 0; deg < 360; deg += 10) {
        ls_imu_sample_t s = pose(deg, 0, 0, datasheet, soft);
        const double e = fabs(fmod(ls_compass_heading(&s, c) - deg + 540, 360) - 180);
        if (e > worst) worst = e;
    }
    quiet = false;
    return worst;
}

LS_CASE(the_figure_eight_takes_out_soft_iron_a_sphere_cannot)
{
    ls_compass_cal_t sphere_only, full;
    /* Held faces only: either the residual is refused outright, or the
       sphere is accepted and the heading is bent. */
    physical_fit(false, true, false);
    const bool accepted = ls_compass_finish(&fit, &sphere_only);
    physical_fit(false, true, true);
    LS_CHECK(ls_compass_finish(&fit, &full));
    LS_CHECK(full.soft[0][0] != 0);
    LS_CHECK(full.error < .03f);
    LS_CHECK(!accepted || worst_flat_error(&sphere_only, false, true) > 5);
    LS_CHECK(worst_flat_error(&full, false, true) < 2);
}

LS_CASE(a_right_basis_is_kept_and_its_dip_agrees_in_every_pose)
{
    physical_fit(false, false, true);
    ls_compass_cal_t c;
    LS_CHECK(ls_compass_finish(&fit, &c));
    LS_EQ_INT(c.basis[0], -2); LS_EQ_INT(c.basis[1], -1); LS_EQ_INT(c.basis[2], -3);
    LS_CHECK(c.dip_spread < 3);
    LS_CHECK(worst_flat_error(&c, false, false) < 2);
}

LS_CASE(the_basis_is_always_the_default_and_others_are_refused)
{
    physical_fit(true, false, true);
    ls_compass_cal_t c;
    LS_CHECK(ls_compass_finish(&fit, &c));
    LS_EQ_INT(c.basis[0], -2); LS_EQ_INT(c.basis[1], -1); LS_EQ_INT(c.basis[2], -3);
    LS_CHECK(c.dip_spread > 10);   /* data from another basis shows as spread */
    c.basis[0] = 2; c.basis[2] = 3;
    LS_CHECK(!ls_compass_cal_valid(&c));
}

LS_CASE(six_held_faces_alone_still_calibrate_hard_iron)
{
    physical_fit(false, false, false);
    ls_compass_cal_t c;
    LS_CHECK(ls_compass_finish(&fit, &c));
    LS_CHECK(c.soft[0][0] == 0);
    for (int i = 0; i < 3; i++) LS_NEAR(c.offset[i], bias[i], .5);
}
