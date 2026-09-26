/* The compass as the rest of the firmware reads it: tilt-compensated,
   corrected to true north by the World Magnetic Model when there is a
   position, and checked against the field that model expects so a reading
   bent by nearby steel says so. Also the one navigation target, set from a
   note or the map and followed by COMPASS. */

#ifndef LS_COMPASS_LIVE_H
#define LS_COMPASS_LIVE_H

#include <stdbool.h>
#include <stddef.h>
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

/* A heading held steady at rest and quick in a turn. A change of a
   degree and a half is noise and is followed over about a second and a half;
   one past four degrees is a turn and is followed within a tenth of a
   second; between the two the time blends. The gyro says a turn has begun
   before the heading has moved far: past 4 deg/s the heading is followed
   within 30 ms, so a slow sweep is not read late. Pitch and roll are smoothed
   over half a second. Angles in degrees, rate in deg/s (NAN unknown), dt
   in seconds. */
typedef struct { bool started; float heading, pitch, roll; } ls_compass_steady_t;
float ls_compass_steady_step(ls_compass_steady_t *s, float heading, float pitch, float roll,
                             float rate_dps, float dt);

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
