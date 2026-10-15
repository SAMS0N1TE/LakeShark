/* The compass as the rest of the firmware reads it: tilt-compensated,
   corrected to true north by the World Magnetic Model when there is a
   position, and checked against the field that model expects so a reading
   bent by nearby steel says so. Also the one navigation target, set from a
   note or the map and followed by COMPASS. */

#ifndef LS_COMPASS_LIVE_H
#define LS_COMPASS_LIVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ls_imu.h"
#include "ls_compass.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Field quality is independent of GPS availability. Unknown dip is omitted,
   never replaced by zero; consumers must also check valid/low_confidence. */
typedef enum { LS_COMPASS_UNCALIBRATED, LS_COMPASS_GOOD, LS_COMPASS_DISTURBED } ls_compass_quality_t;
ls_compass_quality_t ls_compass_quality(bool calibrated, float field_ut, float cal_ut,
                                       float dip, float expected_dip);
const char *ls_compass_quality_name(ls_compass_quality_t quality);

typedef struct {
    ls_compass_quality_t quality;
    bool valid;          /* a heading could be computed */
    bool low_confidence; /* view indeterminate: hold last heading, do not navigate */
    bool calibrated;     /* hard-iron calibration applied */
    bool true_valid;     /* declination known (position and date) */
    bool back_axis;      /* upright: the heading is where the back faces */
    bool interference;   /* field strength or dip far from the model; from
                            ls_compass_live it is debounced (ls_compass_bend_step) */
    float strength_off;  /* measured over expected strength, minus one */
    float dip_off;       /* measured minus expected dip, degrees */
    float bend;          /* this sample's worst deviation over its limit: 1 is
                            at the limit, NAN when there is nothing to judge */
    bool model;          /* expected field known */
    bool position_saved; /* model from the last saved fix or HOME, not a live one */
    float magnetic, true_deg, declination;
    float tilt;          /* screen away from flat, degrees */
    float pitch, roll;   /* top up / right edge down, degrees */
    float dip;           /* measured inclination, down positive */
    float field_ut;      /* measured strength */
    float cal_ut;        /* the strength the calibration measured, or NAN */
    float expected_ut, expected_dip;
    bool learnt;         /* the learner's drift and current terms are taken off */
    float sigma_deg;     /* the heading's 1-sigma from the learner, NAN when unknown */
} ls_compass_reading_t;

/* Pure core: one IMU sample in, a reading out. `back` carries the
   upright/flat choice between calls so it does not flicker at the edge. */
bool ls_compass_solve(const ls_imu_sample_t *s, const ls_compass_cal_t *cal,
                      bool *back, ls_compass_reading_t *out);
/* Adds declination and the interference check from a model field. */
void ls_compass_apply_model(ls_compass_reading_t *r, double declination,
                            double expected_ut, double expected_dip);

/* The interference warning over time. One sample judges nothing: the
   field jitters by a few percent and the dip by a few degrees, and a flag
   taken per sample flickered at the limit. The score is smoothed over about
   a second; the warning comes on after 2 s over the limit and goes off after
   3 s under three quarters of it. */
typedef struct { bool primed, on; float score; int64_t last_us, edge_us; } ls_compass_bend_t;
bool ls_compass_bend_step(ls_compass_bend_t *b, float bend, int64_t now_us);

/* Complementary heading core. steady_step accepts an already qualified yaw;
   production callers use steady_sample once per sensor observation. It gates
   yaw on independent accel/magnetic stillness, a converged three-axis bias,
   <=1 dps per-axis rest SD, saturation, and two-second magnetic agreement.
   A rejected/unconverged gyro falls back to smoothed magnetic heading.
   Angles are degrees, rates deg/s, dt seconds. Clearing started resets the
   heading reference while preserving sensor health and rest calibration. */
#define LS_COMPASS_FUSE_TAU_S 1.0f
#define LS_COMPASS_JUMP_S     0.3f
#define LS_COMPASS_COAST_S    3.0f
#define LS_COMPASS_PULL_DPS   15.0f
#define LS_COMPASS_STILL_S    2.5f
#define LS_COMPASS_LEARN_DPS  10.0f
#define LS_COMPASS_SETTLE_S   0.5f
typedef struct {
    bool started;
    float heading, pitch, roll;
    float bias;      /* learnt gyro bias about up, deg/s, after the rest reading */
    float agree;     /* -1..+1: gyro and magnetometer turn the same way */
    float prev_mag;  /* last magnetic input, for agree */
    float far_s;     /* how long the magnetometer has been over 45 deg away */
    float win_s, win_gyro, win_mag;   /* the current agree window */
    uint32_t glitches;                /* single readings refused as jumps */
    bool gyro_used;  /* the last step was turned by the gyro */
    float coast_s;   /* how long the gyro alone has carried it */
    float slow_s;    /* how long it has turned slower than LS_COMPASS_LEARN_DPS */
    float mag_tau_s; /* how slowly the magnetometer pulls; 0 is LS_COMPASS_FUSE_TAU_S.
                        Longer while the field is bent (the MAG lamp): the gyro carries
                        the heading, and a lasting reading is never taken in one step. */
    /* Kept by ls_compass_steady_imu. */
    float rest[3];   /* the gyro's rest reading, deg/s, ls_imu axes; 0 until learnt */
    bool rest_known;
    bool resting;    /* a still spell is under way */
    float still_s;   /* how long it has lasted */
    float still_g[3], still_a[3];     /* the gyro's and accelerometer's means over it */
    float sat_s;     /* a gyro axis was at full scale within this */
    float yaw;       /* the turn rate last used, deg/s, NAN when none */
    float gyro_var[3], rest_heading;
    bool observed, stationary, gyro_healthy;
    float observe_s, anchor_m[3], mean_m[3], mean_a[3], var_m[3], var_a[3];
    float motion_s, check_s, check_gyro, check_mag, check_heading;
} ls_compass_steady_t;
float ls_compass_steady_step(ls_compass_steady_t *s, float heading, float pitch, float roll,
                             float yaw_dps, float dt);

/* Vector wrapper for callers without magnetic vectors. Rest then requires
   a stable magnetic heading as well as acceleration. Prefer steady_sample:
   it observes accel/mag variance and freezes output at confirmed rest. */
#define LS_COMPASS_REST_USE_S  0.3f
#define LS_COMPASS_REST_S      2.0f
#define LS_COMPASS_STEADY_G    0.03f
#define LS_COMPASS_GYRO_FS_DPS 250.0f
#define LS_COMPASS_MOVE_UT2    49.0f   /* a fresh sample 7 uT off the rest anchor is movement */
#define LS_COMPASS_SAT_HOLD_S  1.0f
float ls_compass_steady_imu(ls_compass_steady_t *s, float heading, float pitch, float roll,
                            const float g[3], const float a[3], float dt);
/* Call once per fresh sensor sample, using the unslewed magnetic vector. */
float ls_compass_steady_sample(ls_compass_steady_t *s, float heading, float pitch, float roll,
                               const ls_imu_sample_t *sample, float dt);

/* Clockwise rate of the gravity-levelled top/back view, deg/s, using
   gyro and acceleration in ls_imu axes. NAN outside 0.5..1.5 g or when
   the right axis is within 10 degrees of vertical (view indeterminate). */
float ls_compass_yaw_rate(const float g[3], const float a[3]);

/* Live, from the field worker's latest sample and the GPS. */
void ls_compass_live(ls_compass_reading_t *out);

/* Great circle: initial true bearing and distance in metres. */
void ls_compass_nav(double lat1, double lon1, double lat2, double lon2,
                    double *bearing, double *metres);

void ls_compass_set_target(double lat, double lon, const char *name);
void ls_compass_clear_target(void);
bool ls_compass_target(double *lat, double *lon, char *name, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
