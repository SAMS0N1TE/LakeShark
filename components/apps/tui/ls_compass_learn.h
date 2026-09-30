/* The compass calibration, kept up to date while the compass is used.

   A calibration is measured once, and the board it measured does not stay
   the same: the charge current on USB, the backlight and the radios all
   put current through traces near the magnetometer, and a pocket magnet or
   a warm sensor moves the hard-iron offset. Each of these shifts the whole
   field by a fixed vector in the board's frame, which is exactly what bends
   a heading.

   Every calibrated reading is checked against two facts that hold wherever
   the board points: the field's horizontal part is sqrt(R^2 - Z^2) long and
   its downward part is Z, where R is the calibration's radius and Z the
   local vertical field. A Kalman filter over seven numbers finds what makes
   those facts true:

     d[3]  the offset the calibration has drifted by, uT, screen axes
     k[3]  the extra offset per ampere of battery current, uT/A
     Z     the local vertical field, uT, down positive

   so the field used for the heading is f - d - k * amps. A board held still
   teaches only the part of d along the field it reads, which moves its
   magnitude and not its direction; a turn teaches the rest. The current
   term is only learnt when the current changes (plugging in, a transmit,
   the backlight), and after that a step in current no longer steps the
   heading.

   A reading far outside what the filter expects is refused (a magnet
   passing, steel close by). If readings keep being refused for
   LS_CL_RELEARN_S, the offset's uncertainty is widened so the filter can
   follow a real change it had no current reading to explain; it then has
   to be turned again before anything is applied.

   Pure: no sensor, gauge or storage is touched here. */

#ifndef LS_COMPASS_LEARN_H
#define LS_COMPASS_LEARN_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LS_CL_N          7
#define LS_CL_RELEARN_S  5.0f
#define LS_CL_MAX_UT     40.0f    /* a learnt offset larger than this is not applied */

typedef struct {
    bool started;
    float x[LS_CL_N];             /* d[3], k[3], Z */
    float P[LS_CL_N][LS_CL_N];
    float radius;                 /* the calibration's R, uT */
    float res2;                   /* smoothed squared horizontal residual, uT^2 */
    float refused_s;              /* how long readings have been refused in a row */
    float amps_lo, amps_hi;       /* battery current range seen while learning */
    uint32_t used, refused, relearns;
    uint8_t sectors;              /* which eighths of the circle the field has been
                                     seen from, in the board's own frame */
    uint8_t applied;              /* offset axes known well enough to take off */
} ls_compass_learn_t;

typedef enum { LS_CL_SKIPPED, LS_CL_USED, LS_CL_REFUSED } ls_cl_result_t;

/* Starts over for a calibration of radius `radius_ut`. The current term and
   the offset carried from a saved state are loaded with ls_compass_learn_seed. */
void ls_compass_learn_reset(ls_compass_learn_t *l, float radius_ut);
/* Starts from a saved offset and current coefficient, with the given 1-sigma
   in uT and uT/A. */
void ls_compass_learn_seed(ls_compass_learn_t *l, const float d[3], const float k[3],
                           float d_sigma, float k_sigma);

/* One calibrated field reading `f` (screen axes, uT, ls_compass_field) with
   the accelerometer's reaction `up` (screen axes, g, ls_compass_gravity), the
   gyro's total rate in deg/s (NAN when unknown), the battery current in A
   (positive charging, NAN when unknown) and the time since the last call. */
ls_cl_result_t ls_compass_learn_step(ls_compass_learn_t *l, const float f[3], const float up[3],
                                     float gyro_dps, float amps, float dt);

/* What to subtract from a calibrated reading at this current: d + k * amps.
   Zero until the offset is known across the field as well as along it: a
   board held still learns only the part of d along the field it reads, and
   applying a guess for the rest bends the heading as soon as it turns. So
   nothing is applied before the field has been seen from LS_CL_SECTORS of
   eight directions, and then only on the axes whose doubt is under
   LS_CL_APPLY_UT (a flat turn never pins the axis out of the glass). An
   axis stays applied until its doubt passes LS_CL_DROP_UT, so it does not
   flicker at the edge. */
#define LS_CL_SECTORS  6
#define LS_CL_APPLY_UT 2.0f
#define LS_CL_DROP_UT  3.5f
bool ls_compass_learn_ready(const ls_compass_learn_t *l);
bool ls_compass_learn_correction(const ls_compass_learn_t *l, float amps, float out[3]);

/* The heading's 1-sigma, degrees, from what the filter still does not know
   and how well recent readings fit. NAN before it has started. */
float ls_compass_learn_accuracy(const ls_compass_learn_t *l, float amps);

#ifdef __cplusplus
}
#endif
#endif
