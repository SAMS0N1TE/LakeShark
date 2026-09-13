#include "ls_compass.h"
#include "ls_imu_heading_math.h"
#include <math.h>
#include <string.h>

void ls_compass_begin(ls_compass_fit_t *f) { memset(f, 0, sizeof(*f)); }

void ls_compass_guide_begin(ls_compass_guide_t *g)
{
    memset(g, 0, sizeof(*g)); g->up_face = -1;
}

void ls_compass_guide_sample(ls_compass_guide_t *g, const ls_imu_sample_t *s)
{
    if (!g || g->step >= 6) return;
    g->aligned = false;
    if (!s || !s->mag_valid || !isfinite(s->mx) || !isfinite(s->my) || !isfinite(s->mz) ||
        !isfinite(s->ax) || !isfinite(s->ay) || !isfinite(s->az) ||
        !isfinite(s->gx) || !isfinite(s->gy) || !isfinite(s->gz)) { g->hold = 0; return; }
    const float a[] = {s->ax, s->ay, s->az};
    const float n = sqrtf(a[0]*a[0] + a[1]*a[1] + a[2]*a[2]);
    if (n < .85f || n > 1.15f) { g->hold = 0; return; }
    int face = -1;
    for (int i = 0; i < 3; i++)
        if (fabsf(a[i]) > .92f*n) face = 2*i + (a[i] < 0);
    /* Screen-up establishes Z polarity. The in-plane directions are the
       same mounted screen axes used by the firmware's auto-rotation. */
    static const int target[] = {-1, 2, 0, 3, 1, -1};
    int want = g->step == 0 ? (face >= 4 ? face : -1) :
               g->step == 5 ? (g->up_face ^ 1) : target[g->step];
    g->aligned = face >= 0 && face == want;
    if (!g->aligned || fabsf(s->gx) > 18 || fabsf(s->gy) > 18 || fabsf(s->gz) > 18) {
        g->hold = 0; return;
    }
    if (g->step == 0 && g->up_face != face) { g->up_face = (int8_t)face; g->hold = 0; }
    if (++g->hold >= LS_COMPASS_HOLD_SAMPLES) { g->step++; g->hold = 0; g->aligned = false; }
}

void ls_compass_collect(ls_compass_fit_t *f, const ls_imu_sample_t *s)
{
    if (!f || !s || !s->mag_valid) return;
    const float m[] = {s->mx, s->my, s->mz}, a[] = {s->ax, s->ay, s->az};
    for (int i = 0; i < 3; i++)
        if (!isfinite(m[i]) || fabsf(m[i]) > 4000 || !isfinite(a[i])) return;
    const float gravity = sqrtf(a[0]*a[0] + a[1]*a[1] + a[2]*a[2]);
    if (gravity < .75f || gravity > 1.25f) return;
    int bucket = 6;
    for (int i = 0; i < 3; i++)
        if (fabsf(a[i]) > .8f * gravity) bucket = 2*i + (a[i] < 0);
    /* Waiting on one side must not drown out the other five, or fill the
       entire sample budget before the user starts turning the board. */
    if (f->bucket_samples[bucket] >= (bucket == 6 ? 128 : 64)) return;
    f->bucket_samples[bucket]++;
    if (bucket < 6) f->faces |= 1u << bucket;
    /* Fit |m-offset|^2 = radius^2, with microtesla scaled for conditioning.
       Four-parameter hard-iron model: NXP AN4246, section 6. */
    double x = m[0]/100.0, y = m[1]/100.0, z = m[2]/100.0;
    const double row[] = {2*x, 2*y, 2*z, 1}, target = x*x + y*y + z*z;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) f->normal[i][j] += row[i] * row[j];
        f->normal[i][4] += row[i] * target;
    }
    f->squares += target * target;
    f->samples++;
}

bool ls_compass_cal_valid(const ls_compass_cal_t *c)
{
    if (!c || !isfinite(c->radius) || c->radius < 10 || c->radius > 100 ||
        !isfinite(c->error) || c->error < 0 || c->error > .12f) return false;
    for (int i = 0; i < 3; i++)
        if (!isfinite(c->offset[i]) || fabsf(c->offset[i]) > 4000) return false;
    return true;
}

bool ls_compass_finish(const ls_compass_fit_t *f, ls_compass_cal_t *out)
{
    if (!f || !out || f->samples < 120 || f->faces != 63) return false;
    double a[4][5]; memcpy(a, f->normal, sizeof(a));
    for (int i = 0; i < 4; i++) {
        int pivot = i;
        for (int j = i+1; j < 4; j++) if (fabs(a[j][i]) > fabs(a[pivot][i])) pivot = j;
        if (fabs(a[pivot][i]) < 1e-6 * f->samples) return false;
        for (int j = i; j < 5; j++) { double t = a[i][j]; a[i][j] = a[pivot][j]; a[pivot][j] = t; }
        double scale = a[i][i];
        for (int j = i; j < 5; j++) a[i][j] /= scale;
        for (int k = 0; k < 4; k++) if (k != i) {
            scale = a[k][i];
            for (int j = i; j < 5; j++) a[k][j] -= scale * a[i][j];
        }
    }
    double radius2 = a[3][4], residual = f->squares;
    ls_compass_cal_t c = {0};
    for (int i = 0; i < 3; i++) { c.offset[i] = (float)(100*a[i][4]); radius2 += a[i][4]*a[i][4]; }
    if (radius2 <= 0) return false;
    for (int i = 0; i < 4; i++) residual -= a[i][4] * f->normal[i][4];
    if (residual < 0) residual = 0;
    c.radius = (float)(100*sqrt(radius2));
    c.error = (float)(sqrt(residual / f->samples) / (2*radius2));
    if (!ls_compass_cal_valid(&c)) return false;
    *out = c;
    return true;
}

float ls_compass_heading(const ls_imu_sample_t *s, const ls_compass_cal_t *c)
{
    if (!s || !s->mag_valid || !isfinite(s->mx) || !isfinite(s->my)) return NAN;
    float x = s->mx, y = s->my;
    if (c) { x -= c->offset[0]; y -= c->offset[1]; }
    return ls_imu_magnetic_heading(x, y);
}

float ls_compass_ease(float current, float target, float fraction)
{
    if (!isfinite(target)) return NAN;
    if (!isfinite(current)) return target;
    if (fraction < 0) fraction = 0;
    if (fraction > 1) fraction = 1;
    float delta = fmodf(target - current + 540, 360) - 180;
    return fmodf(current + fraction * delta + 360, 360);
}
