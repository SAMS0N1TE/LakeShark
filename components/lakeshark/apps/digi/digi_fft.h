/* Mixed-radix complex FFT for the digital-mode decoders.

   FT8 and FT4 want transform sizes tied to their symbol lengths at 12 kHz
   (3840 = 2^8 x 15, 1152 = 2^7 x 9), so this is a plain Cooley-Tukey
   decimation in time over factors 4, 2, 3, 5 and anything else, written from
   the textbook recurrence. Tables live in PSRAM (digi_alloc). Forward only,
   unscaled. */
#ifndef DIGI_FFT_H
#define DIGI_FFT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

/* Big working buffers go to PSRAM on the board; internal RAM is the DMA pool. */
static inline void *digi_alloc(size_t n)
{
#ifdef ESP_PLATFORM
    void *p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : NULL;
#else
    return calloc(1, n);
#endif
}
static inline void digi_free(void *p) { free(p); }

typedef struct { float re, im; } digi_cpx;

#define DIGI_FFT_MAX_FACTORS 24
typedef struct {
    int n;
    int fac[2 * DIGI_FFT_MAX_FACTORS];  /* (radix, remaining length) pairs */
    digi_cpx *tw;                       /* e^(-2 pi i k / n), k < n         */
} digi_fft_t;

bool digi_fft_init(digi_fft_t *f, int n);
void digi_fft_free(digi_fft_t *f);
/* out[k] = sum in[j] e^(-2 pi i jk/n). in and out must not overlap. */
void digi_fft_run(const digi_fft_t *f, const digi_cpx *in, digi_cpx *out);

#endif
