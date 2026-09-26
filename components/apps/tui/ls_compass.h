#ifndef LS_COMPASS_H
#define LS_COMPASS_H

#include <stdbool.h>
#include <stdint.h>
#include "ls_imu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* corrected = soft * (m - offset), in the stored magnetic basis, then mapped
   to screen axes by `basis`: screen axis i = sign(basis[i]) * stored axis
   |basis[i]| - 1. An all-zero soft matrix means none (a sphere fit); an
   all-zero basis means the default {-my, -mx, -mz}. */
typedef struct {
    float offset[3], radius, error;
    float soft[3][3];
    int8_t basis[3];
    float dip_spread;    /* degrees across the fit's samples */
} ls_compass_cal_t;

/* Six held faces, then a figure eight that fills the directions a hard-iron
   sphere cannot, so the fit can also take out soft iron. */
#define LS_COMPASS_FACE_STEPS   6
#define LS_COMPASS_STEPS        7
#define LS_COMPASS_KEEP         704
#define LS_COMPASS_COVER_BINS   26
#define LS_COMPASS_COVER_NEEDED 20
#define LS_COMPASS_TUMBLE_MAX   450   /* 45 s at 10 Hz, then finish anyway */

typedef struct {
    double normal[4][5], squares;
    uint32_t samples;
    uint8_t faces;
    uint8_t bucket_samples[8];
    bool tumble;
    float center[3];
    uint32_t cover;
    uint16_t tumble_seen;
    uint16_t kept;
    float keep[LS_COMPASS_KEEP][6];   /* mx my mz ax ay az */
} ls_compass_fit_t;

#define LS_COMPASS_HOLD_SAMPLES 20
typedef struct {
    uint8_t step, hold;
    int8_t up_face;
    bool aligned;
} ls_compass_guide_t;

void ls_compass_guide_begin(ls_compass_guide_t *guide);
/* Called once per fresh 100 ms reading; NULL breaks the current hold. */
void ls_compass_guide_sample(ls_compass_guide_t *guide, const ls_imu_sample_t *sample);

void ls_compass_begin(ls_compass_fit_t *fit);
void ls_compass_collect(ls_compass_fit_t *fit, const ls_imu_sample_t *sample);
/* After the six faces: fixes an interim centre so coverage can be judged.
   False when the faces alone do not fit, which fails the calibration. */
bool ls_compass_tumble_begin(ls_compass_fit_t *fit);
int  ls_compass_tumble_cover(const ls_compass_fit_t *fit);
bool ls_compass_tumble_done(const ls_compass_fit_t *fit);
bool ls_compass_finish(const ls_compass_fit_t *fit, ls_compass_cal_t *cal);
bool ls_compass_cal_valid(const ls_compass_cal_t *cal);
/* What makes a calibration unusable, or NULL when it is fine. */
const char *ls_compass_cal_problem(const ls_compass_cal_t *cal);
/* The accelerometer in true screen axes. ls_imu's accelerometer has x and z
   reversed on the T-Display-P4 (flat face up reads z -1, right edge down
   reads x +1); auto-rotation is built on that, so it is corrected here, for
   the compass only, as measured on the board. */
void ls_compass_gravity(const ls_imu_sample_t *sample, float out[3]);
/* The corrected field in screen axes (x right, y up, z out of the glass). */
bool ls_compass_field(const ls_imu_sample_t *sample, const ls_compass_cal_t *cal, float out[3]);
float ls_compass_heading(const ls_imu_sample_t *sample, const ls_compass_cal_t *cal);
float ls_compass_ease(float current, float target, float fraction);

#ifdef __cplusplus
}
#endif
#endif
