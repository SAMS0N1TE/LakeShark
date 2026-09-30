#include "ls_df_fit.h"

#include <math.h>
#include <string.h>
#include "esp_attr.h"

#define STEP     (360.0f / LS_DF_BINS)
#define PSTEP    (360.0f / LS_DF_PAT_BINS)
#define RADF     0.017453292f
#define DEGF     57.29578f
/* Fading is not independent from one direction to the next, so the reads
   say less than their count suggests: on simulated turns with fading the
   plain figure held two sigma only two times in three. */
#define SIGMA_K  2.0f

static float wrap360(float a) { a = fmodf(a, 360.0f); return a < 0 ? a + 360.0f : a; }

/* Gauss-Jordan with partial pivoting on an n x (n+1) row-major system. */
static bool solve(float *a, int n)
{
    const int w = n + 1;
    for (int i = 0; i < n; i++) {
        int pivot = i;
        for (int j = i + 1; j < n; j++) if (fabsf(a[j * w + i]) > fabsf(a[pivot * w + i])) pivot = j;
        if (!(fabsf(a[pivot * w + i]) > 1e-6f)) return false;
        for (int j = i; j < w; j++) { const float t = a[i * w + j]; a[i * w + j] = a[pivot * w + j]; a[pivot * w + j] = t; }
        const float d = a[i * w + i];
        for (int j = i; j < w; j++) a[i * w + j] /= d;
        for (int k = 0; k < n; k++) if (k != i) {
            const float f = a[k * w + i];
            for (int j = i; j < w; j++) a[k * w + j] -= f * a[i * w + j];
        }
    }
    return true;
}

typedef struct { float th[LS_DF_BINS], v[LS_DF_BINS], w[LS_DF_BINS]; int n, gap, covered; float sw; } heard_t;

/* Degrees of the circle heard. A read every 10 degrees fills every other
   bin, and that is a turn heard all round: only a run of three or more
   empty bins (15 degrees) counts as a direction not heard. */
static int covered_deg(const ls_df_sweep_t *s)
{
    int missing = 0, run = 0, lead = -1;
    for (int i = 0; i < LS_DF_BINS; i++) {
        if (!s->count[i]) { run++; continue; }
        if (lead < 0) lead = run;
        else if (run >= 3) missing += run;
        run = 0;
    }
    if (lead < 0) return 0;
    if (run + lead >= 3) missing += run + lead;
    return (int)((LS_DF_BINS - missing) * STEP);
}

static void gather(const ls_df_sweep_t *s, heard_t *h)
{
    h->n = 0; h->sw = 0;
    int run = 0, worst = 0, first_gap = -1;
    for (int i = 0; i < LS_DF_BINS; i++) {
        const float v = ls_df_typical(s, i);
        if (!isfinite(v)) { run++; continue; }
        if (first_gap < 0) first_gap = run;
        if (run > worst) worst = run;
        run = 0;
        const float n = s->ring_n[i];
        h->th[h->n] = i * STEP * RADF;
        h->v[h->n] = v;
        h->w[h->n] = n / (n + 1.0f);
        h->sw += h->w[h->n];
        h->n++;
    }
    /* The gap that wraps past 0 joins the last run to the first. */
    if (first_gap >= 0 && run + first_gap > worst) worst = run + first_gap;
    h->gap = h->n ? (int)(worst * STEP) : 360;
    h->covered = covered_deg(s);
}

static float curve(const float c[5], int terms, float th)
{
    float y = c[0] + c[1] * cosf(th) + c[2] * sinf(th);
    if (terms == 5) y += c[3] * cosf(2 * th) + c[4] * sinf(2 * th);
    return y;
}

static float slope(const float c[5], int terms, float th)
{
    float y = -c[1] * sinf(th) + c[2] * cosf(th);
    if (terms == 5) y += -2 * c[3] * sinf(2 * th) + 2 * c[4] * cosf(2 * th);
    return y;
}

/* The pattern at relative angle `rel` degrees, linearly between bins. */
static float pat_at(const ls_df_pattern_t *p, float rel)
{
    const float x = wrap360(rel) / PSTEP;
    const int i = (int)x % LS_DF_PAT_BINS, j = (i + 1) % LS_DF_PAT_BINS;
    const float f = x - floorf(x);
    return p->g[i] * (1 - f) + p->g[j] * f;
}

/* Weighted correlation of the heard levels with the pattern turned to
   `truth`, and the gain and offset that best map one onto the other. */
static float match_at(const heard_t *h, const ls_df_pattern_t *p, float truth, float *gain, float *resid2)
{
    float mv = 0, mg = 0;
    for (int i = 0; i < h->n; i++) { mv += h->w[i] * h->v[i]; mg += h->w[i] * pat_at(p, h->th[i] * DEGF - truth); }
    mv /= h->sw; mg /= h->sw;
    float cvg = 0, vv = 0, gg = 0;
    for (int i = 0; i < h->n; i++) {
        const float dv = h->v[i] - mv, dg = pat_at(p, h->th[i] * DEGF - truth) - mg;
        cvg += h->w[i] * dv * dg; vv += h->w[i] * dv * dv; gg += h->w[i] * dg * dg;
    }
    if (!(vv > 1e-6f) || !(gg > 1e-6f)) return 0;
    const float b = cvg / gg;
    if (gain) *gain = b;
    if (resid2) *resid2 = fmaxf(0.0f, (vv - b * cvg) / fmaxf(1.0f, h->sw - 2));
    return cvg / sqrtf(vv * gg);
}

static bool fit_pattern(const heard_t *h, const ls_df_pattern_t *p, ls_df_fit_t *out)
{
    float best = -2, best_at = 0;
    EXT_RAM_BSS_ATTR static float rho[180];
    for (int k = 0; k < 180; k++) {
        rho[k] = match_at(h, p, k * 2.0f, NULL, NULL);
        if (rho[k] > best) { best = rho[k]; best_at = k; }
    }
    const int k = (int)best_at;
    const float l = rho[(k + 179) % 180], r = rho[(k + 1) % 180];
    float off = 0;
    const float den = l - 2 * rho[k] + r;
    if (fabsf(den) > 1e-6f) off = fmaxf(-0.5f, fminf(0.5f, 0.5f * (l - r) / den));
    const float truth = wrap360((k + off) * 2.0f);
    float gain = 0, resid2 = 0;
    out->match = match_at(h, p, truth, &gain, &resid2);
    out->bearing = truth;
    float lo = INFINITY, hi = -INFINITY;
    for (int i = 0; i < LS_DF_PAT_BINS; i++) { lo = fminf(lo, p->g[i]); hi = fmaxf(hi, p->g[i]); }
    out->depth = gain * (hi - lo);
    out->r2 = out->match * out->match;
    /* Phase of a known shape: var = resid^2 / (gain^2 sum w g'^2). */
    float d2 = 0;
    for (int i = 0; i < h->n; i++) {
        const float rel = h->th[i] * DEGF - truth;
        const float gd = (pat_at(p, rel + 1.0f) - pat_at(p, rel - 1.0f)) * 0.5f * DEGF;
        d2 += h->w[i] * gd * gd;
    }
    out->sigma = gain > 0 && d2 > 0 ? SIGMA_K * sqrtf(resid2 / (gain * gain * d2)) * DEGF : 180.0f;
    out->sigma = fmaxf(0.5f, fminf(180.0f, out->sigma));
    return gain > 0 && out->match >= 0.4f && out->depth >= 3.0f;
}

bool ls_df_fit(const ls_df_sweep_t *s, ls_df_method_t method, const ls_df_pattern_t *pattern,
               ls_df_fit_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->bearing = out->sigma = out->alt = out->match = NAN;
    if (!s) return false;
    /* Off the caller's stack: FIND runs on the UI task. */
    EXT_RAM_BSS_ATTR static heard_t h;
    gather(s, &h);
    out->bins = h.n;
    out->gap = h.gap;
    const int coverage = h.covered;
    const bool null = method == LS_DF_NULL;
    if (coverage < (null ? 270 : 180) || h.gap > (null ? 90 : 150) || h.n < 8) return false;

    if (pattern && pattern->circles > 0) {
        out->valid = fit_pattern(&h, pattern, out);
        return out->valid;
    }

    /* The second harmonic needs most of the circle, or it bends the curve
       into whatever was not heard. */
    const int terms = h.gap <= 90 && h.n >= 12 && h.covered >= 300 ? 5 : 3;
    float a[5][6];
    memset(a, 0, sizeof(a));
    for (int i = 0; i < h.n; i++) {
        const float t = h.th[i];
        const float row[5] = { 1, cosf(t), sinf(t), cosf(2 * t), sinf(2 * t) };
        for (int j = 0; j < terms; j++) {
            for (int k = 0; k < terms; k++) a[j][k] += h.w[i] * row[j] * row[k];
            a[j][terms] += h.w[i] * row[j] * h.v[i];
        }
    }
    float sys[5 * 6];
    for (int j = 0; j < terms; j++) for (int k = 0; k <= terms; k++) sys[j * (terms + 1) + k] = a[j][k];
    if (!solve(sys, terms)) return false;
    float c[5] = { 0 };
    for (int j = 0; j < terms; j++) c[j] = sys[j * (terms + 1) + terms];

    /* The curve's extreme, to a degree, then between degrees. */
    EXT_RAM_BSS_ATTR static float y[360];
    float hi = -INFINITY, lo = INFINITY;
    int ihi = 0, ilo = 0;
    for (int d = 0; d < 360; d++) {
        y[d] = curve(c, terms, d * RADF);
        if (y[d] > hi) { hi = y[d]; ihi = d; }
        if (y[d] < lo) { lo = y[d]; ilo = d; }
    }
    const int e = null ? ilo : ihi;
    const float l = y[(e + 359) % 360], r = y[(e + 1) % 360], den = l - 2 * y[e] + r;
    const float off = fabsf(den) > 1e-9f ? fmaxf(-0.5f, fminf(0.5f, 0.5f * (l - r) / den)) : 0;
    float aim = (float)e + off;
    if (null) aim += 180.0f;
    out->bearing = wrap360(aim);
    out->depth = hi - lo;

    float mean = 0, ss_tot = 0, ss_res = 0, d2 = 0;
    for (int i = 0; i < h.n; i++) mean += h.w[i] * h.v[i];
    mean /= h.sw;
    for (int i = 0; i < h.n; i++) {
        const float f = curve(c, terms, h.th[i]);
        ss_tot += h.w[i] * (h.v[i] - mean) * (h.v[i] - mean);
        ss_res += h.w[i] * (h.v[i] - f) * (h.v[i] - f);
        /* Turning the curve by d moves level i by -slope * d, so the
           steeper it is under the heard directions the better the aim. */
        const float sl = slope(c, terms, h.th[i]);
        d2 += h.w[i] * sl * sl;
    }
    out->r2 = ss_tot > 0 ? 1.0f - ss_res / ss_tot : 0;
    const float resid2 = ss_res / fmaxf(1.0f, h.sw - terms);
    out->sigma = d2 > 1e-9f ? SIGMA_K * sqrtf(resid2 / d2) * DEGF : 180.0f;
    out->sigma = fmaxf(0.5f, fminf(180.0f, out->sigma));
    const float a1 = sqrtf(c[1] * c[1] + c[2] * c[2]), a2 = terms == 5 ? sqrtf(c[3] * c[3] + c[4] * c[4]) : 0;
    if (a2 > a1) { out->ambiguous = true; out->alt = wrap360(out->bearing + 180.0f); }
    out->valid = out->depth >= 3.0f;
    return out->valid;
}

void ls_df_fit_estimate(const ls_df_sweep_t *s, const ls_df_fit_t *f, ls_df_estimate_t *out)
{
    memset(out, 0, sizeof(*out));
    out->bearing = out->spread = NAN;
    out->coverage = s ? covered_deg(s) : 0;
    out->contrast = s ? s->top - s->floor : 0;
    if (!f->valid) return;
    out->valid = true;
    out->bearing = f->bearing;
    out->spread = fminf(90.0f, 2.0f * f->sigma);
}

void ls_df_pattern_clear(ls_df_pattern_t *p) { memset(p, 0, sizeof(*p)); }

bool ls_df_pattern_learn(ls_df_pattern_t *p, const ls_df_sweep_t *s, float truth)
{
    if (!p || !s || !isfinite(truth)) return false;
    float sum[LS_DF_PAT_BINS] = { 0 }, cnt[LS_DF_PAT_BINS] = { 0 };
    int heard = 0;
    for (int i = 0; i < LS_DF_BINS; i++) {
        const float v = ls_df_typical(s, i);
        if (!isfinite(v)) continue;
        heard++;
        const int k = (int)lroundf(wrap360(i * STEP - truth) / PSTEP) % LS_DF_PAT_BINS;
        sum[k] += v; cnt[k] += 1;
    }
    if (!heard || covered_deg(s) < 300) return false;
    float g[LS_DF_PAT_BINS];
    int have = 0;
    for (int k = 0; k < LS_DF_PAT_BINS; k++) { g[k] = cnt[k] ? sum[k] / cnt[k] : NAN; have += cnt[k] > 0; }
    if (have < LS_DF_PAT_BINS / 2) return false;
    /* Holes filled from their neighbours round the circle. */
    for (int k = 0; k < LS_DF_PAT_BINS; k++) {
        if (isfinite(g[k])) continue;
        int a = k, b = k, da = 0, db = 0;
        do { a = (a + LS_DF_PAT_BINS - 1) % LS_DF_PAT_BINS; da++; } while (!cnt[a]);
        do { b = (b + 1) % LS_DF_PAT_BINS; db++; } while (!cnt[b]);
        g[k] = (sum[a] / cnt[a] * db + sum[b] / cnt[b] * da) / (da + db);
    }
    float mean = 0;
    for (int k = 0; k < LS_DF_PAT_BINS; k++) mean += g[k];
    mean /= LS_DF_PAT_BINS;
    /* An average of the circles, the newest weighing at least an eighth. */
    const float keep = p->circles < 7 ? (float)p->circles / (p->circles + 1) : 7.0f / 8.0f;
    for (int k = 0; k < LS_DF_PAT_BINS; k++) p->g[k] = p->g[k] * keep + (g[k] - mean) * (1 - keep);
    if (p->circles < UINT16_MAX) p->circles++;
    return true;
}

void ls_df_pattern_pack(const ls_df_pattern_t *p, int8_t out[LS_DF_PAT_BINS])
{
    for (int k = 0; k < LS_DF_PAT_BINS; k++) {
        const long v = lroundf(p->g[k] * 2.0f);
        out[k] = (int8_t)(v > 127 ? 127 : v < -127 ? -127 : v);
    }
}

void ls_df_pattern_unpack(ls_df_pattern_t *p, const int8_t in[LS_DF_PAT_BINS], uint16_t circles)
{
    for (int k = 0; k < LS_DF_PAT_BINS; k++) p->g[k] = in[k] * 0.5f;
    p->circles = circles;
}
