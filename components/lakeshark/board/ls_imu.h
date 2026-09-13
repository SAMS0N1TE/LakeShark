/* The 9-axis sensor: which way up the board is, and which way it is pointing. */

#ifndef LS_IMU_H
#define LS_IMU_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "ls_board.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Acceleration in g, board axes. At rest the vector is gravity and its
       magnitude is about 1.0 - which is also the cheapest health check
       there is on a sensor that is reading. */
    float ax, ay, az;
    /* Angular rate in degrees per second. Near zero when still. */
    float gx, gy, gz;
    /* Magnetic flux in microtesla, or all zero when the magnetometer did not
       answer. Earth's field is roughly 25 to 65 uT depending where you are,
       so a magnitude far outside that means something ferrous is close - or
       the part needs calibrating. */
    /* Magnetic calibration basis differs from acceleration on the P4:
       -mx is toward the physical top, +my toward the left. */
    float mx, my, mz;
    bool  mag_valid;
    /* Die temperature in Celsius. The ICM's own, not the room's: it sits
       next to a transmitter and reads high. */
    float temp_c;
} ls_imu_sample_t;

typedef enum {
    LS_IMU_FLAT = 0,
    LS_IMU_UP,          /* portrait, the normal way round */
    LS_IMU_DOWN,        /* portrait, upside down          */
    LS_IMU_LEFT,        /* landscape                      */
    LS_IMU_RIGHT,       /* landscape, the other way       */
} ls_imu_pose_t;

/* Bring the sensor up: identity check, reset, wake, and the magnetometer
   behind it. Safe to call twice. ESP_ERR_NOT_FOUND when nothing answers,
   which on a board that may be assembled without one is not a fault. */
esp_err_t ls_imu_start(void);

/* True once the identity register has answered with the right value. Nothing
   else in this header is meaningful until it does. */
bool ls_imu_present(void);

/* One reading, straight off the part. Returns false when the sensor is
   absent or the bus refused. Cached briefly: the bus is shared with the
   touch controller, which is polled at 10 ms and matters more. */
bool ls_imu_read(ls_imu_sample_t *out);

/* The pose, with hysteresis already applied.

   A raw threshold on the accelerometer flickers between two answers whenever
   the board is held near a diagonal, and a screen that re-lays itself out
   twice a second is worse than one that never rotates. A new pose has to be
   held before this reports it; until then it keeps saying the old one. */
ls_imu_pose_t ls_imu_pose(void);

float ls_imu_heading(void);

#ifdef __cplusplus
}
#endif

#endif /* LS_IMU_H */
