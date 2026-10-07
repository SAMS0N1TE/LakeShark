#include "ls_test.h"
#include "ls_compass_live.h"
#include "ls_field.h"
#include <math.h>
#include <string.h>

#define RAD 0.017453292519943295

typedef struct { double n, e, d; } ned;
static ned cross3(ned a, ned b) { return (ned){ a.e * b.d - a.d * b.e, a.d * b.n - a.n * b.d, a.n * b.e - a.e * b.n }; }
static double dot3(ned a, ned b) { return a.n * b.n + a.e * b.e + a.d * b.d; }

/* A field of 20 uT north and 45 uT down: about New England's dip. */
static const ned FIELD = { 20, 0, 45 };

/* The board with its top at azimuth `yaw`, raised by `pitch`, rolled about
   the top by `roll`, all degrees, in `field`. Fills a sample the way ls_imu
   does. */
static ls_imu_sample_t pose_in(ned field, double yaw, double pitch, double roll, const float offset[3])
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
    const double mr = dot3(field, right), mu = dot3(field, top), mo = dot3(field, out);
    s.mx = (float)-mu; s.my = (float)-mr; s.mz = (float)-mo;
    if (offset) { s.mx += offset[0]; s.my += offset[1]; s.mz += offset[2]; }
    s.mag_valid = true;
    return s;
}

static ls_imu_sample_t pose(double yaw, double pitch, double roll, const float offset[3])
{
    return pose_in(FIELD, yaw, pitch, roll, offset);
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

/* Raising the board from flat to upright moves the heading smoothly, rolled
   or not. The axis used to switch from the top to the back in one step, and
   with any roll the two point different ways: the reading jumped, and a hold
   near the switch flipped it back and forth. */
LS_CASE(rising_from_flat_to_upright_never_jumps)
{
    for (int roll = -30; roll <= 30; roll += 10)
        for (int yaw = 0; yaw < 360; yaw += 90) {
            bool back = false;
            double last = NAN, worst = 0;
            int at = 0;
            for (int i = 0; i <= 170; i++) {
                const int pitch = i <= 85 ? i : 170 - i;     /* up to 85 and back down */
                ls_imu_sample_t s = pose(yaw, pitch, roll, NULL);
                ls_compass_reading_t r;
                LS_CHECK(ls_compass_solve(&s, NULL, &back, &r));
                if (isfinite(last) && angle_error(r.magnetic, last) > worst) {
                    worst = angle_error(r.magnetic, last);
                    at = pitch;
                }
                last = r.magnetic;
            }
            LS_CHECK_MSG(worst < 4.0, "roll %d yaw %d: the heading stepped %.1f degrees at pitch %d",
                         roll, yaw, worst, at);
        }
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

static float off_deg(float a, float b) { return fabsf(fmodf(a - b + 540.0f, 360.0f) - 180.0f); }

/* Lying still, a magnetic heading that wobbles by a couple of degrees and a
   gyro that rests at 3 deg/s read within half a degree once the bias is
   learnt; a turn is followed as it happens, not after. */
LS_CASE(the_fused_heading_holds_at_rest_and_follows_a_turn)
{
    ls_compass_steady_t s = { 0 };
    float h = 0;
    for (int i = 0; i < 500; i++) {                      /* 20 s at 25 frames a second */
        const float wobble = 2.0f * sinf(i * 1.7f) + 1.0f * sinf(i * 4.3f);
        h = ls_compass_steady_step(&s, fmodf(359.5f + wobble + 360.0f, 360.0f), 0, 0, 3.0f, 0.04f);
    }
    LS_CHECK_MSG(off_deg(h, 359.5f) < 0.5f, "at rest %.2f from true", off_deg(h, 359.5f));
    LS_CHECK_MSG(fabsf(s.bias - 3.0f) < 0.5f, "bias learnt as %.2f", s.bias);
    /* Still after the bias is learnt: the last few frames barely move. */
    float lo = 999, hi = -999;
    for (int i = 0; i < 25; i++) {
        const float wobble = 2.0f * sinf(i * 1.7f) + 1.0f * sinf(i * 4.3f);
        h = ls_compass_steady_step(&s, fmodf(359.5f + wobble + 360.0f, 360.0f), 0, 0, 3.0f, 0.04f);
        const float d = fmodf(h - 359.5f + 540.0f, 360.0f) - 180.0f;
        if (d < lo) lo = d;
        if (d > hi) hi = d;
    }
    LS_CHECK_MSG(hi - lo < 0.6f, "jitter at rest %.2f deg", hi - lo);
    /* A 90 degree turn at 90 deg/s: every frame of it within 2 degrees of
       where the board points, not trailing it. */
    float worst = 0;
    for (int i = 1; i <= 25; i++) {
        const float truth = fmodf(359.5f + i * 3.6f, 360.0f);
        h = ls_compass_steady_step(&s, truth, 0, 0, 90.0f + 3.0f, 0.04f);
        worst = fmaxf(worst, off_deg(h, truth));
    }
    LS_CHECK_MSG(worst < 2.0f, "worst lag in the turn %.2f deg", worst);
    LS_CHECK_MSG(s.agree > 0.4f, "agree %.2f after a clean turn", s.agree);
    /* Stopping: no overshoot past where it stopped. */
    float past = 0;
    for (int i = 0; i < 50; i++) {
        h = ls_compass_steady_step(&s, 89.5f, 0, 0, 3.0f, 0.04f);
        past = fmaxf(past, fmodf(h - 89.5f + 540.0f, 360.0f) - 180.0f);
    }
    LS_CHECK_MSG(past < 1.0f, "overshoot %.2f deg", past);
}

/* Without a gyro it follows the magnetometer alone; a jump past 45 degrees
   is taken once it has lasted 0.3 s. */
LS_CASE(the_fused_heading_without_a_gyro)
{
    ls_compass_steady_t q = { 0 };
    float h = ls_compass_steady_step(&q, 100, 0, 0, NAN, 0.04f);
    h = ls_compass_steady_step(&q, 160, 0, 0, NAN, 0.04f);
    LS_CHECK_MSG(fabsf(h - 100) < 0.1f, "one reading is not yet a jump %.1f", h);
    for (int i = 0; i < 8; i++) h = ls_compass_steady_step(&q, 160, 0, 0, NAN, 0.04f);
    LS_CHECK_MSG(fabsf(h - 160) < 0.1f, "jump %.1f", h);
    for (int i = 0; i < 25; i++) h = ls_compass_steady_step(&q, 180, 0, 0, NAN, 0.04f);
    LS_CHECK_MSG(fabsf(h - 180) < 1.0f, "20 degrees after a second %.1f", h);
    LS_CHECK(!q.gyro_used);
}

/* One wild magnetic reading - 2.5.0's field showed eight-degree blips at
   rest, and a bus glitch can do far worse - does not throw the dial across
   and back. */
LS_CASE(a_single_glitched_reading_does_not_throw_the_dial)
{
    ls_compass_steady_t s = { 0 };
    float h = 0, worst = 0;
    for (int i = 0; i < 100; i++) h = ls_compass_steady_step(&s, 200, 0, 0, 0, 0.04f);
    for (int k = 0; k < 3; k++) {
        h = ls_compass_steady_step(&s, 20, 0, 0, 0, 0.04f);          /* 180 out, one frame */
        worst = fmaxf(worst, off_deg(h, 200));
        for (int i = 0; i < 10; i++) { h = ls_compass_steady_step(&s, 200, 0, 0, 0, 0.04f); worst = fmaxf(worst, off_deg(h, 200)); }
    }
    LS_CHECK_MSG(worst < 1.0f, "a glitch moved the dial %.1f deg", worst);
    LS_EQ_INT((int)s.glitches, 3);
}

/* A gyro that turns against the magnetometer is found out and left out,
   not believed. */
LS_CASE(a_gyro_turning_the_wrong_way_is_left_out)
{
    ls_compass_steady_t s = { 0 };
    float h = ls_compass_steady_step(&s, 0, 0, 0, 0, 0.04f);
    for (int i = 1; i <= 100; i++) h = ls_compass_steady_step(&s, fmodf(i * 1.2f, 360.0f), 0, 0, -30.0f, 0.04f);
    LS_CHECK_MSG(s.agree < -0.5f, "agree %.2f", s.agree);
    LS_CHECK(!s.gyro_used);
    for (int i = 0; i < 50; i++) h = ls_compass_steady_step(&s, 120, 0, 0, -30.0f, 0.04f);
    LS_CHECK_MSG(off_deg(h, 120) < 1.0f, "follows the magnetometer %.1f", h);
}

/* Clockwise from above is positive whichever proper rotation the IMU is
   mounted in: face up, and the same turn with x and z reversed. */
LS_CASE(yaw_rate_is_clockwise_from_above_in_any_mounting)
{
    const float up[3] = { 0, 0, 1 }, turn_cw[3] = { 0, 0, -40 };
    LS_CHECK(fabsf(ls_compass_yaw_rate(turn_cw, up) - 40) < 1e-3f);
    const float up_r[3] = { 0, 0, -1 }, turn_r[3] = { 0, 0, 40 };
    LS_CHECK(fabsf(ls_compass_yaw_rate(turn_r, up_r) - 40) < 1e-3f);
    /* Held upright, top up: a turn about the board's y axis. */
    const float up_y[3] = { 0, 0.98f, 0.1f }, turn_y[3] = { 0, -25, 0 };
    LS_CHECK(fabsf(ls_compass_yaw_rate(turn_y, up_y) - 25 * 0.98f / sqrtf(0.98f * 0.98f + 0.01f)) < 0.1f);
    const float free_fall[3] = { 0, 0, 0.1f };
    LS_CHECK(isnan(ls_compass_yaw_rate(turn_cw, free_fall)));
}

/* A frame with no magnetic heading does not blank or reset the dial: the
   gyro carries it, for a while, then it holds. */
LS_CASE(a_frame_without_a_heading_coasts_on_the_gyro)
{
    ls_compass_steady_t s = { 0 };
    for (int i = 0; i < 50; i++) ls_compass_steady_step(&s, 100, 0, 0, 0, 0.04f);
    float h = ls_compass_steady_step(&s, NAN, 0, 0, 30.0f, 0.1f);
    for (int i = 0; i < 9; i++) h = ls_compass_steady_step(&s, NAN, 0, 0, 30.0f, 0.1f);
    LS_CHECK_MSG(off_deg(h, 130) < 1.0f, "coasted to %.1f", h);
    for (int i = 0; i < 100; i++) h = ls_compass_steady_step(&s, NAN, 0, 0, 30.0f, 0.1f);
    LS_CHECK_MSG(off_deg(h, 100 + 30 * LS_COMPASS_COAST_S) < 3.5f, "held at %.1f", h);   /* a step either side of the limit */
    LS_CHECK(s.started);
}

/* ------------------------------------------------ the board on the bench -- */

/* The bench board's field: dip 67 degrees, 15.5 uT across and 36.5 down,
   the 39.6 uT its calibration measured. */
static const ned DIP67 = { 15.5, 0, 36.5 };
/* About what its gyro reads lying still, deg/s, ls_imu axes: 12 in all,
   4.9 of it about up when flat. */
static const float REST[3] = { -7.4f, -8.1f, 4.9f };

static double wrap180(double a)
{
    double d = fmod(a, 360.0);
    if (d > 180) d -= 360;
    if (d <= -180) d += 360;
    return d;
}

/* The board flat on the bench as COMPASS's frames see it: the field worker
   takes a sample every 100 ms (gyro, accelerometer, and the magnetic
   heading ls_compass_solve finds), and the 40 ms frames read each until
   the next. */
typedef struct {
    ned field;
    float offset[3];      /* added to the magnetometer, its raw axes, uT */
    float rest[3];        /* what the gyro reads lying still */
    float declination;    /* added to the magnetic heading */
    bool loop_only;       /* the fusion's own step, fed the raw turn rate */
    float noise;          /* sensor noise: 1 is about 0.3 uT, 0.15 deg/s, 3 mg */
    ls_rng_t rng;
    double next;
    float heading, g[3], a[3];
    bool back;
} bench_t;

static bench_t bench(ned field, const float offset[3])
{
    bench_t b;
    memset(&b, 0, sizeof(b));
    b.field = field;
    if (offset) memcpy(b.offset, offset, sizeof(b.offset));
    memcpy(b.rest, REST, sizeof(b.rest));
    b.noise = 1.0f;
    ls_rng_seed(&b.rng, 5);
    return b;
}

/* The magnetic heading the board reads pointing at `yaw`, without noise. */
static float bench_heading(const bench_t *b, double yaw)
{
    ls_imu_sample_t s = pose_in(b->field, yaw, 0, 0, b->offset);
    ls_compass_reading_t r; bool back = false;
    return ls_compass_solve(&s, NULL, &back, &r) ? r.magnetic : NAN;
}

/* One 40 ms frame at time t, the board pointing at `yaw` and turning at
   `rate` deg/s clockwise: a sample when one is due, then the fusion as
   scr_compass runs it. */
static float bench_frame(bench_t *b, ls_compass_steady_t *s, double t, double yaw, double rate)
{
    if (t >= b->next - 1e-6) {
        b->next += 0.1;
        ls_imu_sample_t m = pose_in(b->field, yaw, 0, 0, b->offset);
        const float n = b->noise;
        m.mx += n * ls_rng_noise(&b->rng); m.my += n * ls_rng_noise(&b->rng); m.mz += n * ls_rng_noise(&b->rng);
        ls_compass_reading_t r;
        b->heading = ls_compass_solve(&m, NULL, &b->back, &r) ? fmodf(r.magnetic + b->declination + 360.0f, 360.0f) : NAN;
        /* ls_imu's axes: flat and face up, a clockwise turn reads positive
           on z. Full scale is 250 deg/s. */
        const float turn[3] = { 0, 0, (float)rate };
        for (int i = 0; i < 3; i++)
            b->g[i] = fmaxf(-250.1f, fminf(250.1f, turn[i] + b->rest[i] + 0.5f * n * ls_rng_noise(&b->rng)));
        b->a[0] = m.ax + 0.01f * n * ls_rng_noise(&b->rng);
        b->a[1] = m.ay + 0.01f * n * ls_rng_noise(&b->rng);
        b->a[2] = m.az + 0.01f * n * ls_rng_noise(&b->rng);
    }
    if (b->loop_only) return ls_compass_steady_step(s, b->heading, 0, 0, ls_compass_yaw_rate(b->g, b->a), 0.04f);
    return ls_compass_steady_imu(s, b->heading, 0, 0, b->g, b->a, 0.04f);
}

/* A turn of `deg` over `secs` from t0, as a hand makes one: from rest to
   rest, quickest halfway. How far it has gone at t, and how fast. */
static double turn_at(double t, double t0, double secs, double deg, double *rate)
{
    const double two_pi = 6.283185307179586, u = (t - t0) / secs;
    *rate = 0;
    if (u <= 0) return 0;
    if (u >= 1) return deg;
    *rate = deg / secs * (1 - cos(two_pi * u));
    return deg * (u - sin(two_pi * u) / two_pi);
}

/* On USB the charge current puts a fixed offset on the magnetometer. Half
   the horizontal field's worth bends the magnetic heading by up to 30
   degrees, a different way at every heading, so through a flat turn it
   swings tens of degrees away from the turn the gyro follows truly. Out 90
   and back at 60 deg/s, from 24 headings, the first turn 0.6 s after the
   dial opens (the gyro's 4.9 deg/s not yet learnt), at both pulls: no frame
   moves the dial more than 5 degrees beyond what the board turned. */
LS_CASE(a_flat_turn_through_a_bent_field_never_jumps)
{
    const float usb[3] = { 7.75f, 0, 0 };
    float worst = 0, worst_tau = 0;
    int worst_from = 0;
    double worst_t = 0;
    for (int k = 0; k < 2; k++)
        for (int from = 0; from < 360; from += 15) {
            bench_t b = bench(DIP67, usb);
            ls_compass_steady_t s;
            memset(&s, 0, sizeof(s));
            s.mag_tau_s = k ? 8.0f : LS_COMPASS_FUSE_TAU_S;
            double was = NAN;
            float shown = NAN;
            for (int i = 0; i < 390; i++) {                   /* 15.6 s */
                const double t = i * 0.04;
                double r1, r2;
                const double yaw = from + turn_at(t, 0.6, 1.5, 90, &r1) + turn_at(t, 8.1, 1.5, -90, &r2);
                const float h = bench_frame(&b, &s, t, yaw, r1 + r2);
                if (isfinite(shown)) {
                    const float excess = (float)fabs(wrap180(h - shown) - wrap180(yaw - was));
                    if (excess > worst) { worst = excess; worst_tau = s.mag_tau_s; worst_from = from; worst_t = t; }
                }
                shown = h; was = yaw;
            }
        }
    LS_CHECK_MSG(worst <= 5.0f, "the dial moved %.1f deg more than the board in one frame (from %d, pull %.0f s, at %.2f s)",
                 worst, worst_from, worst_tau, worst_t);
}

/* Lying still on USB with the MAG lamp lit (the magnetometer pulls over
   8 s), the gyro's 4.9 deg/s about up is how it reads at rest, not a turn:
   once the board has been seen still the dial does not creep. */
LS_CASE(still_with_the_lamp_on_it_does_not_drift)
{
    const float usb[3] = { 3.0f, -2.0f, -18.0f };      /* the field reads 24 uT of 39.6 */
    bench_t b = bench(DIP67, usb);
    ls_compass_steady_t s;
    memset(&s, 0, sizeof(s));
    s.mag_tau_s = 8.0f;
    const float mag = bench_heading(&b, 40);
    float worst = 0;
    double at = 0;
    for (int i = 0; i < 1538; i++) {                      /* 61.5 s */
        const double t = i * 0.04;
        const float h = bench_frame(&b, &s, t, 40, 0);
        if (t >= 1.5 && off_deg(h, mag) > worst) { worst = off_deg(h, mag); at = t; }
    }
    LS_CHECK_MSG(worst <= 3.0f, "lying still the dial crept %.1f deg from the magnetometer (at %.1f s)", worst, at);
    for (int i = 0; i < 3; i++)
        LS_CHECK_MSG(fabsf(s.rest[i] - REST[i]) < 0.3f, "rest[%d] learnt as %.2f, the gyro reads %.2f", i, s.rest[i], REST[i]);
}

/* Switching between true and magnetic north starts the dial again from the
   new reading. What it has learnt of the gyro is kept, so it does not start
   drifting while that is learnt again. */
LS_CASE(the_bias_survives_a_true_magnetic_switch)
{
    /* The loop's own trim, learnt lying still. */
    ls_compass_steady_t s = { 0 };
    for (int i = 0; i < 750; i++) ls_compass_steady_step(&s, 100, 0, 0, 3.0f, 0.04f);
    LS_CHECK_MSG(fabsf(s.bias - 3.0f) < 0.3f, "bias learnt as %.2f", s.bias);
    s.started = false;                                    /* the switch, as scr_compass makes it */
    float worst = 0;
    for (int i = 0; i < 500; i++)
        worst = fmaxf(worst, off_deg(ls_compass_steady_step(&s, 114.5f, 0, 0, 3.0f, 0.04f), 114.5f));
    LS_CHECK_MSG(fabsf(s.bias - 3.0f) < 0.3f, "bias %.2f after the switch", s.bias);
    LS_CHECK_MSG(worst < 0.5f, "the dial drifted %.2f deg after the switch", worst);

    /* The gyro's rest reading, learnt lying still, the same. Without noise,
       so the reading the restart takes is the one it should hold. */
    bench_t b = bench(DIP67, NULL);
    b.noise = 0;
    ls_compass_steady_t q;
    memset(&q, 0, sizeof(q));
    for (int i = 0; i < 125; i++) bench_frame(&b, &q, i * 0.04, 100, 0);    /* 5 s */
    q.started = false;
    b.declination = 14.5f;
    const float want = fmodf(bench_heading(&b, 100) + 14.5f, 360.0f);
    worst = 0;
    for (int i = 125; i < 625; i++) worst = fmaxf(worst, off_deg(bench_frame(&b, &q, i * 0.04, 100, 0), want));
    LS_CHECK_MSG(worst < 0.5f, "the dial drifted %.2f deg after the switch", worst);
}

/* Through a turn the magnetometer, bent by the USB current, swings away
   from the turn the gyro follows. That is not the gyro's bias, and the loop
   does not learn it as one. */
LS_CASE(a_turn_through_a_bent_field_does_not_teach_bias)
{
    const float usb[3] = { 7.75f, 0, 0 };
    float worst = 0;
    int at = 0;
    for (int from = 0; from < 360; from += 15) {
        bench_t b = bench(DIP67, usb);
        b.loop_only = true;
        ls_compass_steady_t s;
        memset(&s, 0, sizeof(s));
        int i = 0;
        for (; i < 750; i++) bench_frame(&b, &s, i * 0.04, from, 0);       /* 30 s still */
        const float before = s.bias;
        LS_CHECK_MSG(fabsf(before - REST[2]) < 0.5f, "from %d the loop learnt %.2f lying still", from, before);
        for (; i <= 788; i++) {                                              /* to the end of the turn */
            double r;
            const double yaw = from + turn_at(i * 0.04, 30.0, 1.5, 90, &r);
            bench_frame(&b, &s, i * 0.04, yaw, r);
        }
        if (fabsf(s.bias - before) > worst) { worst = fabsf(s.bias - before); at = from; }
    }
    LS_CHECK_MSG(worst < 0.5f, "a turn from %d taught the loop %.2f deg/s of bias", at, worst);
}

/* Spun faster than the gyro's 250 deg/s full scale, the gyro reads too
   small a turn and the dial falls behind. The magnetometer has taken back
   what the gyro lost within a second of the board coming to rest, what the
   spin did is not learnt as the gyro's bias, and the dial then holds. */
LS_CASE(a_spin_past_gyro_full_scale_still_recovers)
{
    bench_t b = bench(DIP67, NULL);
    ls_compass_steady_t s;
    memset(&s, 0, sizeof(s));
    /* Still; a quarter turn, so the gyro is seen turning with the
       magnetometer; still; two turns peaking at 400 deg/s; still. */
    const double spin_at = 7.5, spin_s = 3.6, stop = spin_at + spin_s;
    float soon = 0, worst = 0, trim = NAN, kicked = 0;
    double at = 0;
    for (int i = 0; i < (int)((stop + 10.0) / 0.04); i++) {
        const double t = i * 0.04;
        double r1, r2;
        const double yaw = 30 + turn_at(t, 3.0, 1.5, 90, &r1) + turn_at(t, spin_at, spin_s, 720, &r2);
        const float off = (float)fabs(wrap180(bench_frame(&b, &s, t, yaw, r1 + r2) - yaw));
        if (t >= spin_at && !isfinite(trim)) trim = s.bias;
        if (t >= spin_at && t <= stop) kicked = fmaxf(kicked, fabsf(s.bias - trim));
        if (t >= stop + 1.0 && t < stop + 1.5) soon = fmaxf(soon, off);
        if (t >= stop + 5.0 && off > worst) { worst = off; at = t - stop; }
    }
    LS_CHECK_MSG(soon < 45.0f, "a second after the spin the dial was %.0f deg out", soon);
    LS_CHECK_MSG(kicked < 0.5f, "the spin moved the gyro's trim by %.2f deg/s", kicked);
    LS_CHECK_MSG(worst < 2.0f, "%.1f deg out %.1f s after the spin", worst, at);
}

/* A frame the field worker has no IMU sample for, with no position known,
   reads magnetic as every other frame does: a declination of 0 would say
   true north is known and switch the dial to true and back. */
extern ls_field_sample_t ls_stub_field_sample;
LS_CASE(a_frame_without_the_imu_keeps_magnetic_north)
{
    memset(&ls_stub_field_sample, 0, sizeof(ls_stub_field_sample));
    ls_stub_field_sample.imu_valid = true;
    ls_stub_field_sample.imu = pose(30, 0, 0, NULL);
    ls_compass_reading_t r;
    ls_compass_live(&r);
    LS_CHECK(r.valid);
    LS_CHECK(!isfinite(r.declination));
    ls_stub_field_sample.imu_valid = false;
    ls_compass_live(&r);
    LS_CHECK(!r.valid);
    LS_CHECK_MSG(!isfinite(r.declination), "declination %.1f with no position known", r.declination);
    memset(&ls_stub_field_sample, 0, sizeof(ls_stub_field_sample));
}
