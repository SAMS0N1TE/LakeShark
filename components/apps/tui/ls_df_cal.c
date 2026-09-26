#include "ls_df_cal.h"

#include <math.h>
#include <string.h>

#define RAD 0.017453292519943295

static float wrap180(float a) { return fmodf(fmodf(a + 180.0f, 360.0f) + 360.0f, 360.0f) - 180.0f; }
static float wrap360(float a) { return fmodf(fmodf(a, 360.0f) + 360.0f, 360.0f); }

void ls_df_cal_begin(ls_df_cal_t *c)
{
    memset(c, 0, sizeof(*c));
    c->phase = LS_DF_CAL_AIM;
    c->mark = NAN;
}

void ls_df_cal_mark(ls_df_cal_t *c, float heading)
{
    if (!isfinite(heading)) return;
    c->mark = wrap360(heading);
    c->count = c->next = 0;
    c->phase = LS_DF_CAL_TURN;
}

bool ls_df_cal_circle(ls_df_cal_t *c, const ls_df_estimate_t *e)
{
    if (c->phase != LS_DF_CAL_TURN || !e || !e->valid || !isfinite(e->bearing) ||
        e->coverage < LS_DF_CAL_COVERAGE || e->contrast < LS_DF_CAL_CONTRAST) return false;
    c->offset[c->next] = wrap180(e->bearing - c->mark);
    c->next = (c->next + 1) % LS_DF_CAL_CIRCLES;
    if (c->count < LS_DF_CAL_CIRCLES) c->count++;
    return true;
}

void ls_df_cal_result(const ls_df_cal_t *c, ls_df_cal_result_t *out)
{
    memset(out, 0, sizeof(*out));
    out->circles = c->count;
    out->offset = out->spread = NAN;
    if (c->count < 1) return;
    /* Angles average on the circle: mean of unit vectors, and the circular
       standard deviation from the length of that mean. */
    double sx = 0, sy = 0;
    for (int i = 0; i < c->count; i++) { sx += cos(c->offset[i] * RAD); sy += sin(c->offset[i] * RAD); }
    sx /= c->count; sy /= c->count;
    const double r = sqrt(sx * sx + sy * sy);
    out->offset = (float)(atan2(sy, sx) / RAD);
    out->spread = r >= 1.0 ? 0.0f : (float)(sqrt(-2.0 * log(r)) / RAD);
    out->ready = c->count >= LS_DF_CAL_NEEDED && out->spread <= LS_DF_CAL_AGREE;
}

float ls_df_cal_apply(float bearing, float offset)
{
    if (!isfinite(bearing)) return bearing;
    if (!isfinite(offset)) return bearing;
    return wrap360(bearing - offset);
}
