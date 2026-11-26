#ifndef LS_MIC_LEVEL_H
#define LS_MIC_LEVEL_H

/* Make-up gain and a peak limiter for the microphone, after the codec.

   Measured on the SX board (2026-11-25, REC-001.wav, quiet room): with the
   ES8311 analog PGA at its 30 dB top and the ADC gain scale at 30 dB, the
   recording sat at -63 dBFS RMS, -49 dBFS peak. Speech a hand's length away
   came out some 30 dB under a normal recording level.

   The codec gives the first 6 dB (ADC gain scale 30 -> 36 dB, applied inside
   the ES8311 before its 24-bit result is cut to 16). This adds a fixed
   +8 dB, the house cap for make-up gain, and a limiter holds every output
   peak at or under -1 dBFS so a clap or a shout is turned down instead of
   clipping. Attack is instant (the gain for a sample already accounts for
   that sample), release is about 150 ms, and the gain is shared by both
   slots so the stereo image does not shift. */

#include <stddef.h>
#include <stdint.h>

#define LS_MIC_MAKEUP_DB 8
/* 10^(8/20) and 10^(-1/20) of full scale. */
#define LS_MIC_MAKEUP 2.5118864f
#define LS_MIC_CEILING 29204.0f

typedef struct {
    float env;     /* peak envelope of the boosted signal, in LSB */
    float release; /* per-frame decay of env toward the signal */
} ls_mic_level_t;

static inline void ls_mic_level_reset(ls_mic_level_t *m, unsigned rate_hz)
{
    m->env = 0.f;
    /* exp(-1/(0.15 s * rate)), by its first-order series: close enough for
       a release time, and no libm call on the audio path. */
    m->release = 1.f - 1.f / (0.15f * (float)(rate_hz ? rate_hz : 44100u));
}

/* In place on interleaved 16-bit frames, `ch` samples per frame. Returns how
   many frames the limiter turned down (0 = linear +8 dB throughout). */
static inline size_t ls_mic_level_run(ls_mic_level_t *m, int16_t *pcm, size_t frames, unsigned ch)
{
    size_t limited = 0;
    float env = m->env;
    const float rel = m->release;
    for (size_t f = 0; f < frames; f++) {
        int16_t *s = pcm + f * ch;
        int32_t peak = 0;
        for (unsigned c = 0; c < ch; c++) {
            const int32_t a = s[c] < 0 ? -(int32_t)s[c] : s[c];
            if (a > peak) peak = a;
        }
        const float x = (float)peak * LS_MIC_MAKEUP;
        env = x > env ? x : env * rel;
        float g = LS_MIC_MAKEUP;
        if (env > LS_MIC_CEILING) { g *= LS_MIC_CEILING / env; limited++; }
        for (unsigned c = 0; c < ch; c++) {
            float y = (float)s[c] * g;
            /* env >= this frame's peak, so |y| <= ceiling already; the
               clamp only guards float rounding. */
            if (y > 32767.f) y = 32767.f;
            if (y < -32768.f) y = -32768.f;
            s[c] = (int16_t)(y < 0 ? y - 0.5f : y + 0.5f);
        }
    }
    m->env = env;
    return limited;
}

#endif
