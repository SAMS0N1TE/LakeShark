/* Broadcast FM multiplex: stereo and RDS. See wfm_mpx.h. */
#include "wfm_mpx.h"

#include <math.h>
#include <string.h>
#include "ls_cold.h"

#define TWO_PI 6.28318530718f
#define PILOT_HZ 19000.0f

/* Pilot PLL: loop bandwidth about 30 Hz, damping 0.707, at 256 kHz. The
   phase detector is normalised to a nominal 9% pilot so its gain is about
   one radian per radian. */
#define PLL_ALPHA  3.13e-4f
#define PLL_BETA   4.9e-8f
#define PLL_KD     (0.09f / 2.0f)
/* How far the loop may pull from 19 kHz, in Hz: a pilot is +-2 Hz, and a
   dongle's clock adds a few more. */
#define PLL_PULL_HZ 12.0f
/* Coherent pilot arm: about 8 ms. Locked above a 3.5% pilot with the
   quadrature arm well below it, for 50 ms. */
#define PILOT_LP     (1.0f / 2048.0f)
#define PILOT_MIN    (0.035f / 2.0f)
#define LOCK_SAMPLES (WFM_MPX_OUT / 20)

/* Costas loop at 16 kHz, about 20 Hz. */
#define COSTAS_ALPHA (3.33e-3f / TWO_PI)
#define COSTAS_BETA  (5.6e-6f / TWO_PI)
#define COSTAS_PULL  (40.0f / WFM_MPX_OUT)

static inline void sincos_cyc(const float *tab, float ph, float *s, float *c)
{
    const float x = ph * 256.0f;
    int i = (int)x;
    if (i < 0) i = 0;
    if (i > 255) i = 255;
    const float f = x - (float)i;
    *s = tab[i] + f * (tab[i + 1] - tab[i]);
    *c = tab[i + 64] + f * (tab[i + 65] - tab[i + 64]);
}

static inline float wrap01(float p)
{
    if (p >= 1.0f) p -= 1.0f;
    else if (p < 0.0f) p += 1.0f;
    return p;
}

LS_COLD void wfm_mpx_reset(wfm_mpx_t *m)
{
    m->fr0 = PILOT_HZ / (float)WFM_MPX_RATE;
    m->ph = 0.0f;
    m->fr = m->fr0;
    m->cycles = 0;
    m->pi_lp = m->pq_lp = 0.0f;
    m->lock_n = 0;
    m->pilot = m->stereo = false;
    m->pos = 0;
    memset(m->sum, 0, sizeof(m->sum));
    memset(m->dif, 0, sizeof(m->dif));
    memset(m->ri, 0, sizeof(m->ri));
    memset(m->rq, 0, sizeof(m->rq));
    m->de_l = m->de_r = 0.0f;
    m->lvl_l = m->lvl_r = 0.0f;
    m->pilot_level = 0.0f;
    memset(m->zi, 0, sizeof(m->zi));
    memset(m->zq, 0, sizeof(m->zq));
    m->zpos = 0;
    m->cph = m->cfr = 0.0f;
    m->cpow = 1e-6f;
    memset(m->acc, 0, sizeof(m->acc));
    memset(m->en, 0, sizeof(m->en));
    for (int k = 0; k < WFM_RDS_PHASES; ++k) m->prevq[k] = 0.0f;
    m->best = 0;
    m->last_sym = 0;
    m->rds_level = 0.0f;
    rds_reset(&m->rds);
}

LS_COLD void wfm_mpx_init(wfm_mpx_t *m)
{
    memset(m, 0, sizeof(*m));
    for (int k = 0; k < 256 + 65; ++k) m->tab[k] = sinf(TWO_PI * (float)k / 256.0f);

    /* sinc^2 and sinc^3 decimators as polyphase weights: a sample at
       position p of a 16-sample block feeds this block's output and the
       next one or two, with the cascaded box kernel's weight at its
       distance from each output. */
    float h2[31], h3[48];
    for (int k = 0; k < 31; ++k) h2[k] = (float)(k < 16 ? k + 1 : 31 - k);
    for (int k = 0; k < 48; ++k) {
        float s = 0.0f;
        for (int i = 0; i < 16; ++i) if (k - i >= 0 && k - i < 31) s += h2[k - i];
        h3[k] = s;
    }
    for (int p = 0; p < WFM_MPX_DECIM; ++p) {
        m->w2[p][0] = h2[15 - p] / 256.0f;
        m->w2[p][1] = (31 - p < 31 ? h2[31 - p] : 0.0f) / 256.0f;
        m->w3[p][0] = h3[15 - p] / 4096.0f;
        m->w3[p][1] = h3[31 - p] / 4096.0f;
        m->w3[p][2] = h3[47 - p] / 4096.0f;
    }
    /* RDS channel filter: windowed sinc, 2.4 kHz at 16 kHz. */
    float sum = 0.0f;
    const float fc = 2400.0f / (float)WFM_MPX_OUT;
    for (int k = 0; k < WFM_RDS_TAPS; ++k) {
        const float n = (float)k - (WFM_RDS_TAPS - 1) * 0.5f;
        const float v = n != 0.0f ? sinf(TWO_PI * fc * n) / (3.14159265f * n) : 2.0f * fc;
        m->fir[k] = v * (0.54f - 0.46f * cosf(TWO_PI * (float)k / (WFM_RDS_TAPS - 1)));
        sum += m->fir[k];
    }
    for (int k = 0; k < WFM_RDS_TAPS; ++k) m->fir[k] /= sum;
    /* 75 us de-emphasis, one pole at 16 kHz. */
    m->de_a = 1.0f - expf(-1.0f / ((float)WFM_MPX_OUT * 75e-6f));
    m->rds_on = true;
    rds_init(&m->rds, NULL, NULL);
    wfm_mpx_reset(m);
}

void wfm_mpx_set_rds(wfm_mpx_t *m, bool on)
{
    if (m->rds_on == on) return;
    m->rds_on = on;
    if (!on) rds_reset(&m->rds);
}

void wfm_mpx_set_mono(wfm_mpx_t *m, bool mono) { m->force_mono = mono; }

/* One 16 kHz RDS sample: channel filter, Costas, and the biphase bank. */
static void rds_sample(wfm_mpx_t *m, float re, float im, float bitph)
{
    const int n = WFM_RDS_TAPS;
    m->zi[m->zpos] = m->zi[m->zpos + n] = re;
    m->zq[m->zpos] = m->zq[m->zpos + n] = im;
    if (++m->zpos == n) m->zpos = 0;
    float fi = 0.0f, fq = 0.0f;
    const float *xi = &m->zi[m->zpos], *xq = &m->zq[m->zpos];
    for (int k = 0; k < n; ++k) { fi += m->fir[k] * xi[k]; fq += m->fir[k] * xq[k]; }

    float s, c;
    sincos_cyc(m->tab, m->cph, &s, &c);
    const float yr = fi * c + fq * s;
    const float yi = fq * c - fi * s;
    m->cpow += 0.002f * (yr * yr + yi * yi - m->cpow);
    const float err = yr * yi / (m->cpow + 1e-12f);
    m->cfr += COSTAS_BETA * err;
    if (m->cfr > COSTAS_PULL) m->cfr = COSTAS_PULL;
    if (m->cfr < -COSTAS_PULL) m->cfr = -COSTAS_PULL;
    m->cph = wrap01(m->cph + m->cfr + COSTAS_ALPHA * err);
    m->rds_level = sqrtf(m->cpow);

    /* Sixteen integrate-and-dump filters, one per bit phase: + over the
       first half of the biphase symbol, - over the second. */
    for (int k = 0; k < WFM_RDS_PHASES; ++k) {
        const float q = wrap01(bitph - (float)k * (1.0f / WFM_RDS_PHASES));
        if (q < m->prevq[k]) {
            const float a = m->acc[k];
            m->en[k] += (1.0f / 64.0f) * (fabsf(a) - m->en[k]);
            if (k == m->best) {
                const int sym = a > 0.0f;
                rds_push_bit(&m->rds, sym ^ m->last_sym);
                m->last_sym = sym;
            }
            m->acc[k] = 0.0f;
            if (k == WFM_RDS_PHASES - 1) {
                /* Change phase only on a clear margin, and only between bits
                   of the current one, so no bit is doubled or dropped. */
                int b = m->best;
                for (int j = 0; j < WFM_RDS_PHASES; ++j) if (m->en[j] > m->en[b]) b = j;
                if (b != m->best && m->en[b] > 1.25f * m->en[m->best]) m->best = b;
            }
        }
        m->prevq[k] = q;
        m->acc[k] += q < 0.5f ? yr : -yr;
    }
}

int wfm_mpx_process(wfm_mpx_t *m, const int16_t *disc, int n,
                    int16_t *left, int16_t *right, int max)
{
    const float k_in = 1.0f / WFM_MPX_FULL;
    const float lo = m->fr0 - PLL_PULL_HZ / (float)WFM_MPX_RATE;
    const float hi = m->fr0 + PLL_PULL_HZ / (float)WFM_MPX_RATE;
    int out = 0;
    for (int i = 0; i < n; ++i) {
        const float x = (float)disc[i] * k_in;
        float s1, c1;
        sincos_cyc(m->tab, m->ph, &s1, &c1);

        /* Pilot: the in-phase arm is its amplitude over two, the quadrature
           arm the phase error. */
        const float e = -x * s1;
        m->pi_lp += PILOT_LP * (x * c1 - m->pi_lp);
        m->pq_lp += PILOT_LP * (e - m->pq_lp);
        const float en = e * (1.0f / PLL_KD);
        m->fr += PLL_BETA / TWO_PI * en;
        if (m->fr < lo) m->fr = lo;
        if (m->fr > hi) m->fr = hi;
        m->ph += m->fr + PLL_ALPHA / TWO_PI * en;
        if (m->ph >= 1.0f) { m->ph -= 1.0f; m->cycles++; }
        else if (m->ph < 0.0f) { m->ph += 1.0f; m->cycles--; }

        /* With the pilot locked, take it out before anything reads the
           multiplex: the CIC would otherwise alias what is left of it. */
        const float xc = m->pilot ? x - 2.0f * m->pi_lp * c1 : x;
        /* The subcarrier is sin(2 phi) where phi = theta + pi/2, the NCO
           having locked in quadrature: -sin(2 theta). */
        const float s2 = 2.0f * s1 * c1;
        const float d = -2.0f * s2 * xc;
        const float *w2 = m->w2[m->pos];
        m->sum[0] += w2[0] * xc; m->sum[1] += w2[1] * xc;
        m->dif[0] += w2[0] * d;  m->dif[1] += w2[1] * d;
        if (m->rds_on) {
            const float c3 = c1 * (4.0f * c1 * c1 - 3.0f);
            const float s3 = s1 * (3.0f - 4.0f * s1 * s1);
            const float a = 2.0f * xc * c3, b = -2.0f * xc * s3;
            const float *w3 = m->w3[m->pos];
            m->ri[0] += w3[0] * a; m->ri[1] += w3[1] * a; m->ri[2] += w3[2] * a;
            m->rq[0] += w3[0] * b; m->rq[1] += w3[1] * b; m->rq[2] += w3[2] * b;
        }
        if (++m->pos < WFM_MPX_DECIM) continue;
        m->pos = 0;

        const float sum = m->sum[0], dif = m->dif[0];
        m->sum[0] = m->sum[1]; m->sum[1] = 0.0f;
        m->dif[0] = m->dif[1]; m->dif[1] = 0.0f;

        /* Pilot lock, judged at 16 kHz. */
        const bool strong = m->pi_lp > PILOT_MIN && m->pi_lp > 3.0f * fabsf(m->pq_lp);
        if (strong) { if (m->lock_n < LOCK_SAMPLES) m->lock_n++; }
        else if (m->lock_n > 0) m->lock_n--;
        if (!m->pilot && m->lock_n >= LOCK_SAMPLES) m->pilot = true;
        if (m->pilot && m->lock_n == 0) m->pilot = false;
        m->stereo = m->pilot && !m->force_mono;
        m->pilot_level = m->pilot ? 2.0f * m->pi_lp : 0.0f;

        if (m->rds_on) {
            const float re = m->ri[0], im = m->rq[0];
            m->ri[0] = m->ri[1]; m->ri[1] = m->ri[2]; m->ri[2] = 0.0f;
            m->rq[0] = m->rq[1]; m->rq[1] = m->rq[2]; m->rq[2] = 0.0f;
            const float bitph = ((float)(m->cycles & 15u) + m->ph) * (1.0f / 16.0f);
            rds_sample(m, re, im, bitph);
        }

        const float l = m->stereo ? sum + dif : sum;
        const float r = m->stereo ? sum - dif : sum;
        m->de_l += m->de_a * (l - m->de_l);
        m->de_r += m->de_a * (r - m->de_r);
        m->lvl_l += 0.002f * (fabsf(m->de_l) - m->lvl_l);
        m->lvl_r += 0.002f * (fabsf(m->de_r) - m->lvl_r);
        if (out >= max) continue;
        float vl = m->de_l * WFM_MPX_FULL, vr = m->de_r * WFM_MPX_FULL;
        if (!right) vl = 0.5f * (vl + vr);
        if (vl > 32767.0f) vl = 32767.0f;
        if (vl < -32768.0f) vl = -32768.0f;
        left[out] = (int16_t)vl;
        if (right) {
            if (vr > 32767.0f) vr = 32767.0f;
            if (vr < -32768.0f) vr = -32768.0f;
            right[out] = (int16_t)vr;
        }
        out++;
    }
    return out;
}
