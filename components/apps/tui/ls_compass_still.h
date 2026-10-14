#ifndef LS_COMPASS_STILL_H
#define LS_COMPASS_STILL_H
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

/* Constant-space Welford statistics. Heading is unwrapped across north. */
typedef struct {
    uint32_t n, moving, missing;
    float first, last, angle, lo, hi;
    double mean[5], m2[5]; /* gyro xyz, corrected field, dip */
} ls_compass_still_t;

static inline void ls_compass_still_add(ls_compass_still_t *s, float heading,
                                        const float values[5], bool stationary)
{
    if (!isfinite(heading)) { s->missing++; return; }
    for (int i=0;i<5;i++) if (!isfinite(values[i])) { s->missing++; return; }
    if (!s->n) s->first = s->last = s->angle = s->lo = s->hi = heading;
    s->angle += fmodf(heading-s->last+540,360)-180;
    s->last = heading;
    s->lo = fminf(s->lo,s->angle); s->hi = fmaxf(s->hi,s->angle);
    s->n++; if (!stationary) s->moving++;
    for (int i=0;i<5;i++) {
        const double d=values[i]-s->mean[i];
        s->mean[i]+=d/s->n; s->m2[i]+=d*(values[i]-s->mean[i]);
    }
}
static inline bool ls_compass_still_pass(const ls_compass_still_t *s, float seconds)
{
    return seconds >= 60 && s->n >= 120 && !s->missing && !s->moving &&
           s->hi-s->lo <= 2 && fabsf(s->angle-s->first) <= 1;
}
#endif
