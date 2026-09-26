#include "ls_compass.h"
#include <math.h>
#include <string.h>
#include "esp_attr.h"

/* The stored magnetic basis as screen axes: -my right, -mx up, -mz out. See
   ls_imu_heading_math.h. It was derived, not measured, so a calibration with
   enough poses checks it (pick_basis) and records what the data says. */
static const int8_t DEFAULT_BASIS[3] = { -2, -1, -3 };

void ls_compass_begin(ls_compass_fit_t *f) { memset(f, 0, sizeof(*f)); }

void ls_compass_gravity(const ls_imu_sample_t *s, float out[3])
{
    out[0] = -s->ax; out[1] = s->ay; out[2] = -s->az;
}

void ls_compass_guide_begin(ls_compass_guide_t *g)
{
    memset(g, 0, sizeof(*g)); g->up_face = -1;
}

void ls_compass_guide_sample(ls_compass_guide_t *g, const ls_imu_sample_t *s)
{
    if (!g || g->step >= LS_COMPASS_FACE_STEPS) return;
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

/* Which of 26 directions (cube faces, edges, corners) a vector points in. */
static int direction_bin(const float u[3])
{
    const float n = sqrtf(u[0]*u[0] + u[1]*u[1] + u[2]*u[2]);
    if (!(n > 1)) return -1;
    int code = 0;
    for (int i = 0; i < 3; i++) {
        const float c = u[i] / n;
        code = code * 3 + (c > .4f ? 2 : c < -.4f ? 0 : 1);
    }
    return code == 13 ? -1 : code < 13 ? code : code - 1;
}

static int popcount32(uint32_t v) { int n = 0; while (v) { v &= v - 1; n++; } return n; }

void ls_compass_collect(ls_compass_fit_t *f, const ls_imu_sample_t *s)
{
    if (!f || !s || !s->mag_valid) return;
    const float m[] = {s->mx, s->my, s->mz}, a[] = {s->ax, s->ay, s->az};
    for (int i = 0; i < 3; i++)
        if (!isfinite(m[i]) || fabsf(m[i]) > 4000 || !isfinite(a[i])) return;
    int bucket = 6;
    if (f->tumble) {
        /* Waving the board about: gravity is not trusted to pick a face,
           and the samples are judged by where the field points instead. */
        if (f->tumble_seen < UINT16_MAX) f->tumble_seen++;
        const float u[] = {m[0] - f->center[0], m[1] - f->center[1], m[2] - f->center[2]};
        const int bin = direction_bin(u);
        if (bin >= 0) f->cover |= 1u << bin;
        bucket = 7;
    } else {
        const float gravity = sqrtf(a[0]*a[0] + a[1]*a[1] + a[2]*a[2]);
        if (gravity < .75f || gravity > 1.25f) return;
        for (int i = 0; i < 3; i++)
            if (fabsf(a[i]) > .8f * gravity) bucket = 2*i + (a[i] < 0);
    }
    /* Waiting on one side must not drown out the other five, or fill the
       entire sample budget before the user starts turning the board. */
    const int cap = bucket == 7 ? 192 : bucket == 6 ? 128 : 64;
    if (f->bucket_samples[bucket] >= cap) return;
    f->bucket_samples[bucket]++;
    if (bucket < 6) f->faces |= 1u << bucket;
    if (f->kept < LS_COMPASS_KEEP) {
        float *k = f->keep[f->kept++];
        float g[3]; ls_compass_gravity(s, g);
        k[0] = m[0]; k[1] = m[1]; k[2] = m[2]; k[3] = g[0]; k[4] = g[1]; k[5] = g[2];
    }
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

const char *ls_compass_cal_problem(const ls_compass_cal_t *c)
{
    if (!c) return "none";
    if (!isfinite(c->radius) || c->radius < 10 || c->radius > 100) return "radius";
    if (!isfinite(c->error) || c->error < 0 || c->error > .12f) return "error";
    for (int i = 0; i < 3; i++)
        if (!isfinite(c->offset[i]) || fabsf(c->offset[i]) > 4000) return "offset";
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) {
        const float v = c->soft[i][j];
        if (!isfinite(v) || fabsf(v) > 4) return "soft iron";
    }
    /* A record from the build that chose its own basis is not trusted. */
    if ((c->basis[0] || c->basis[1] || c->basis[2]) &&
        memcmp(c->basis, DEFAULT_BASIS, sizeof(DEFAULT_BASIS)) != 0) return "basis";
    return NULL;
}

bool ls_compass_cal_valid(const ls_compass_cal_t *c) { return !ls_compass_cal_problem(c); }

/* Gauss-Jordan with partial pivoting on an n x (n+1) row-major system. */
static bool solve(double *a, int n, double tiny)
{
    const int w = n + 1;
    for (int i = 0; i < n; i++) {
        int pivot = i;
        for (int j = i + 1; j < n; j++) if (fabs(a[j*w+i]) > fabs(a[pivot*w+i])) pivot = j;
        if (!(fabs(a[pivot*w+i]) > tiny)) return false;
        for (int j = i; j < w; j++) { double t = a[i*w+j]; a[i*w+j] = a[pivot*w+j]; a[pivot*w+j] = t; }
        const double d = a[i*w+i];
        for (int j = i; j < w; j++) a[i*w+j] /= d;
        for (int k = 0; k < n; k++) if (k != i) {
            const double s = a[k*w+i];
            for (int j = i; j < w; j++) a[k*w+j] -= s * a[i*w+j];
        }
    }
    return true;
}

/* `strict` applies the full validity test. Without it the result is only a
   centre and a radius, good enough to judge coverage or to start the
   ellipsoid from when soft iron has pushed the sphere's residual too far. */
static bool sphere_fit(const ls_compass_fit_t *f, ls_compass_cal_t *out, bool strict)
{
    if (!f || !out || f->samples < 120 || f->faces != 63) return false;
    double a[4][5]; memcpy(a, f->normal, sizeof(a));
    if (!solve(&a[0][0], 4, 1e-6 * f->samples)) return false;
    double radius2 = a[3][4], residual = f->squares;
    ls_compass_cal_t c;
    memset(&c, 0, sizeof(c));
    for (int i = 0; i < 3; i++) { c.offset[i] = (float)(100*a[i][4]); radius2 += a[i][4]*a[i][4]; }
    if (radius2 <= 0) return false;
    for (int i = 0; i < 4; i++) residual -= a[i][4] * f->normal[i][4];
    if (residual < 0) residual = 0;
    c.radius = (float)(100*sqrt(radius2));
    c.error = (float)(sqrt(residual / f->samples) / (2*radius2));
    if (strict ? !ls_compass_cal_valid(&c) : !(c.radius >= 10 && c.radius <= 100 && isfinite(c.error)))
        return false;
    *out = c;
    return true;
}

static bool has_soft(const ls_compass_cal_t *c)
{
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) if (c->soft[i][j] != 0) return true;
    return false;
}

static void correct(const ls_compass_cal_t *c, const float m[3], float out[3])
{
    const float d[] = {m[0] - c->offset[0], m[1] - c->offset[1], m[2] - c->offset[2]};
    if (!has_soft(c)) { memcpy(out, d, sizeof(d)); return; }
    for (int i = 0; i < 3; i++) out[i] = c->soft[i][0]*d[0] + c->soft[i][1]*d[1] + c->soft[i][2]*d[2];
}

/* RMS of the corrected magnitude against the radius, as a fraction. */
static float magnitude_error(const ls_compass_fit_t *f, const ls_compass_cal_t *c)
{
    double sum = 0;
    for (int i = 0; i < f->kept; i++) {
        float v[3]; correct(c, f->keep[i], v);
        const double e = (sqrt((double)v[0]*v[0] + (double)v[1]*v[1] + (double)v[2]*v[2]) - c->radius) / c->radius;
        sum += e * e;
    }
    return f->kept ? (float)sqrt(sum / f->kept) : INFINITY;
}

/* Symmetric 3x3 eigen decomposition by Jacobi rotations: a = v diag(w) v'. */
static void eigen3(double a[3][3], double v[3][3], double w[3])
{
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) v[i][j] = i == j;
    for (int sweep = 0; sweep < 50; sweep++) {
        if (a[0][1]*a[0][1] + a[0][2]*a[0][2] + a[1][2]*a[1][2] < 1e-24) break;
        for (int p = 0; p < 2; p++) for (int q = p + 1; q < 3; q++) {
            if (fabs(a[p][q]) < 1e-30) continue;
            const double theta = (a[q][q] - a[p][p]) / (2 * a[p][q]);
            const double t = (theta >= 0 ? 1 : -1) / (fabs(theta) + sqrt(theta*theta + 1));
            const double c = 1 / sqrt(t*t + 1), s = t * c;
            for (int k = 0; k < 3; k++) {
                const double kp = a[k][p], kq = a[k][q];
                a[k][p] = c*kp - s*kq; a[k][q] = s*kp + c*kq;
            }
            for (int k = 0; k < 3; k++) {
                const double pk = a[p][k], qk = a[q][k];
                a[p][k] = c*pk - s*qk; a[q][k] = s*pk + c*qk;
            }
            for (int k = 0; k < 3; k++) {
                const double kp = v[k][p], kq = v[k][q];
                v[k][p] = c*kp - s*kq; v[k][q] = s*kp + c*kq;
            }
        }
    }
    for (int i = 0; i < 3; i++) w[i] = a[i][i];
}

/* Hard and soft iron: fit the general ellipsoid u'Au + 2v'u = 1 to the kept
   samples, centred and scaled by the sphere fit so the system is well
   conditioned, then take the symmetric square root of A as the correction.
   The steel can of the keyboard's 21700 and its magnets are soft iron, which
   no offset can take out. Replaces `c` only when it describes the data
   better and the distortion is plausible for a handheld. */
/* Only the field worker finishes a fit; the system lives off its 8 KB stack. */
EXT_RAM_BSS_ATTR static double s_quadric[9][10];

static void ellipsoid_fit(const ls_compass_fit_t *f, ls_compass_cal_t *c)
{
    memset(s_quadric, 0, sizeof(s_quadric));
    const double r0 = c->radius;
    for (int i = 0; i < f->kept; i++) {
        const double x = (f->keep[i][0] - c->offset[0]) / r0, y = (f->keep[i][1] - c->offset[1]) / r0,
                     z = (f->keep[i][2] - c->offset[2]) / r0;
        const double row[9] = {x*x, y*y, z*z, 2*x*y, 2*x*z, 2*y*z, 2*x, 2*y, 2*z};
        for (int j = 0; j < 9; j++) {
            for (int k = 0; k < 9; k++) s_quadric[j][k] += row[j] * row[k];
            s_quadric[j][9] += row[j];
        }
    }
    if (!solve(&s_quadric[0][0], 9, 1e-9 * f->kept)) return;
    double p[9];
    for (int i = 0; i < 9; i++) p[i] = s_quadric[i][9];
    double A[3][3] = {{p[0], p[3], p[4]}, {p[3], p[1], p[5]}, {p[4], p[5], p[2]}};
    double sys[3][4];
    for (int i = 0; i < 3; i++) { for (int j = 0; j < 3; j++) sys[i][j] = A[i][j]; sys[i][3] = -p[6+i]; }
    if (!solve(&sys[0][0], 3, 1e-9)) return;
    const double uc[3] = {sys[0][3], sys[1][3], sys[2][3]};
    if (uc[0]*uc[0] + uc[1]*uc[1] + uc[2]*uc[2] > .25) return;   /* moved half a radius */
    double k = 1;
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) k += uc[i] * A[i][j] * uc[j];
    if (!(k > 0)) return;
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) A[i][j] /= k;
    double v[3][3], w[3];
    eigen3(A, v, w);
    double lo = INFINITY, hi = 0, det = 1;
    for (int i = 0; i < 3; i++) {
        if (!(w[i] > 0)) return;
        w[i] = sqrt(w[i]);
        if (w[i] < lo) lo = w[i];
        if (w[i] > hi) hi = w[i];
        det *= w[i];
    }
    if (hi / lo > 2.5) return;
    const double norm = cbrt(det);
    ls_compass_cal_t e = *c;
    for (int i = 0; i < 3; i++) {
        e.offset[i] = (float)(c->offset[i] + r0 * uc[i]);
        for (int j = 0; j < 3; j++) {
            double s = 0;
            for (int m = 0; m < 3; m++) s += v[i][m] * w[m] * v[j][m];
            e.soft[i][j] = (float)(s / norm);
        }
    }
    e.radius = (float)(r0 / norm);
    const float before = magnitude_error(f, c);
    e.error = magnitude_error(f, &e);
    c->error = before;
    if (e.error < before && ls_compass_cal_valid(&e)) *c = e;
}

static void map_basis(const int8_t b[3], const float m[3], float out[3])
{
    for (int i = 0; i < 3; i++) {
        const int axis = (b[i] < 0 ? -b[i] : b[i]) - 1;
        out[i] = b[i] < 0 ? -m[axis] : m[axis];
    }
}

/* Spread and mean of the dip over the kept samples with a quiet accelerometer,
   through basis `b`. In the right basis the angle between the field and
   gravity is the same in every pose; in a mirrored or swapped one it is not. */
static bool dip_stats(const ls_compass_fit_t *f, const ls_compass_cal_t *c, const int8_t b[3],
                      float *spread, float *mean)
{
    double sum = 0, sum2 = 0; int count = 0;
    for (int i = 0; i < f->kept; i++) {
        const float *k = f->keep[i];
        const float g = sqrtf(k[3]*k[3] + k[4]*k[4] + k[5]*k[5]);
        if (g < .9f || g > 1.1f) continue;
        float corrected[3], s[3];
        correct(c, k, corrected);
        map_basis(b, corrected, s);
        const float n = sqrtf(s[0]*s[0] + s[1]*s[1] + s[2]*s[2]);
        if (!(n > 1)) continue;
        const float down = -(s[0]*k[3] + s[1]*k[4] + s[2]*k[5]) / (n * g);
        const double dip = asin(fmax(-1, fmin(1, down))) * 57.29577951308232;
        sum += dip; sum2 += dip * dip; count++;
    }
    if (count < 40) return false;
    const double m = sum / count;
    *mean = (float)m;
    *spread = (float)sqrt(fmax(0, sum2 / count - m * m));
    return true;
}

/* The basis is the default, which is right in true screen axes once the
   accelerometer is (ls_compass_gravity). On the board, with the offset taken
   out, it reads a dip of 50 to 64 degrees across five poses against a model
   of 67; with ls_imu's own accelerometer it looked 54 degrees inconsistent,
   and a search over bases then picked one reading -47. The spread is kept
   as a check of the fit. */
static void pick_basis(const ls_compass_fit_t *f, ls_compass_cal_t *c)
{
    memcpy(c->basis, DEFAULT_BASIS, sizeof(c->basis));
    float spread, mean;
    c->dip_spread = dip_stats(f, c, DEFAULT_BASIS, &spread, &mean) ? spread : NAN;
}

bool ls_compass_tumble_begin(ls_compass_fit_t *f)
{
    ls_compass_cal_t c;
    if (!sphere_fit(f, &c, false)) return false;
    memcpy(f->center, c.offset, sizeof(f->center));
    f->tumble = true; f->cover = 0; f->tumble_seen = 0;
    return true;
}

int ls_compass_tumble_cover(const ls_compass_fit_t *f) { return f ? popcount32(f->cover) : 0; }

bool ls_compass_tumble_done(const ls_compass_fit_t *f)
{
    if (!f || !f->tumble) return false;
    return (popcount32(f->cover) >= LS_COMPASS_COVER_NEEDED && f->bucket_samples[7] >= 60) ||
           f->tumble_seen >= LS_COMPASS_TUMBLE_MAX;
}

bool ls_compass_finish(const ls_compass_fit_t *f, ls_compass_cal_t *out)
{
    ls_compass_cal_t c;
    const bool rich = f && f->tumble && popcount32(f->cover) >= 12 && f->bucket_samples[7] >= 40;
    if (!out || !sphere_fit(f, &c, !rich)) return false;
    if (rich) ellipsoid_fit(f, &c);
    pick_basis(f, &c);
    if (!ls_compass_cal_valid(&c)) return false;
    *out = c;
    return true;
}

bool ls_compass_field(const ls_imu_sample_t *s, const ls_compass_cal_t *c, float out[3])
{
    if (!s || !s->mag_valid || !isfinite(s->mx) || !isfinite(s->my) || !isfinite(s->mz)) return false;
    float m[3] = {s->mx, s->my, s->mz};
    const int8_t *b = DEFAULT_BASIS;
    if (c) {
        float v[3]; correct(c, m, v); memcpy(m, v, sizeof(m));
        if (c->basis[0] || c->basis[1] || c->basis[2]) b = c->basis;
    }
    map_basis(b, m, out);
    return true;
}

float ls_compass_heading(const ls_imu_sample_t *s, const ls_compass_cal_t *c)
{
    float f[3];
    if (!ls_compass_field(s, c, f) || f[0]*f[0] + f[1]*f[1] < 1) return NAN;
    /* Flat: north lies along the field's horizontal part, and the heading
       is the top edge's angle clockwise from it. */
    float degrees = atan2f(-f[0], f[1]) * 57.295779513f;
    return degrees < 0 ? degrees + 360 : degrees;
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
