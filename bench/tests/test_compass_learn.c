#include "ls_test.h"
#include "ls_compass.h"
#include "ls_compass_learn.h"
#include <math.h>
#include <string.h>

#define RAD 0.017453292519943295

typedef struct { double n, e, d; } ned;
static ned cross3(ned a, ned b) { return (ned){ a.e * b.d - a.d * b.e, a.d * b.n - a.n * b.d, a.n * b.e - a.e * b.n }; }
static double dot3(ned a, ned b) { return a.n * b.n + a.e * b.e + a.d * b.d; }

/* 20 uT north, 48 down: R 52, dip 67, about New England. */
static const ned FIELD = { 20, 0, 48 };

/* The board with its top at `yaw`, raised by `pitch`, rolled by `roll`, and
   `extra` (screen axes, uT) added to what the magnetometer reads, as a
   drifted offset or a current near the sensor would. */
static ls_imu_sample_t pose(double yaw, double pitch, double roll, const float extra[3],
                            const ls_compass_cal_t *cal)
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
    s.ax = (float)-dot3(upward, right); s.ay = (float)dot3(upward, top); s.az = (float)-dot3(upward, out);
    float scr[3] = { (float)dot3(FIELD, right), (float)dot3(FIELD, top), (float)dot3(FIELD, out) };
    if (extra) for (int i = 0; i < 3; i++) scr[i] += extra[i];
    float raw[3];
    ls_compass_unfield(cal, scr, raw);
    s.mx = raw[0] + cal->offset[0]; s.my = raw[1] + cal->offset[1]; s.mz = raw[2] + cal->offset[2];
    s.mag_valid = true;
    return s;
}

static ls_compass_cal_t plain_cal(void)
{
    ls_compass_cal_t c;
    memset(&c, 0, sizeof(c));
    c.offset[0] = -25; c.offset[1] = -21; c.offset[2] = -97;
    c.radius = 52.0f; c.error = 0.02f;
    return c;
}

static float heading_of(const ls_imu_sample_t *s, const ls_compass_cal_t *cal, const float corr[3])
{
    ls_imu_sample_t t = *s;
    if (corr) {
        float raw[3];
        ls_compass_unfield(cal, corr, raw);
        t.mx -= raw[0]; t.my -= raw[1]; t.mz -= raw[2];
    }
    float f[3], up[3];
    ls_compass_field(&t, cal, f);
    ls_compass_gravity(&t, up);
    /* Flat: the top's angle clockwise from the field's horizontal part. */
    return (float)fmod(atan2(-f[0], f[1]) / RAD + 720.0, 360.0);
}

static float off_deg(float a, float b) { return fabsf(fmodf(a - b + 540.0f, 360.0f) - 180.0f); }

static void feed(ls_compass_learn_t *l, const ls_imu_sample_t *s, const ls_compass_cal_t *cal, float amps)
{
    float f[3], up[3];
    ls_compass_field(s, cal, f);
    ls_compass_gravity(s, up);
    ls_compass_learn_step(l, f, up, 20.0f, amps, 0.1f);
}

LS_CASE(unfield_undoes_field_with_soft_iron)
{
    ls_compass_cal_t c = plain_cal();
    c.soft[0][0] = 1.1f; c.soft[1][1] = 0.92f; c.soft[2][2] = 1.0f; c.soft[0][1] = c.soft[1][0] = 0.05f;
    const float want[3] = { 3, -7, 11 };
    float raw[3];
    LS_CHECK(ls_compass_unfield(&c, want, raw));
    ls_imu_sample_t a = { .mx = 10, .my = 20, .mz = 30, .mag_valid = true }, b = a;
    b.mx -= raw[0]; b.my -= raw[1]; b.mz -= raw[2];
    float fa[3], fb[3];
    ls_compass_field(&a, &c, fa); ls_compass_field(&b, &c, fb);
    for (int i = 0; i < 3; i++) LS_NEAR(fa[i] - fb[i], want[i], 1e-3);
}

/* A drifted offset the calibration does not know about bends a flat
   heading by up to 25 degrees here; two slow turns teach it away. */
LS_CASE(a_drifted_offset_is_learnt_in_two_turns)
{
    ls_compass_cal_t cal = plain_cal();
    const float drift[3] = { 8, -6, 4 };
    float worst_before = 0;
    for (int yaw = 0; yaw < 360; yaw += 10) {
        ls_imu_sample_t s = pose(yaw, 0, 0, drift, &cal);
        worst_before = fmaxf(worst_before, off_deg(heading_of(&s, &cal, NULL), (float)yaw));
    }
    LS_CHECK_MSG(worst_before > 15, "the drift should matter: %.1f", worst_before);
    ls_compass_learn_t l;
    ls_compass_learn_reset(&l, cal.radius);
    ls_rng_t rng; ls_rng_seed(&rng, 7);
    for (int i = 0; i < 240; i++) {
        const double yaw = i * 3.0;                 /* 30 deg/s at 10 Hz */
        const double pitch = 8 * sin(i * 0.05), roll = 6 * cos(i * 0.07);
        ls_imu_sample_t s = pose(yaw, pitch, roll, drift, &cal);
        s.mx += ls_rng_noise(&rng) * 2.8f; s.my += ls_rng_noise(&rng) * 2.8f;
        s.mz += ls_rng_noise(&rng) * 2.8f;
        feed(&l, &s, &cal, NAN);
    }
    float corr[3];
    LS_CHECK(ls_compass_learn_correction(&l, NAN, corr));
    float worst = 0;
    for (int yaw = 0; yaw < 360; yaw += 10) {
        ls_imu_sample_t s = pose(yaw, 0, 0, drift, &cal);
        worst = fmaxf(worst, off_deg(heading_of(&s, &cal, corr), (float)yaw));
    }
    LS_CHECK_MSG(worst < 3.0f, "worst %.2f deg after learning (was %.1f), corr %.1f %.1f %.1f",
                 worst, worst_before, corr[0], corr[1], corr[2]);
    const float acc = ls_compass_learn_accuracy(&l, NAN);
    LS_CHECK_MSG(acc > 0.2f && acc < 5.0f, "accuracy %.2f", acc);
}

/* Held still, the filter must not turn the heading: what it learns from
   one direction lies along the field it reads. */
LS_CASE(held_still_it_does_not_turn_the_heading)
{
    ls_compass_cal_t cal = plain_cal();
    const float drift[3] = { 5, 5, -5 };
    ls_compass_learn_t l;
    ls_compass_learn_reset(&l, cal.radius);
    ls_imu_sample_t s = pose(40, 2, 1, drift, &cal);
    const float before = heading_of(&s, &cal, NULL);
    for (int i = 0; i < 600; i++) feed(&l, &s, &cal, NAN);
    float corr[3];
    /* Nothing is applied: only the part along this one field is known, and
       taking off a guess for the rest would bend the heading once turned. */
    LS_CHECK(!ls_compass_learn_correction(&l, NAN, corr));
    for (int yaw = 0; yaw < 360; yaw += 30) {
        ls_imu_sample_t t = pose(yaw, 0, 0, drift, &cal);
        LS_CHECK(off_deg(heading_of(&t, &cal, corr), heading_of(&t, &cal, NULL)) < 0.01f);
    }
    LS_CHECK_MSG(off_deg(heading_of(&s, &cal, corr), before) < 0.5f, "moved %.2f",
                 off_deg(heading_of(&s, &cal, corr), before));
}

/* Plugging in: the charge current adds a fixed vector. After the filter has
   seen the current change during turns, a later plug-in does not step the
   heading. */
LS_CASE(the_charge_current_offset_is_learnt)
{
    ls_compass_cal_t cal = plain_cal();
    const float per_amp[3] = { 30, -20, 10 };        /* 0.5 A: 18 uT */
    ls_compass_learn_t l;
    ls_compass_learn_reset(&l, cal.radius);
    ls_rng_t rng; ls_rng_seed(&rng, 11);
    for (int i = 0; i < 1200; i++) {
        const float amps = (i / 150) % 2 ? 0.5f : -0.25f;   /* charging, then on battery */
        float extra[3];
        for (int k = 0; k < 3; k++) extra[k] = per_amp[k] * amps;
        ls_imu_sample_t s = pose(i * 3.0, 6 * sin(i * 0.05), 4 * cos(i * 0.03), extra, &cal);
        s.mx += ls_rng_noise(&rng) * 2.8f; s.my += ls_rng_noise(&rng) * 2.8f;
        s.mz += ls_rng_noise(&rng) * 2.8f;
        feed(&l, &s, &cal, amps);
    }
    for (int k = 0; k < 3; k++)
        LS_CHECK_MSG(fabsf(l.x[3 + k] - per_amp[k]) < 5.0f, "k[%d] %.1f want %.1f", k, l.x[3 + k], per_amp[k]);
    float worst = 0;
    for (int yaw = 0; yaw < 360; yaw += 15) {
        float extra[3];
        for (int k = 0; k < 3; k++) extra[k] = per_amp[k] * 0.6f;
        ls_imu_sample_t s = pose(yaw, 0, 0, extra, &cal);
        float corr[3];
        ls_compass_learn_correction(&l, 0.6f, corr);
        worst = fmaxf(worst, off_deg(heading_of(&s, &cal, corr), (float)yaw));
    }
    LS_CHECK_MSG(worst < 3.0f, "worst %.2f deg at 0.6 A", worst);
}

/* A magnet passing is refused, not learnt. */
LS_CASE(a_passing_magnet_is_refused)
{
    ls_compass_cal_t cal = plain_cal();
    ls_compass_learn_t l;
    ls_compass_learn_reset(&l, cal.radius);
    for (int i = 0; i < 200; i++) {
        ls_imu_sample_t s = pose(i * 3.0, 3, 0, NULL, &cal);
        feed(&l, &s, &cal, NAN);
    }
    const float magnet[3] = { 60, 0, 30 };
    const uint32_t refused = l.refused;
    for (int i = 0; i < 20; i++) {
        ls_imu_sample_t s = pose(90, 3, 0, magnet, &cal);
        feed(&l, &s, &cal, NAN);
    }
    LS_CHECK(l.refused >= refused + 19);
    float corr[3];
    ls_compass_learn_correction(&l, NAN, corr);
    LS_CHECK_MSG(sqrtf(corr[0] * corr[0] + corr[1] * corr[1] + corr[2] * corr[2]) < 1.5f,
                 "corr %.1f %.1f %.1f", corr[0], corr[1], corr[2]);
}

/* A step the filter has no current to explain (USB power it cannot see)
   is refused at first, then relearnt once it has lasted. */
LS_CASE(a_lasting_step_is_relearnt)
{
    ls_compass_cal_t cal = plain_cal();
    ls_compass_learn_t l;
    ls_compass_learn_reset(&l, cal.radius);
    for (int i = 0; i < 300; i++) {
        ls_imu_sample_t s = pose(i * 3.0, 4 * sin(i * 0.1), 0, NULL, &cal);
        feed(&l, &s, &cal, NAN);
    }
    const float step[3] = { 14, -10, 6 };
    for (int i = 0; i < 600; i++) {
        ls_imu_sample_t s = pose(i * 3.0, 4 * sin(i * 0.1), 3 * cos(i * 0.07), step, &cal);
        feed(&l, &s, &cal, NAN);
    }
    LS_CHECK(l.relearns >= 1);
    float corr[3];
    ls_compass_learn_correction(&l, NAN, corr);
    for (int k = 0; k < 3; k++) LS_CHECK_MSG(fabsf(corr[k] - step[k]) < 3.0f, "corr[%d] %.1f want %.1f", k, corr[k], step[k]);
}
