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

typedef struct {
    bool valid;          /* a heading could be computed */
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

/* A heading held steady at rest and quick in a turn: the gyro turns it and
   the magnetometer pulls it back over LS_COMPASS_FUSE_TAU_S.

   2.5.0 filtered the magnetic heading alone at one of two speeds, 1.5 s
   for a small change and 30 ms once the gyro passed 4 deg/s, and then ran
   it through the dial's spring. The operator reported it "sluggish, and
   moves oddly": each correction jumped most of the way, overshot on the
   spring and crawled the last degree and a half. A gyro turns the heading
   the moment the board turns and does not jitter; the magnetometer knows
   north and does. Each covers the other's fault, so nothing needs a gate.

   yaw_dps is clockwise-from-above, the gyro's rate about the accelerometer's
   up (see ls_compass_yaw_rate), NAN when unknown. The gyro's own bias about
   that axis is learnt from the magnetometer's pull (a PI loop), so it
   follows the board as it is tilted and as it warms.

   `agree` watches whether the gyro and the magnetometer turn the same way
   in a real turn. It starts at 0 and heads to +1. If it ever passes -0.5
   the gyro is left out and the heading follows the magnetometer alone:
   that is an axis fault somewhere, and the console says so rather than
   the dial quietly spinning against the turn.

   A magnetic reading over 45 degrees from the fused heading is a jump (the
   upright/flat axis switch, true/magnetic, a calibration) only once it has
   stayed there for LS_COMPASS_JUMP_S; until then it is a glitch and the
   gyro alone carries the heading. Taking every such reading at once made
   one bad sample throw the dial across and straight back.

   `agree` votes once per half second of turning: the gyro's integrated
   turn against the magnetometer's, each over 8 degrees. A gyro turning
   the wrong way is out within about a second of turning.

   Pitch and roll are smoothed over 0.2 s. Angles in degrees, dt in
   seconds. */
#define LS_COMPASS_FUSE_TAU_S 1.0f
#define LS_COMPASS_JUMP_S     0.3f
#define LS_COMPASS_COAST_S    3.0f
typedef struct {
    bool started;
    float heading, pitch, roll;
    float bias;      /* learnt gyro bias about up, deg/s */
    float agree;     /* -1..+1: gyro and magnetometer turn the same way */
    float prev_mag;  /* last magnetic input, for agree */
    float far_s;     /* how long the magnetometer has been over 45 deg away */
    float win_s, win_gyro, win_mag;   /* the current agree window */
    uint32_t glitches;                /* single readings refused as jumps */
    bool gyro_used;  /* the last step was turned by the gyro */
    float coast_s;   /* how long the gyro alone has carried it */
    float mag_tau_s; /* how slowly the magnetometer pulls; 0 is LS_COMPASS_FUSE_TAU_S.
                        Longer while the field is bent, so the gyro carries the heading. */
} ls_compass_steady_t;
float ls_compass_steady_step(ls_compass_steady_t *s, float heading, float pitch, float roll,
                             float yaw_dps, float dt);

/* Clockwise-from-above turn rate, deg/s: minus the gyro's component along
   the accelerometer's reaction (up). Both are ls_imu's axes, and a dot
   product does not care which proper rotation turns those into the
   screen's, so this needs no knowledge of the mounting. NAN when the
   accelerometer is not near 1 g. */
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
