/* Calibrating FIND against a beacon at a known bearing.

   A direction finder built from a signal level and a compass is bent by the
   antenna's own pattern and by the body holding it: the lobe's peak (or
   null) sits some degrees off the true bearing, the same way each time for
   a given radio and method. Set a beacon down (the Flipper's DF BEACON on
   915 or 433.92 MHz), aim the board's top at it and mark that heading, then
   turn full circles. Each circle's estimate against the mark is one offset; the
   circular mean of a few that agree is the correction, kept per radio and
   method and taken off every later estimate.

   Pure: no radio, sensor or storage is touched here. */

#ifndef LS_DF_CAL_H
#define LS_DF_CAL_H

#include <stdbool.h>
#include "ls_df.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LS_DF_CAL_CIRCLES   6      /* kept, newest replacing oldest */
#define LS_DF_CAL_NEEDED    3      /* circles before a result can be saved */
#define LS_DF_CAL_AGREE     8.0f   /* degrees of spread (1 sigma) allowed */
#define LS_DF_CAL_COVERAGE  330    /* degrees of a circle that must be heard */
#define LS_DF_CAL_CONTRAST  6.0f   /* dB between the strongest and weakest bin */
#define LS_DF_CAL_TURN_MAX  30.0f  /* degrees a second; faster skips bins */

typedef enum {
    LS_DF_CAL_OFF,
    LS_DF_CAL_AIM,       /* waiting for the board's top to be on the beacon */
    LS_DF_CAL_TURN,      /* marked; circles being turned */
} ls_df_cal_phase_t;

typedef struct {
    ls_df_cal_phase_t phase;
    float mark;                        /* true bearing to the beacon */
    float offset[LS_DF_CAL_CIRCLES];   /* estimate minus mark, per circle */
    int count, next;
} ls_df_cal_t;

typedef struct {
    bool ready;          /* enough circles, and they agree */
    int circles;
    float offset;        /* circular mean, degrees, estimate minus truth */
    float spread;        /* circular standard deviation, degrees */
} ls_df_cal_result_t;

void ls_df_cal_begin(ls_df_cal_t *c);
/* The board's top is on the beacon: `heading` is the true heading now. */
void ls_df_cal_mark(ls_df_cal_t *c, float heading);
/* A circle is done when it covers enough and stands out enough; returns
   whether `e` was taken. The caller clears its sweep after a take. */
bool ls_df_cal_circle(ls_df_cal_t *c, const ls_df_estimate_t *e);
void ls_df_cal_result(const ls_df_cal_t *c, ls_df_cal_result_t *out);
/* A bearing with the saved correction taken off. */
float ls_df_cal_apply(float bearing, float offset);

#ifdef __cplusplus
}
#endif
#endif
