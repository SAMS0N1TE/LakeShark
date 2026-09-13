#pragma once
#include <stdint.h>

/* Synthesis phases are bounded before reaching this interpolation table. */
static inline float p25p2_phase(float x)
{
    const float tau = 6.283185307179586f;
    int32_t turns = (int32_t)(x * 0.159154943091895f);
    float phase = x - turns * tau;
    return phase < 0 ? phase + tau : phase;
}

static inline float p25p2_cos(float x)
{
    static const float table[513] = {
#include "fast_cos_table.inc"
    };
    float p = p25p2_phase(x) * 81.48733086305042f;
    unsigned i = (unsigned)p;
    if (i >= 512) return 1.0f;
    return table[i] + (p - i) * (table[i + 1] - table[i]);
}
