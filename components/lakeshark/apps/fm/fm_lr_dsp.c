#include "fm_lr_dsp.h"
#include <math.h>
#include <string.h>
#define PI 3.14159265358979323846f
void fm_lr_reset(fm_lr_dsp_t *s)
{
    memset((char *)s + offsetof(fm_lr_dsp_t, i), 0,
           sizeof(*s) - offsetof(fm_lr_dsp_t, i));
}
void fm_lr_set_dc_block(fm_lr_dsp_t *s, bool on)
{
    if (s->dc_block != on) { s->dc_i = s->dc_q = 0; }
    s->dc_block = on;
}
/* One input pair into the filter history, through the DC blocker when on. */
static inline void push_pair(fm_lr_dsp_t *s, float re, float im)
{
    re /= 32768.0f; im /= 32768.0f;
    if (s->dc_block) {
        const float k = 1.0f / (float)(1u << FM_LR_DC_SHIFT);
        s->dc_i += (re - s->dc_i) * k; s->dc_q += (im - s->dc_q) * k;
        re -= s->dc_i; im -= s->dc_q;
    }
    s->i[s->pos] = re; s->q[s->pos] = im;
    s->pos = (s->pos+1)%FM_LR_TAPS;
}
void fm_lr_init(fm_lr_dsp_t *s, unsigned channel_hz)
{
    memset(s, 0, sizeof(*s));
    /* Narrow channel filtering also quietens uncorrelated noise. Normalize
       its measured discriminator hiss to the shared squelch's 0..1 scale. */
    s->noise_scale = channel_hz == 12500 ? 1.9f : 1.2f;
    float fc = (channel_hz == 12500 ? 6000.0f : 11000.0f) / 125000.0f;
    float sum = 0;
    for (int k=0; k<FM_LR_TAPS; ++k) {
        int n = k - FM_LR_TAPS/2;
        float v = n ? sinf(2*PI*fc*n)/(PI*n) : 2*fc;
        s->taps[k] = v * (0.54f - 0.46f*cosf(2*PI*k/(FM_LR_TAPS-1)));
        sum += s->taps[k];
    }
    for (int k=0; k<FM_LR_TAPS; ++k) s->taps[k] /= sum;
}
int fm_lr_demod(fm_lr_dsp_t *s, fm_dsp_t *fm, const uint8_t *iq,
                size_t bytes, float *out, int capacity)
{
    int count = 0; float peak = 0;
    for (size_t b=0; b+3<bytes; b+=4) {
        float re = (int16_t)((unsigned)iq[b] | ((unsigned)iq[b+1]<<8));
        float im = (int16_t)((unsigned)iq[b+2] | ((unsigned)iq[b+3]<<8));
        float level = fmaxf(fabsf(re), fabsf(im))/32768.0f;
        if (level > peak) peak = level;
        push_pair(s, re, im);
        if (++s->decim < 4) continue;
        s->decim = 0; re = im = 0;
        unsigned j = s->pos;
        for (int k=0; k<FM_LR_TAPS; ++k) {
            if (!j) j = FM_LR_TAPS;
            --j; re += s->taps[k]*s->i[j]; im += s->taps[k]*s->q[j];
        }
        float d = s->have ? atan2f(im*s->prev_i-re*s->prev_q,
                                     re*s->prev_i+im*s->prev_q) : 0;
        /* Normalize angle to the existing 32 kHz discriminator scale. */
        d *= 31250.0f/32000.0f;
        s->prev_i = re; s->prev_q = im; s->have = true;
        /* Rational 128/125 interpolation with one input sample latency. */
        while (s->phase < 128) {
            float v = s->prev_d + (d-s->prev_d)*(float)s->phase/128.0f;
            fm->noise_hp += 0.25f*(v-fm->noise_hp);
            float hp = v-fm->noise_hp;
            /* 20 ms noise power averaging, independent of block boundaries. */
            s->noise_power += (hp*hp-s->noise_power)/640.0f;
            if (count < capacity) out[count++] = v;
            s->phase += 125;
        }
        s->phase -= 128; s->prev_d = d;
    }
    fm->iq_block_peak = peak;
    if (count) fm->demod_noise = sqrtf(s->noise_power)*s->noise_scale;
    return count;
}

int fm_lr_demod_env(fm_lr_dsp_t *s, fm_dsp_t *fm, const uint8_t *iq,
                    size_t bytes, float *out32k, int capacity)
{
    int count = 0; float peak = 0;
    for (size_t b=0; b+3<bytes; b+=4) {
        float re = (int16_t)((unsigned)iq[b] | ((unsigned)iq[b+1]<<8));
        float im = (int16_t)((unsigned)iq[b+2] | ((unsigned)iq[b+3]<<8));
        float level = fmaxf(fabsf(re), fabsf(im))/32768.0f;
        if (level > peak) peak = level;
        push_pair(s, re, im);
        if (++s->decim < 4) continue;
        s->decim = 0; re = im = 0;
        unsigned j = s->pos;
        for (int k=0; k<FM_LR_TAPS; ++k) {
            if (!j) j = FM_LR_TAPS;
            --j; re += s->taps[k]*s->i[j]; im += s->taps[k]*s->q[j];
        }
        const float mag = sqrtf(re*re + im*im);
        if (s->am_dc <= 0.0f) s->am_dc = mag;
        s->am_dc += 0.002f*(mag - s->am_dc);
        const float d = (mag - s->am_dc)/(s->am_dc + 1e-6f);
        while (s->phase < 128) {
            if (count < capacity)
                out32k[count++] = s->prev_d + (d - s->prev_d)*(float)s->phase/128.0f;
            s->phase += 125;
        }
        s->phase -= 128; s->prev_d = d;
    }
    fm->iq_block_peak = peak;
    fm->iq_peak = peak;
    return count;
}

int fm_lr_demod_am(fm_lr_dsp_t *s, fm_dsp_t *fm, const uint8_t *iq,
                   size_t bytes, int16_t *pcm16k, int capacity)
{
    int count = 0; float peak = 0;
    for (size_t b=0; b+3<bytes; b+=4) {
        float re = (int16_t)((unsigned)iq[b] | ((unsigned)iq[b+1]<<8));
        float im = (int16_t)((unsigned)iq[b+2] | ((unsigned)iq[b+3]<<8));
        float level = fmaxf(fabsf(re), fabsf(im))/32768.0f;
        if (level > peak) peak = level;
        push_pair(s, re, im);
        if (++s->decim < 4) continue;
        s->decim = 0; re = im = 0;
        unsigned j = s->pos;
        for (int k=0; k<FM_LR_TAPS; ++k) {
            if (!j) j = FM_LR_TAPS;
            --j; re += s->taps[k]*s->i[j]; im += s->taps[k]*s->q[j];
        }
        /* Envelope over the carrier level: 100% modulation reads about 10000. */
        const float mag = sqrtf(re*re + im*im);
        if (s->am_dc <= 0.0f) s->am_dc = mag;
        s->am_dc += 0.002f*(mag - s->am_dc);
        const float d = (mag - s->am_dc) * (10000.0f/(s->am_dc*32768.0f + 1.0f)) * 32768.0f;
        /* 31.25k -> 32k as in fm_lr_demod, then pairs to 16k. */
        while (s->phase < 128) {
            const float v = s->prev_d + (d - s->prev_d)*(float)s->phase/128.0f;
            s->am_acc += v;
            if (++s->am_n == 2) {
                float x = s->am_acc*0.5f;
                s->am_acc = 0; s->am_n = 0;
                if (x > 32767) x = 32767;
                if (x < -32768) x = -32768;
                if (count < capacity) pcm16k[count++] = (int16_t)x;
            }
            s->phase += 125;
        }
        s->phase -= 128; s->prev_d = d;
    }
    fm->iq_block_peak = peak;
    fm->iq_peak = peak;
    return count;
}
