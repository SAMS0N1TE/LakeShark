/* See rflab_meas.h. Two passes over one captured block: statistics and
   averaged spectra, then the discriminator refinement once the peak is
   known. The P4 has a single-precision FPU, so the per-sample work is float
   and only the per-frame totals are carried in double. */
#include "rflab_meas.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PI_F      3.14159265f
#define LOBE      3               /* bins each side of the peak: Hann's main lobe and a margin */
#define DISC_L    (RFLAB_N / 8)   /* discriminator boxcar: passes about +-4 bins */
#define TINY      1e-20f
#define LOG10_E   0.43429448f

void rflab_init(rflab_t *w)
{
    memset(w, 0, sizeof(*w));
    for (int k = 0; k < RFLAB_N / 2; k++) {
        const float a = -2.0f * PI_F * (float)k / (float)RFLAB_N;
        w->tw_re[k] = cosf(a);
        w->tw_im[k] = sinf(a);
    }
    /* Hann. A rectangular window smears a strong carrier across the whole
       span in sidelobes, which reads as signal everywhere. */
    for (int i = 0; i < RFLAB_N; i++)
        w->win[i] = 0.5f - 0.5f * cosf(2.0f * PI_F * (float)i / (float)(RFLAB_N - 1));
    w->ready = true;
}

static void fft512(rflab_t *w)
{
    float *re = w->re, *im = w->im;
    for (int i = 1, j = 0; i < RFLAB_N; i++) {
        int bit = RFLAB_N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t;
            t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= RFLAB_N; len <<= 1) {
        const int half = len >> 1, step = RFLAB_N / len;
        for (int i = 0; i < RFLAB_N; i += len)
            for (int k = 0; k < half; k++) {
                const float cr = w->tw_re[k * step], ci = w->tw_im[k * step];
                const int a = i + k, b = i + k + half;
                const float br = re[b] * cr - im[b] * ci, bi = re[b] * ci + im[b] * cr;
                re[b] = re[a] - br; im[b] = im[a] - bi;
                re[a] = re[a] + br; im[a] = im[a] + bi;
            }
    }
}

/* One pair as fractions of full scale, and whether either rail is hit. */
static inline void pair_at(const int16_t *iq, size_t k, float *i, float *q, bool *clip)
{
    const int16_t *s = iq + 2 * k;
    *i = (float)s[0] * (1.0f / 32768.0f);
    *q = (float)s[1] * (1.0f / 32768.0f);
    *clip = s[0] >= 32767 || s[0] <= -32768 || s[1] >= 32767 || s[1] <= -32768 ||
            fabsf(*i) >= 0.9999f || fabsf(*q) >= 0.9999f;
}

static float db(float p) { return 10.0f * LOG10_E * logf(p > TINY ? p : TINY); }

/* k-th smallest of a[0..n), reordering a (Hoare's selection). */
static float select_k(float *a, int n, int k)
{
    int lo = 0, hi = n - 1;
    while (lo < hi) {
        const float pivot = a[(lo + hi) / 2];
        int i = lo, j = hi;
        while (i <= j) {
            while (a[i] < pivot) i++;
            while (a[j] > pivot) j--;
            if (i <= j) { const float t = a[i]; a[i] = a[j]; a[j] = t; i++; j--; }
        }
        if (k <= j) hi = j;
        else if (k >= i) lo = i;
        else return a[k];
    }
    return a[k];
}

static float bin_hz(int k, float rbw) { return (float)(k - RFLAB_N / 2) * rbw; }

bool rflab_measure(rflab_t *w, const rflab_cfg_t *cfg, const int16_t *iq,
                   size_t pairs, rflab_result_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!w || !cfg || !iq || !cfg->rate_hz || pairs < RFLAB_N) return false;
    if (!w->ready) rflab_init(w);
    const float fs = (float)cfg->rate_hz;
    const float rbw = fs / RFLAB_N;
    const int frames = (int)(pairs / RFLAB_N);
    const size_t used = (size_t)frames * RFLAB_N;

    /* ---- pass 1: moments and averaged spectra --------------------------- */
    memset(w->acc, 0, sizeof(w->acc));
    double si = 0, sq = 0, sii = 0, sqq = 0, siq = 0;
    uint32_t clipped = 0;
    double sw2 = 0;
    for (int n = 0; n < RFLAB_N; ++n) sw2 += (double)w->win[n] * w->win[n];
    for (int f = 0; f < frames; ++f) {
        float fi = 0, fq = 0, fii = 0, fqq = 0, fiq = 0;
        for (int n = 0; n < RFLAB_N; ++n) {
            float i, q; bool clip;
            pair_at(iq, (size_t)f * RFLAB_N + n, &i, &q, &clip);
            clipped += clip;
            fi += i; fq += q; fii += i * i; fqq += q * q; fiq += i * q;
            w->re[n] = i * w->win[n];
            w->im[n] = q * w->win[n];
        }
        si += fi; sq += fq; sii += fii; sqq += fqq; siq += fiq;
        fft512(w);
        for (int n = 0; n < RFLAB_N; ++n) w->acc[n] += w->re[n] * w->re[n] + w->im[n] * w->im[n];
    }
    w->frames = frames;
    /* Bins in fftshift order, scaled to sum to the mean power. */
    const float scale = (float)(1.0 / ((double)frames * RFLAB_N * sw2));
    double total = 0;
    for (int k = 0; k < RFLAB_N; ++k) {
        w->bin[k] = w->acc[(k + RFLAB_N / 2) & (RFLAB_N - 1)] * scale;
        total += w->bin[k];
    }

    out->valid = true;
    out->frames = frames;
    out->pairs = (uint32_t)used;
    out->rbw_hz = rbw;
    out->clip_pct = 100.0f * (float)clipped / (float)used;
    const double inv_n = 1.0 / (double)used;
    const double mi = si * inv_n, mq = sq * inv_n;
    out->dc_i = (float)mi;
    out->dc_q = (float)mq;
    out->dc_dbfs = db((float)(mi * mi + mq * mq));
    out->total_dbfs = db((float)((sii + sqq) * inv_n));
    const float vi = (float)(sii * inv_n - mi * mi), vq = (float)(sqq * inv_n - mq * mq);
    const float cv = (float)(siq * inv_n - mi * mq);
    if (vi > 0 && vq > 0) {
        const float g = sqrtf(vq / vi);            /* Q's amplitude over I's */
        float c = cv / sqrtf(vi * vq);
        if (c > 1) c = 1;
        if (c < -1) c = -1;
        const float phi = asinf(c);
        out->iq_gain_db = db(vi / vq);
        out->iq_phase_deg = phi * 180.0f / PI_F;
        const float num = 1 + 2 * g * cosf(phi) + g * g, den = 1 - 2 * g * cosf(phi) + g * g;
        out->image_db = den > 1e-12f ? db(num / den) : 99.0f;
    }

    /* ---- floor ----------------------------------------------------------- */
    memcpy(w->tmp, w->bin, sizeof(w->tmp));
    const float median = select_k(w->tmp, RFLAB_N, RFLAB_N / 2);
    const float c3 = 1.0f - 1.0f / (9.0f * (float)frames);
    const float noise = median / (c3 * c3 * c3);
    out->noise_dbfs_bin = db(noise);
    out->noise_dbfs_hz = db(noise / rbw);

    /* ---- peak -------------------------------------------------------------- */
    int pk = -1;
    for (int k = 1; k < RFLAB_N - 1; ++k) {
        if (abs(k - RFLAB_N / 2) <= cfg->dc_guard_bins && cfg->dc_guard_bins) continue;
        if (pk < 0 || w->bin[k] > w->bin[pk]) pk = k;
    }
    if (pk < 0) pk = RFLAB_N / 2;
    float delta = 0;
    {
        const float a = logf(w->bin[pk - 1] + TINY), b = logf(w->bin[pk] + TINY), c = logf(w->bin[pk + 1] + TINY);
        const float den = a - 2 * b + c;
        if (den < 0) delta = 0.5f * (a - c) / den;
        if (delta > 0.5f) delta = 0.5f;
        if (delta < -0.5f) delta = -0.5f;
    }
    float f_peak = ((float)(pk - RFLAB_N / 2) + delta) * rbw;
    float lobe = 0;
    int lobe_bins = 0;
    for (int k = pk - LOBE; k <= pk + LOBE; ++k)
        if (k >= 0 && k < RFLAB_N) { lobe += w->bin[k]; lobe_bins++; }
    float tone = lobe - noise * (float)lobe_bins;
    if (tone < noise * 0.01f) tone = noise * 0.01f;
    out->peak_dbfs = db(tone);
    out->carrier = w->bin[pk] > noise * 10.0f;

    /* ---- discriminator: mix the peak to 0 Hz, boxcar, mean phase step ---- */
    if (w->bin[pk] > noise * 30.0f && used >= 4 * DISC_L) {
        /* The mean comes out first, so a DC offset cannot pull a carrier
           toward 0 Hz - unless the peak is the DC, which is then measured. */
        const bool dc_peak = abs(pk - RFLAB_N / 2) <= LOBE;
        const float step = -2.0f * PI_F * f_peak / fs;
        const float cr = cosf(step), ci = sinf(step);
        float pr = 1, pi = 0;                /* the mixer's phasor */
        float yr_prev = 0, yi_prev = 0;
        double acc_r = 0, acc_i = 0;
        bool have_prev = false;
        for (size_t blk = 0; blk + DISC_L <= used; blk += DISC_L) {
            float yr = 0, yi = 0;
            for (int n = 0; n < DISC_L; ++n) {
                float i, q; bool clip;
                pair_at(iq, blk + n, &i, &q, &clip);
                if (!dc_peak) { i -= (float)mi; q -= (float)mq; }
                yr += i * pr - q * pi;
                yi += i * pi + q * pr;
                const float nr = pr * cr - pi * ci;
                pi = pr * ci + pi * cr;
                pr = nr;
            }
            const float mag = sqrtf(pr * pr + pi * pi);  /* keep the phasor on the circle */
            pr /= mag; pi /= mag;
            if (have_prev) {
                acc_r += (double)yr * yr_prev + (double)yi * yi_prev;
                acc_i += (double)yi * yr_prev - (double)yr * yi_prev;
            }
            yr_prev = yr; yi_prev = yi; have_prev = true;
        }
        if (acc_r != 0 || acc_i != 0) {
            f_peak += atan2f((float)acc_i, (float)acc_r) * fs / (2.0f * PI_F * DISC_L);
            out->fine = true;
        }
    }
    out->peak_offset_hz = f_peak;
    out->freq_err_hz = f_peak - (float)cfg->expect_offset_hz;
    const double rf = (double)cfg->center_hz + cfg->expect_offset_hz;
    out->freq_err_ppm = cfg->center_hz ? (float)(out->freq_err_hz / rf * 1e6) : 0;

    /* ---- channel: bins weighted by how much of each the channel covers --- */
    const float bw = cfg->chan_bw_hz ? (float)cfg->chan_bw_hz : 12500.0f;
    const float lo = (float)cfg->chan_offset_hz - bw / 2, hi = (float)cfg->chan_offset_hz + bw / 2;
    float chan = 0;
    for (int k = 0; k < RFLAB_N; ++k) {
        const float a = bin_hz(k, rbw) - rbw / 2, b = a + rbw;
        const float ov = (b < hi ? b : hi) - (a > lo ? a : lo);
        if (ov > 0) chan += w->bin[k] * ov / rbw;
    }
    out->chan_dbfs = db(chan);
    const float chan_bins = bw / rbw > 1 ? bw / rbw : 1;
    out->snr_db = db(tone) - db(noise * chan_bins);

    /* ---- 99% occupied bandwidth ------------------------------------------ */
    /* Of the signal, not of the floor: with the noise taken out of every bin
       a carrier 30 dB up reads its own width, not 99% of the span. When
       what stands above the floor is under a tenth of the power there is no
       signal to speak of and the raw spectrum is used, as an analyser would. */
    double net = 0;
    for (int k = 0; k < RFLAB_N; ++k) {
        w->tmp[k] = w->bin[k] > noise ? w->bin[k] - noise : 0;
        net += w->tmp[k];
    }
    out->obw_net = net >= total * 0.1;
    const float *occ = out->obw_net ? w->tmp : w->bin;
    if (out->obw_net) total = net;
    const double edge = total * 0.005;
    double cum = 0;
    float f_lo = bin_hz(0, rbw) - rbw / 2, f_hi = bin_hz(RFLAB_N - 1, rbw) + rbw / 2;
    bool got_lo = false;
    for (int k = 0; k < RFLAB_N; ++k) {
        const double next = cum + occ[k];
        const float left = bin_hz(k, rbw) - rbw / 2;
        if (!got_lo && next >= edge) {
            f_lo = left + (float)((edge - cum) / (occ[k] > 0 ? occ[k] : 1)) * rbw;
            got_lo = true;
        }
        if (next >= total - edge) {
            f_hi = left + (float)((total - edge - cum) / (occ[k] > 0 ? occ[k] : 1)) * rbw;
            break;
        }
        cum = next;
    }
    out->obw_lo_hz = f_lo;
    out->obw_hi_hz = f_hi;
    out->obw_hz = f_hi - f_lo;
    return true;
}

bool rflab_spectrum_db(const rflab_t *w, float *out, int n)
{
    if (!w || !out || n < 1 || !w->frames) return false;
    for (int o = 0; o < n; ++o) {
        int lo = (int)((int64_t)o * RFLAB_N / n), hi = (int)((int64_t)(o + 1) * RFLAB_N / n);
        if (hi <= lo) hi = lo + 1;
        float pk = 0;
        for (int k = lo; k < hi && k < RFLAB_N; ++k) if (w->bin[k] > pk) pk = w->bin[k];
        out[o] = db(pk);
    }
    return true;
}
