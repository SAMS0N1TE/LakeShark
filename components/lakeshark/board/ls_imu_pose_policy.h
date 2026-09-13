#pragma once
#include "ls_imu.h"
#include <math.h>

static inline ls_imu_pose_t ls_imu_classify_pose(const ls_imu_sample_t *s)
{
    float x = fabsf(s->ax), y = fabsf(s->ay), z = fabsf(s->az);
    float gravity = x*x + y*y + z*z;
    if (!isfinite(gravity) || gravity < .5625f || gravity > 1.5625f || z > .75f)
        return LS_IMU_FLAT;
    /* A diagonal grip and movement are not deliberate orientation changes. */
    if (fabsf(s->gx) > 60 || fabsf(s->gy) > 60 || fabsf(s->gz) > 60)
        return LS_IMU_FLAT;
    if (y > .65f && y > x * 1.35f)
        return s->ay > 0 ? LS_IMU_UP : LS_IMU_DOWN;
    if (x > .65f && x > y * 1.35f)
        return s->ax > 0 ? LS_IMU_RIGHT : LS_IMU_LEFT;
    return LS_IMU_FLAT;
}
