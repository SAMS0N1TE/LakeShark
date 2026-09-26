#include "ls_test.h"
#include "ls_compass_live.h"
#include <math.h>
#include <string.h>

#define RAD 0.017453292519943295

typedef struct { double n, e, d; } ned;
static ned cross3(ned a, ned b) { return (ned){ a.e * b.d - a.d * b.e, a.d * b.n - a.n * b.d, a.n * b.e - a.e * b.n }; }
static double dot3(ned a, ned b) { return a.n * b.n + a.e * b.e + a.d * b.d; }

/* A field of 20 uT north and 45 uT down: about New England's dip. */
static const ned FIELD = { 20, 0, 45 };

/* The board with its top at azimuth `yaw`, raised by `pitch`, rolled about
   the top by `roll`, all degrees. Fills a sample the way ls_imu does. */
static ls_imu_sample_t pose(double yaw, double pitch, double roll, const float offset[3])
{
    const double y = yaw * RAD, p = pitch * RAD, r = roll * RAD;
    const ned top = { cos(y) * cos(p), sin(y) * cos(p), -sin(p) };
    const ned r0 = { -sin(y), cos(y), 0 };
    const ned tr = cross3(top, r0);
    const ned right = { r0.n * cos(r) + tr.n * sin(r), r0.e * cos(r) + tr.e * sin(r), r0.d * cos(r) + tr.d * sin(r) };
    const ned out = cross3(right, top);
    const ned upward = { 0, 0, -1 };
    ls_imu_sample_t s;
    memset(&s, 0, sizeof(s));
    /* As ls_imu delivers it on this board: x and z reversed. */
    s.ax = (float)-dot3(upward, right); s.ay = (float)dot3(upward, top); s.az = (float)-dot3(upward, out);
    /* Screen-frame field, then into the magnetometer's basis. */
    const double mr = dot3(FIELD, right), mu = dot3(FIELD, top), mo = dot3(FIELD, out);
    s.mx = (float)-mu; s.my = (float)-mr; s.mz = (float)-mo;
    if (offset) { s.mx += offset[0]; s.my += offset[1]; s.mz += offset[2]; }
    s.mag_valid = true;
    return s;
}

static double angle_error(double a, double b) { double d = fmod(a - b + 540, 360) - 180; return fabs(d); }

LS_CASE(flat_matches_the_existing_heading_formula)
{
    for (int yaw = 0; yaw < 360; yaw += 15) {
        ls_imu_sample_t s = pose(yaw, 0, 0, NULL);
        ls_compass_reading_t r; bool back = false;
        LS_CHECK(ls_compass_solve(&s, NULL, &back, &r));
        LS_CHECK_MSG(angle_error(r.magnetic, yaw) < 0.2, "yaw %d got %.2f", yaw, r.magnetic);
        LS_CHECK(!r.back_axis);
        LS_NEAR(r.tilt, 0, 0.1);
    }
}

LS_CASE(tilt_does_not_move_the_heading)
{
    for (int yaw = 0; yaw < 360; yaw += 30)
        for (int pitch = -40; pitch <= 40; pitch += 20)
            for (int roll = -40; roll <= 40; roll += 20) {
                ls_imu_sample_t s = pose(yaw, pitch, roll, NULL);
                ls_compass_reading_t r; bool back = false;
                LS_CHECK(ls_compass_solve(&s, NULL, &back, &r));
                LS_CHECK_MSG(angle_error(r.magnetic, yaw) < 0.3, "yaw %d pitch %d roll %d got %.2f",
                             yaw, pitch, roll, r.magnetic);
            }
}

LS_CASE(held_upright_it_reads_where_the_back_faces)
{
    /* Top up at 80 degrees, screen toward the user: the back faces the yaw. */
    for (int yaw = 0; yaw < 360; yaw += 45) {
        ls_imu_sample_t s = pose(yaw, 80, 0, NULL);
        ls_compass_reading_t r; bool back = false;
        LS_CHECK(ls_compass_solve(&s, NULL, &back, &r));
        LS_CHECK(r.back_axis);
        LS_CHECK_MSG(angle_error(r.magnetic, yaw) < 0.5, "yaw %d got %.2f", yaw, r.magnetic);
    }
}

LS_CASE(the_axis_choice_does_not_flicker_at_the_edge)
{
    bool back = false; ls_compass_reading_t r;
    ls_imu_sample_t s = pose(0, 50, 0, NULL);         /* between the thresholds */
    ls_compass_solve(&s, NULL, &back, &r); LS_CHECK(!r.back_axis);
    s = pose(0, 70, 0, NULL); ls_compass_solve(&s, NULL, &back, &r); LS_CHECK(r.back_axis);
    s = pose(0, 50, 0, NULL); ls_compass_solve(&s, NULL, &back, &r); LS_CHECK(r.back_axis);
    s = pose(0, 30, 0, NULL); ls_compass_solve(&s, NULL, &back, &r); LS_CHECK(!r.back_axis);
}

LS_CASE(calibration_offset_is_removed_and_dip_and_strength_are_measured)
{
    const float offset[3] = { 300, -580, -575 };
    ls_imu_sample_t s = pose(123, 10, -5, offset);
    ls_compass_cal_t cal = { .offset = { 300, -580, -575 }, .radius = 49, .error = 0.01f };
    ls_compass_reading_t r; bool back = false;
    LS_CHECK(ls_compass_solve(&s, &cal, &back, &r));
    LS_CHECK(r.calibrated);
    LS_CHECK(angle_error(r.magnetic, 123) < 0.3);
    LS_NEAR(r.field_ut, sqrt(20 * 20 + 45 * 45), 0.05);
    LS_NEAR(r.dip, atan2(45, 20) / RAD, 0.1);
}

LS_CASE(model_adds_declination_and_flags_a_bent_field)
{
    ls_imu_sample_t s = pose(10, 0, 0, NULL);
    ls_compass_cal_t cal = { .offset = { 0, 0, 0 }, .radius = 49, .error = 0.01f };
    ls_compass_reading_t r; bool back = false;
    LS_CHECK(ls_compass_solve(&s, &cal, &back, &r));
    ls_compass_apply_model(&r, -14.5, sqrt(20 * 20 + 45 * 45), atan2(45, 20) / RAD);
    LS_CHECK(r.true_valid);
    LS_NEAR(r.true_deg, 355.5, 0.3);
    LS_CHECK(!r.interference);
    /* Strength is judged against the calibration's radius, not the model:
       the model can be off by a third and nothing is wrong... */
    ls_compass_apply_model(&r, -14.5, 30, atan2(45, 20) / RAD);
    LS_CHECK(!r.interference);
    /* ...but a field well off the calibration's is. */
    r.field_ut = 30;
    ls_compass_apply_model(&r, -14.5, sqrt(20 * 20 + 45 * 45), atan2(45, 20) / RAD);
    LS_CHECK(r.interference);
    r.field_ut = 49;
    ls_compass_apply_model(&r, -14.5, sqrt(20 * 20 + 45 * 45), 40); /* dip off by 26 degrees */
    LS_CHECK(r.interference);
}

LS_CASE(no_magnetometer_or_no_gravity_gives_no_heading)
{
    ls_imu_sample_t s = pose(0, 0, 0, NULL);
    ls_compass_reading_t r; bool back = false;
    s.mag_valid = false;
    LS_CHECK(!ls_compass_solve(&s, NULL, &back, &r));
    LS_CHECK(isnan(r.magnetic));
    LS_NEAR(r.tilt, 0, 0.1);                 /* the level still works */
    s = pose(0, 0, 0, NULL); s.ax = s.ay = s.az = 0;
    LS_CHECK(!ls_compass_solve(&s, NULL, &back, &r));
}

LS_CASE(great_circle_bearing_and_distance)
{
    double b, d;
    ls_compass_nav(0, 0, 0, 1, &b, &d);
    LS_NEAR(b, 90, 1e-6); LS_NEAR(d, 111195, 5);
    ls_compass_nav(43.2, -71.65, 43.3, -71.65, &b, &d);
    LS_NEAR(b, 0, 1e-6); LS_NEAR(d, 11119.5, 1);
    ls_compass_nav(43.2, -71.65, 41.66, -70.2, &b, &d);   /* Franklin NH to West Dennis MA */
    LS_CHECK(b > 140 && b < 150);
    LS_CHECK(d > 205000 && d < 215000);
}

LS_CASE(the_target_is_kept_until_cleared)
{
    double lat, lon; char name[48];
    ls_compass_clear_target();
    LS_CHECK(!ls_compass_target(&lat, &lon, name, sizeof(name)));
    ls_compass_set_target(43.1, -71.2, "North tower");
    LS_CHECK(ls_compass_target(&lat, &lon, name, sizeof(name)));
    LS_EQ_STR(name, "North tower");
    ls_compass_set_target(NAN, 0, "bad");
    LS_CHECK(!ls_compass_target(&lat, &lon, name, sizeof(name)));
}

/* Five poses held on the board, as ls_imu reported them
   (accelerometer g, raw field uT), with the offset the four distinct poses
   solve to. Flat reads as flat, and the dip agrees with the model (67) in
   every pose; with the accelerometer taken as delivered it did not. */
LS_CASE(board_poses_read_flat_as_flat_and_agree_on_the_dip)
{
    static const float p[5][6] = {
        { 0.02f,  0.04f, -0.98f,   5.1f, -10.6f,  -33.5f},   /* flat, face up */
        { 0.02f,  0.03f, -0.98f,  -3.7f, -36.8f,  -32.2f},   /* flat, turned */
        { 1.00f, -0.06f,  0.08f, -12.6f, -75.0f, -113.6f},   /* right edge down */
        {-0.03f,  0.99f, -0.12f,  31.3f, -15.6f, -111.2f},   /* USB edge down */
        { 0.02f,  0.04f, -0.98f,  10.3f, -13.8f,  -41.9f}};  /* flat again */
    const ls_compass_cal_t cal = { .offset = { -32.3f, -15.4f, -89.2f }, .radius = 67.3f, .error = 0.01f };
    for (int i = 0; i < 5; i++) {
        ls_imu_sample_t s = {.ax=p[i][0],.ay=p[i][1],.az=p[i][2],.mx=p[i][3],.my=p[i][4],.mz=p[i][5],.mag_valid=true};
        ls_compass_reading_t r; bool back = false;
        LS_CHECK(ls_compass_solve(&s, &cal, &back, &r));
        LS_CHECK(r.dip > 45 && r.dip < 72);
        if (i == 0 || i == 1 || i == 4) LS_CHECK(r.tilt < 10);
    }
}

LS_CASE(the_interference_warning_does_not_flicker)
{
    ls_compass_bend_t b; memset(&b, 0, sizeof(b));
    int64_t t = 0;
    /* Jitter across the limit every sample, as the board does: never on. */
    for (int i = 0; i < 200; i++, t += 50000)
        LS_CHECK(!ls_compass_bend_step(&b, (i & 1) ? 1.15f : 0.8f, t));
    /* Truly bent: on after about two seconds, not before. */
    int on_at = -1;
    for (int i = 0; i < 200; i++, t += 50000)
        if (ls_compass_bend_step(&b, 2.0f, t) && on_at < 0) on_at = i;
    LS_CHECK(on_at >= 38 && on_at < 80);
    /* Back to clean: stays on for three seconds, then off, and stays off. */
    int off_at = -1;
    for (int i = 0; i < 300; i++, t += 50000)
        if (!ls_compass_bend_step(&b, 0.3f, t) && off_at < 0) off_at = i;
    LS_CHECK(off_at >= 60);
    LS_CHECK(!ls_compass_bend_step(&b, 0.3f, t));
    /* Nothing to judge clears it at once. */
    LS_CHECK(!ls_compass_bend_step(&b, NAN, t));
}

/* Lying still, a heading that wobbles by a couple of degrees reads within
   half a degree; a turn is followed at once. */
LS_CASE(the_steady_heading_holds_at_rest_and_follows_a_turn)
{
    ls_compass_steady_t s = { 0 };
    float h = 0;
    for (int i = 0; i < 250; i++) {                      /* 10 s at 25 frames a second */
        const float wobble = 2.0f * sinf(i * 1.7f) + 1.0f * sinf(i * 4.3f);
        h = ls_compass_steady_step(&s, fmodf(359.5f + wobble + 360.0f, 360.0f), 0, 0, 0.5f, 0.04f);
    }
    const float off = fabsf(fmodf(h - 359.5f + 540.0f, 360.0f) - 180.0f);
    LS_CHECK_MSG(off < 0.7f, "at rest %.2f from true", off);
    /* A 45 degree turn over half a second, the gyro saying so. */
    for (int i = 1; i <= 12; i++) h = ls_compass_steady_step(&s, fmodf(359.5f + i * 3.75f, 360.0f), 0, 0, 90.0f, 0.04f);
    LS_CHECK_MSG(fabsf(h - 44.5f) < 1.5f, "after the turn %.1f", h);
    /* Without a gyro, a jump is still a turn. */
    ls_compass_steady_t q = { 0 };
    ls_compass_steady_step(&q, 100, 0, 0, NAN, 0.04f);
    for (int i = 0; i < 10; i++) h = ls_compass_steady_step(&q, 160, 0, 0, NAN, 0.04f);
    LS_CHECK_MSG(fabsf(h - 160) < 3.0f, "jump %.1f", h);
}
