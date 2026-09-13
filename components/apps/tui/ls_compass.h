#ifndef LS_COMPASS_H
#define LS_COMPASS_H

#include <stdbool.h>
#include <stdint.h>
#include "ls_imu.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { float offset[3], radius, error; } ls_compass_cal_t;
typedef struct {
    double normal[4][5], squares;
    uint32_t samples;
    uint8_t faces;
    uint8_t bucket_samples[7];
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
bool ls_compass_finish(const ls_compass_fit_t *fit, ls_compass_cal_t *cal);
bool ls_compass_cal_valid(const ls_compass_cal_t *cal);
float ls_compass_heading(const ls_imu_sample_t *sample, const ls_compass_cal_t *cal);
float ls_compass_ease(float current, float target, float fraction);

#ifdef __cplusplus
}
#endif
#endif
