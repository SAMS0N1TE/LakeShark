#include "ls_compass_learn.h"

#include <math.h>
#include <string.h>

enum { D0 = 0, K0 = 3, ZI = 6 };

/* Readings: how far each fact can be off from sensor noise and the tilt the
   accelerometer gets wrong while the board moves. */
#define SIGMA_H_UT   1.5f
#define SIGMA_V_UT   2.5f
/* Starting uncertainty and how fast each state may wander, per second. */
#define D_SIGMA0     4.0f
#define K_SIGMA0     60.0f
#define Z_SIGMA0     6.0f
#define D_WALK       0.02f        /* uT^2 per second */
#define K_WALK       1.0f         /* (uT/A)^2 per second */
#define Z_WALK       0.01f
#define GATE         16.0f        /* 4 sigma, squared */
#define RELEARN_UT   10.0f
#define MIN_USED     30

static int bits8(uint8_t v) { int n = 0; while (v) { v &= v - 1; n++; } return n; }

void ls_compass_learn_reset(ls_compass_learn_t *l, float radius_ut)
{
    memset(l, 0, sizeof(*l));
    l->radius = radius_ut;
    for (int i = 0; i < 3; i++) {
        l->P[D0 + i][D0 + i] = D_SIGMA0 * D_SIGMA0;
        l->P[K0 + i][K0 + i] = K_SIGMA0 * K_SIGMA0;
    }
    l->P[ZI][ZI] = Z_SIGMA0 * Z_SIGMA0;
    l->amps_lo = INFINITY; l->amps_hi = -INFINITY;
}

void ls_compass_learn_seed(ls_compass_learn_t *l, const float d[3], const float k[3],
                           float d_sigma, float k_sigma)
{
    for (int i = 0; i < 3; i++) {
        if (d && isfinite(d[i])) { l->x[D0 + i] = d[i]; l->P[D0 + i][D0 + i] = d_sigma * d_sigma; }
        if (k && isfinite(k[i])) { l->x[K0 + i] = k[i]; l->P[K0 + i][K0 + i] = k_sigma * k_sigma; }
    }
}

/* One scalar measurement: residual r = h(x) - z with gradient hv. Returns
   the normalised innovation squared, or -1 when it was refused. */
static float update(ls_compass_learn_t *l, const float hv[LS_CL_N], float r, float sigma, bool apply)
{
    float ph[LS_CL_N];
    float s = sigma * sigma;
    for (int i = 0; i < LS_CL_N; i++) {
        float acc = 0;
        for (int j = 0; j < LS_CL_N; j++) acc += l->P[i][j] * hv[j];
        ph[i] = acc;
        s += hv[i] * acc;
    }
    if (!(s > 0)) return -1;
    const float nis = r * r / s;
    if (!apply || nis > GATE) return nis > GATE ? -1 : nis;
    for (int i = 0; i < LS_CL_N; i++) l->x[i] -= ph[i] * r / s;
    for (int i = 0; i < LS_CL_N; i++)
        for (int j = 0; j < LS_CL_N; j++) l->P[i][j] -= ph[i] * ph[j] / s;
    /* Keep it symmetric and its diagonal positive against rounding. */
    for (int i = 0; i < LS_CL_N; i++) {
        if (l->P[i][i] < 1e-6f) l->P[i][i] = 1e-6f;
        for (int j = i + 1; j < LS_CL_N; j++) l->P[i][j] = l->P[j][i] = 0.5f * (l->P[i][j] + l->P[j][i]);
    }
    return nis;
}

/* The two residuals and their gradients at the current state. */
static bool residuals(const ls_compass_learn_t *l, const float f[3], const float down[3], float amps,
                      float *rh, float hh[LS_CL_N], float *rv, float hvv[LS_CL_N])
{
    float v[3];
    for (int i = 0; i < 3; i++) v[i] = f[i] - l->x[D0 + i] - l->x[K0 + i] * amps;
    const float vd = v[0] * down[0] + v[1] * down[1] + v[2] * down[2];
    float vh[3];
    for (int i = 0; i < 3; i++) vh[i] = v[i] - vd * down[i];
    const float h = sqrtf(vh[0] * vh[0] + vh[1] * vh[1] + vh[2] * vh[2]);
    const float z = l->x[ZI];
    const float h2 = l->radius * l->radius - z * z;
    if (!(h > 1.0f) || !(h2 > 0.05f * l->radius * l->radius)) return false;
    const float H = sqrtf(h2);
    memset(hh, 0, sizeof(float) * LS_CL_N);
    memset(hvv, 0, sizeof(float) * LS_CL_N);
    for (int i = 0; i < 3; i++) {
        const float u = vh[i] / h;
        hh[D0 + i] = -u; hh[K0 + i] = -u * amps;
        hvv[D0 + i] = -down[i]; hvv[K0 + i] = -down[i] * amps;
    }
    hh[ZI] = z / H;
    hvv[ZI] = -1.0f;
    *rh = h - H;
    *rv = vd - z;
    return true;
}

ls_cl_result_t ls_compass_learn_step(ls_compass_learn_t *l, const float f[3], const float up[3],
                                     float gyro_dps, float amps, float dt)
{
    if (!l || !f || !up || !(l->radius > 5.0f)) return LS_CL_SKIPPED;
    for (int i = 0; i < 3; i++) if (!isfinite(f[i]) || !isfinite(up[i])) return LS_CL_SKIPPED;
    const float g = sqrtf(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
    /* Gravity is only down while the board is not being swung. */
    if (!(g > 0.92f && g < 1.08f)) return LS_CL_SKIPPED;
    if (isfinite(gyro_dps) && gyro_dps > 90.0f) return LS_CL_SKIPPED;
    const float down[3] = { -up[0] / g, -up[1] / g, -up[2] / g };
    /* An unknown current is none: k then learns nothing and adds nothing. */
    const float a = isfinite(amps) ? amps : 0.0f;
    if (!(dt > 0)) dt = 0;
    if (dt > 1.0f) dt = 1.0f;

    if (!l->started) {
        l->x[ZI] = f[0] * down[0] + f[1] * down[1] + f[2] * down[2];
        if (fabsf(l->x[ZI]) > 0.95f * l->radius) return LS_CL_SKIPPED;
        l->started = true;
    }
    for (int i = 0; i < 3; i++) {
        l->P[D0 + i][D0 + i] += D_WALK * dt;
        l->P[K0 + i][K0 + i] += K_WALK * dt;
    }
    l->P[ZI][ZI] += Z_WALK * dt;

    float rh, rv, hh[LS_CL_N], hv[LS_CL_N];
    /* Both facts are judged before either is taken, so a reading bent by
       steel is refused whole. One that cannot be judged at all is refused
       too: with no current change to tell the current's term from the
       offset, the two can carry the vertical field up to the radius, which
       leaves no horizontal field to judge any reading by, and skipped it
       would stay that way. */
    const bool judged = residuals(l, f, down, a, &rh, hh, &rv, hv);
    if (!judged || update(l, hh, rh, SIGMA_H_UT, false) < 0 || update(l, hv, rv, SIGMA_V_UT, false) < 0) {
        l->refused++;
        l->refused_s += dt;
        if (l->refused_s >= LS_CL_RELEARN_S) {
            for (int i = 0; i < 3; i++) l->P[D0 + i][D0 + i] += RELEARN_UT * RELEARN_UT;
            l->P[ZI][ZI] += Z_SIGMA0 * Z_SIGMA0;
            /* A vertical field no reading can be judged against starts
               again from this reading's, as at the start, kept inside the
               range where one can. */
            if (!(l->radius * l->radius - l->x[ZI] * l->x[ZI] > 0.05f * l->radius * l->radius)) {
                const float zmax = 0.95f * l->radius;
                float z = 0;
                for (int i = 0; i < 3; i++) z += (f[i] - l->x[D0 + i] - l->x[K0 + i] * a) * down[i];
                l->x[ZI] = z > zmax ? zmax : z < -zmax ? -zmax : z;
            }
            l->refused_s = 0;
            l->relearns++;
            l->sectors = 0;
            l->applied = 0;
        }
        return LS_CL_REFUSED;
    }
    l->refused_s = 0;
    update(l, hh, rh, SIGMA_H_UT, true);
    if (residuals(l, f, down, a, &rh, hh, &rv, hv)) update(l, hv, rv, SIGMA_V_UT, true);
    if (residuals(l, f, down, a, &rh, hh, &rv, hv))
        l->res2 = l->used ? l->res2 + (rh * rh - l->res2) * 0.05f : rh * rh;
    /* Where the field points in the board's frame, by eighths round the
       board's own z: a turn in any hold sweeps it round. */
    const float az = atan2f(f[1], f[0]);
    l->sectors |= (uint8_t)(1u << (((int)floorf((az + 3.14159265f) / 0.78539816f)) & 7));
    for (int i = 0; i < 3; i++) {
        const float var = l->P[D0 + i][D0 + i];
        if (var > LS_CL_DROP_UT * LS_CL_DROP_UT) l->applied &= (uint8_t)~(1u << i);
        else if (var <= LS_CL_APPLY_UT * LS_CL_APPLY_UT && l->used >= MIN_USED && bits8(l->sectors) >= LS_CL_SECTORS)
            l->applied |= (uint8_t)(1u << i);
    }
    l->used++;
    if (isfinite(amps)) {
        if (amps < l->amps_lo) l->amps_lo = amps;
        if (amps > l->amps_hi) l->amps_hi = amps;
    }
    return LS_CL_USED;
}

bool ls_compass_learn_ready(const ls_compass_learn_t *l) { return l && l->started && l->applied; }

bool ls_compass_learn_correction(const ls_compass_learn_t *l, float amps, float out[3])
{
    out[0] = out[1] = out[2] = 0;
    if (!ls_compass_learn_ready(l)) return false;
    const float a = isfinite(amps) ? amps : 0.0f;
    float c[3], n2 = 0;
    for (int i = 0; i < 3; i++) {
        c[i] = l->applied & (1u << i) ? l->x[D0 + i] + l->x[K0 + i] * a : 0.0f;
        if (!isfinite(c[i])) return false;
        n2 += c[i] * c[i];
    }
    if (n2 > LS_CL_MAX_UT * LS_CL_MAX_UT) return false;
    memcpy(out, c, sizeof(c));
    return true;
}

float ls_compass_learn_accuracy(const ls_compass_learn_t *l, float amps)
{
    if (!l || !l->started) return NAN;
    const float z = l->x[ZI];
    const float h2 = l->radius * l->radius - z * z;
    if (!(h2 > 1)) return NAN;
    const float a = isfinite(amps) ? amps : 0.0f;
    /* Two of the offset's three axes lie across the horizontal field on
       average; the current term's doubt scales with the current. */
    float var = 0;
    for (int i = 0; i < 3; i++) var += l->P[D0 + i][D0 + i] + a * a * l->P[K0 + i][K0 + i];
    var = var * (2.0f / 3.0f) + l->res2;
    return atanf(sqrtf(var / h2)) * 57.29578f;
}
