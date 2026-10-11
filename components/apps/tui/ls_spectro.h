/* Spectrogram arithmetic for the MUSIC waterfall: which FFT bins each pixel
   row reads, where a frequency lands on the axis, which ticks to label, and
   power to a byte. Pure functions, so the bench checks them directly. */
#ifndef LS_SPECTRO_H
#define LS_SPECTRO_H
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The lowest frequency a log axis shows. */
#define LS_SPECTRO_LOG_LO_HZ 40.0f

/* Stored power, in half-dB steps from LS_SPECTRO_CODE_FLOOR_DB (code 0,
   silence and below) to 0 dB (code 255). */
#define LS_SPECTRO_CODE_FLOOR_DB (-127.5f)

/* 10 * log10(p) to within a few hundredths of a dB, without the libm call:
   one of these runs for every bin of every line. */
static inline float ls_spectro_db(float p)
{
    if (!(p > 1e-14f)) return -140.0f;
    union { float f; uint32_t u; } v = { p };
    const int e = (int)((v.u >> 23) & 0xFF) - 127;
    v.u = (v.u & 0x007FFFFFu) | 0x3F800000u; /* mantissa in [1, 2) */
    const float m = v.f - 1.0f;
    /* log2(1 + m), max error under 0.00001 over [0, 1). */
    const float l2 = m * (1.4425449f + m * (-0.7181452f + m * (0.4575485f +
                     m * (-0.2779042f + m * (0.1217970f + m * -0.0258411f)))));
    return 3.0102999f * ((float)e + l2);
}

static inline uint8_t ls_spectro_code(float db)
{
    float c = (db - LS_SPECTRO_CODE_FLOOR_DB) * 2.0f + 0.5f;
    return (uint8_t)(c < 0.0f ? 0 : c > 255.0f ? 255 : (int)c);
}

static inline float ls_spectro_code_db(int code)
{
    return LS_SPECTRO_CODE_FLOOR_DB + code * 0.5f;
}

/* The frequency at a position 0..1 up the axis (0 the bottom edge). */
static inline float ls_spectro_axis_hz(float pos, bool log_axis, float top_hz)
{
    if (!log_axis) return pos * top_hz;
    return LS_SPECTRO_LOG_LO_HZ * powf(top_hz / LS_SPECTRO_LOG_LO_HZ, pos);
}

/* Where `hz` sits up the axis, 0..1, or outside that range when it is off
   the axis. */
static inline float ls_spectro_axis_pos(float hz, bool log_axis, float top_hz)
{
    if (!log_axis) return top_hz > 0 ? hz / top_hz : 0;
    if (hz <= 0) return -1;
    return logf(hz / LS_SPECTRO_LOG_LO_HZ) / logf(top_hz / LS_SPECTRO_LOG_LO_HZ);
}

/* Row 0 is the top pixel row. The pixel row that shows `hz`, or -1. */
static inline int ls_spectro_hz_row(float hz, int rows, bool log_axis, float top_hz)
{
    const float pos = ls_spectro_axis_pos(hz, log_axis, top_hz);
    if (rows < 1 || pos < 0 || pos > 1) return -1;
    int from_bottom = (int)(pos * rows);
    if (from_bottom >= rows) from_bottom = rows - 1;
    return rows - 1 - from_bottom;
}

/* The bins each pixel row reads, top row first: first[y]..last[y], both
   inclusive and never empty. A row wider than a bin takes every bin inside
   it (the waterfall shows their loudest); a row narrower than a bin takes
   the one nearest its centre, so a zoomed axis repeats a bin rather than
   invent detail. `fft_n` is the transform size, `rate` the sample rate and
   `top_hz` the top edge, at most rate / 2. */
static inline void ls_spectro_rows(int rows, int fft_n, unsigned rate, bool log_axis,
                                   float top_hz, int16_t *first, int16_t *last)
{
    if (rows < 1 || fft_n < 2 || !rate) return;
    const int top_bin = fft_n / 2 - 1;
    const float per_bin = (float)rate / (float)fft_n;
    for (int y = 0; y < rows; y++) {
        const int from_bottom = rows - 1 - y;
        const float lo = ls_spectro_axis_hz((float)from_bottom / rows, log_axis, top_hz);
        const float hi = ls_spectro_axis_hz((float)(from_bottom + 1) / rows, log_axis, top_hz);
        /* The bins whose centres fall in [lo, hi). */
        int a = (int)ceilf(lo / per_bin), b = (int)ceilf(hi / per_bin) - 1;
        if (b < a) a = b = (int)lroundf((lo + hi) * 0.5f / per_bin);
        if (a < 0) a = 0;
        if (b > top_bin) b = top_bin;
        if (a > top_bin) a = top_bin;
        if (b < a) b = a;
        first[y] = (int16_t)a;
        last[y] = (int16_t)b;
    }
}

/* Linear-axis tick spacing in Hz: the smallest of 1-2-5 steps (and 2.5)
   that keeps labelled ticks at least `min_px` apart on a `px`-tall axis. */
static inline float ls_spectro_tick_hz(float top_hz, int px, int min_px)
{
    static const float STEPS[] = {100, 200, 250, 500, 1000, 2000, 2500, 5000, 10000, 20000, 50000};
    for (unsigned i = 0; i < sizeof(STEPS) / sizeof(STEPS[0]); i++)
        if (px > 0 && STEPS[i] / top_hz * px >= (float)min_px) return STEPS[i];
    return top_hz;
}

/* An axis label in kHz: "0", "500", "2k", "2.5k", "22k". */
static inline void ls_spectro_khz(char *out, int n, float hz)
{
    if (n < 2) return;
    if (hz < 1000.0f) { snprintf(out, (size_t)n, "%.0f", (double)hz); return; }
    const float k = hz / 1000.0f;
    if (fabsf(k - roundf(k)) < 0.05f) snprintf(out, (size_t)n, "%.0fk", (double)k);
    else snprintf(out, (size_t)n, "%.1fk", (double)k);
}
#endif
