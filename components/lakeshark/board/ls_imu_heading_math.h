#ifndef LS_IMU_HEADING_MATH_H
#define LS_IMU_HEADING_MATH_H

#include <math.h>

/* Keep the existing magnetic sample basis so saved hard-iron offsets remain
   valid. On the P4, -mx is toward the physical top and +my toward the left;
   these are not the accelerometer's screen axes. Heading increases clockwise
   from the physical top, independent of the display's current rotation. */
static inline float ls_imu_magnetic_heading(float mx, float my)
{
    if (!isfinite(mx) || !isfinite(my) || mx*mx + my*my < 1) return NAN;
    float degrees = atan2f(my, -mx) * 57.295779513f;
    return degrees < 0 ? degrees + 360 : degrees;
}

#endif
