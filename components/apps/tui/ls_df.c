#include "ls_df.h"

#include <math.h>
#include <string.h>

#define STEP (360.0f / LS_DF_BINS)

void ls_df_clear(ls_df_sweep_t *s) { if (s) memset(s, 0, sizeof(*s)); }

static int bin_of(float heading)
{
    float h = fmodf(heading, 360.0f);
    if (h < 0) h += 360.0f;
    int b = (int)(h / STEP + 0.5f);
    return b % LS_DF_BINS;
}

static void bounds(ls_df_sweep_t *s)
{
    bool any = false;
    for (int i = 0; i < LS_DF_BINS; i++) {
        if (!s->count[i]) continue;
        if (!any || s->level[i] < s->floor) s->floor = s->level[i];
        if (!any || s->level[i] > s->top) s->top = s->level[i];
        any = true;
    }
    if (!any) s->floor = s->top = 0;
}

void ls_df_add(ls_df_sweep_t *s, float heading, float level, int64_t now_us)
{
    if (!s || !isfinite(heading) || !isfinite(level)) return;
    const int b = bin_of(heading);
    /* The strongest level heard facing that way: a turn samples each
       direction a few times and fading only ever takes signal away. */
    if (!s->count[b] || level >= s->level[b]) { s->level[b] = level; s->peak_us[b] = now_us; }
    s->last[b] = level;
    if (s->count[b] < UINT16_MAX) s->count[b]++;
    s->seen_us[b] = now_us;
    s->samples++;
    bounds(s);
}

void ls_df_age(ls_df_sweep_t *s, int64_t now_us, int64_t max_age_us)
{
    if (!s) return;
    for (int i = 0; i < LS_DF_BINS; i++)
        if (s->count[i] && now_us - s->seen_us[i] > max_age_us) s->count[i] = 0;
    bounds(s);
}

void ls_df_decay(ls_df_sweep_t *s, int64_t now_us, const ls_df_decay_t *d)
{
    if (!s || !d) return;
    const float dt = s->decayed_us && now_us > s->decayed_us ? (float)(now_us - s->decayed_us) / 1e6f : 0.0f;
    s->decayed_us = now_us;
    if (d->decay_db_s <= 0 || dt <= 0) return;
    const int64_t hold = (int64_t)(fmaxf(0.0f, d->hold_s) * 1e6f);
    for (int i = 0; i < LS_DF_BINS; i++) {
        if (!s->count[i] || now_us - s->peak_us[i] < hold) continue;
        /* Only the part of dt past the hold counts, so a peak does not
           drop a whole frame's worth the moment its hold runs out. */
        const float past = fminf(dt, (float)(now_us - s->peak_us[i] - hold) / 1e6f);
        const float v = s->level[i] - d->decay_db_s * past;
        s->level[i] = v > s->last[i] ? v : s->last[i];
    }
    bounds(s);
}

/* Each populated bin with its populated neighbours, so one lucky sample
   does not decide the bearing. */
static float smoothed(const ls_df_sweep_t *s, int i)
{
    float sum = 0, w = 0;
    for (int k = -1; k <= 1; k++) {
        const int j = (i + k + LS_DF_BINS) % LS_DF_BINS;
        if (!s->count[j]) continue;
        const float wk = k ? 0.5f : 1.0f;
        sum += wk * s->level[j]; w += wk;
    }
    return w ? sum / w : NAN;
}

bool ls_df_estimate(const ls_df_sweep_t *s, ls_df_method_t method, ls_df_estimate_t *out)
{
    if (!s || !out) return false;
    memset(out, 0, sizeof(*out));
    out->bearing = out->spread = NAN;
    float v[LS_DF_BINS];
    int covered = 0, best = -1;
    for (int i = 0; i < LS_DF_BINS; i++) {
        v[i] = s->count[i] ? smoothed(s, i) : NAN;
        if (!s->count[i]) continue;
        covered++;
        const bool better = best < 0 || (method == LS_DF_PEAK ? v[i] > v[best] : v[i] < v[best]);
        if (better) best = i;
    }
    out->coverage = (int)(covered * STEP);
    out->contrast = s->top - s->floor;
    if (best < 0) return false;
    /* A peak needs the far side seen to be a peak; a null needs nearly the
       whole circle, because a missing direction could hold a deeper one. */
    const int need = method == LS_DF_PEAK ? 180 : 270;
    if (out->coverage < need || out->contrast < 3.0f) return false;
    /* Parabola through the extreme and its neighbours for sub-bin aim. */
    float offset = 0;
    const int l = (best + LS_DF_BINS - 1) % LS_DF_BINS, r = (best + 1) % LS_DF_BINS;
    if (isfinite(v[l]) && isfinite(v[r])) {
        const float den = v[l] - 2 * v[best] + v[r];
        if (fabsf(den) > 1e-6f) offset = 0.5f * (v[l] - v[r]) / den;
        if (offset > 0.5f) offset = 0.5f;
        if (offset < -0.5f) offset = -0.5f;
    }
    float bearing = (best + offset) * STEP;
    /* Width of the lobe: bins within 3 dB of the extreme, walking out. */
    const float edge = method == LS_DF_PEAK ? v[best] - 3.0f : v[best] + 3.0f;
    int left = 0, right = 0;
    for (int k = 1; k < LS_DF_BINS / 2; k++) {
        const int j = (best - k + LS_DF_BINS) % LS_DF_BINS;
        if (!isfinite(v[j]) || (method == LS_DF_PEAK ? v[j] < edge : v[j] > edge)) break;
        left = k;
    }
    for (int k = 1; k < LS_DF_BINS / 2; k++) {
        const int j = (best + k) % LS_DF_BINS;
        if (!isfinite(v[j]) || (method == LS_DF_PEAK ? v[j] < edge : v[j] > edge)) break;
        right = k;
    }
    bearing += (right - left) * STEP * 0.5f;
    if (method == LS_DF_NULL) bearing += 180.0f;
    bearing = fmodf(bearing + 720.0f, 360.0f);
    out->bearing = bearing;
    out->spread = fmaxf(STEP * 0.5f, (left + right + 1) * STEP * 0.5f);
    out->valid = true;
    return true;
}

int ls_df_peaks(const ls_df_sweep_t *s, float min_prominence_db, ls_df_peak_t *out, int max)
{
    if (!s || !out || max < 1) return 0;
    float v[LS_DF_BINS];
    float lowest = INFINITY;
    for (int i = 0; i < LS_DF_BINS; i++) {
        v[i] = s->count[i] ? smoothed(s, i) : NAN;
        if (isfinite(v[i]) && v[i] < lowest) lowest = v[i];
    }
    int n = 0;
    for (int i = 0; i < LS_DF_BINS; i++) {
        if (!isfinite(v[i])) continue;
        const float l = v[(i + LS_DF_BINS - 1) % LS_DF_BINS], r = v[(i + 1) % LS_DF_BINS];
        /* Strictly above the left, level with or above the right: a flat
           top is one peak, at its first bin. */
        if ((isfinite(l) && !(v[i] > l)) || (isfinite(r) && v[i] < r)) continue;
        /* Walk each way to something higher, keeping the lowest ground. An
           unheard bin is not ground: nothing is known to be low there. */
        float key_col = -INFINITY;
        bool higher = false;
        for (int dir = -1; dir <= 1; dir += 2) {
            float low = v[i];
            for (int k = 1; k < LS_DF_BINS; k++) {
                const float w = v[(i + dir * k + LS_DF_BINS * 2) % LS_DF_BINS];
                if (!isfinite(w)) continue;
                if (w > v[i]) { higher = true; break; }
                if (w < low) low = w;
            }
            if (low > key_col) key_col = low;
        }
        const float prom = higher ? v[i] - key_col : v[i] - lowest;
        if (prom < min_prominence_db) continue;
        ls_df_peak_t p = { .bearing = i * STEP, .level = v[i], .prominence = prom };
        int at;
        if (n < max) at = n++;
        else if (out[max - 1].level < p.level) at = max - 1;
        else continue;
        while (at > 0 && out[at - 1].level < p.level) { out[at] = out[at - 1]; at--; }
        out[at] = p;
    }
    return n;
}

bool ls_df_triangulate(const ls_df_bearing_t *b, int n, ls_df_fix_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!b || n < 2) return false;
    double lat0 = 0, lon0 = 0;
    for (int i = 0; i < n; i++) { lat0 += b[i].lat; lon0 += b[i].lon; }
    lat0 /= n; lon0 /= n;
    const double kx = 111320.0 * cos(lat0 * M_PI / 180.0), ky = 110540.0;
    /* Two lines that nearly agree in direction cross nowhere useful. */
    float widest = 0;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++) {
            float d = fabsf(fmodf(b[i].bearing - b[j].bearing + 540.0f, 360.0f) - 180.0f);
            if (d > 90) d = 180 - d;
            if (d > widest) widest = d;
        }
    if (widest < 15.0f) return false;
    double x = 0, y = 0, cov[3] = { 0, 0, 0 };
    /* Two passes: the first places the crossing, the second weights each
       line by how far off it could be at that distance. */
    for (int pass = 0; pass < 2; pass++) {
        double a11 = 0, a12 = 0, a22 = 0, b1 = 0, b2 = 0;
        for (int i = 0; i < n; i++) {
            const double px = (b[i].lon - lon0) * kx, py = (b[i].lat - lat0) * ky;
            const double t = b[i].bearing * M_PI / 180.0;
            const double nx = cos(t), ny = -sin(t);          /* normal to the line */
            double w = 1.0;
            if (pass) {
                const double dist = fmax(50.0, hypot(x - px, y - py));
                const double spread = fmax(1.0, b[i].spread) * M_PI / 180.0;
                const double e = dist * sin(spread);
                w = 1.0 / (e * e);
            }
            a11 += w * nx * nx; a12 += w * nx * ny; a22 += w * ny * ny;
            const double c = nx * px + ny * py;
            b1 += w * nx * c; b2 += w * ny * c;
        }
        const double det = a11 * a22 - a12 * a12;
        if (fabs(det) < 1e-18) return false;
        x = (a22 * b1 - a12 * b2) / det;
        y = (a11 * b2 - a12 * b1) / det;
        cov[0] = a22 / det; cov[1] = -a12 / det; cov[2] = a11 / det;
    }
    /* A crossing behind any of the observers is two lines, not a fix. */
    for (int i = 0; i < n; i++) {
        const double px = (b[i].lon - lon0) * kx, py = (b[i].lat - lat0) * ky;
        const double t = b[i].bearing * M_PI / 180.0;
        if ((x - px) * sin(t) + (y - py) * cos(t) <= 0) return false;
    }
    const double tr = cov[0] + cov[2], dt = cov[0] * cov[2] - cov[1] * cov[1];
    const double big = tr / 2 + sqrt(fmax(0.0, tr * tr / 4 - dt));
    out->lat = lat0 + y / ky;
    out->lon = lon0 + x / kx;
    out->radius_m = (float)sqrt(big);
    out->used = n;
    out->valid = true;
    return true;
}

float ls_df_iq_band_db(const uint8_t *iq, int pairs, uint32_t rate_hz,
                       int32_t offset_hz, uint32_t bw_hz)
{
    if (!iq || pairs < 64 || !rate_hz) return NAN;
    /* Bins of rate/pairs Hz; the band's centre bin and its neighbours out
       to half the bandwidth. At least one bin, at most 31. Single precision:
       the P4 has no double-precision FPU, and 2048 samples lose nothing. */
    const float width = (float)rate_hz / pairs;
    int half = (int)(bw_hz / 2 / width);
    if (half > 15) half = 15;
    const int centre = (int)lroundf(offset_hz / width);
    float mean_i = 0, mean_q = 0;
    for (int n = 0; n < pairs; n++) { mean_i += iq[2 * n]; mean_q += iq[2 * n + 1]; }
    mean_i /= pairs; mean_q /= pairs;
    float power = 0;
    for (int k = centre - half; k <= centre + half; k++) {
        const float w = -2.0f * (float)M_PI * k / pairs;
        const float cw = cosf(w), sw = sinf(w);
        float re = 0, im = 0, c = 1, s = 0;
        for (int n = 0; n < pairs; n++) {
            const float x = (iq[2 * n] - mean_i) * (1.0f / 127.5f), y = (iq[2 * n + 1] - mean_q) * (1.0f / 127.5f);
            re += x * c - y * s;
            im += x * s + y * c;
            const float nc = c * cw - s * sw;
            s = c * sw + s * cw; c = nc;
        }
        power += (re * re + im * im) / ((float)pairs * pairs);
    }
    return power > 1e-15f ? 10.0f * log10f(power) : -150.0f;
}
