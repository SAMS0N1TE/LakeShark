#ifndef LS_SPECTRUM_H
#define LS_SPECTRUM_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**/
/* A POWER SPECTRUM OF THE IQ WE ALREADY HAVE. */

#define SPEC_FFT_N   512

void spectrum_init(void);

/* Fold one interleaved u8 IQ buffer into the running average. `len` is BYTES
   (two per complex sample). Safe to call from the rx task; it does no
   allocation and touches no LVGL. */
void spectrum_accum(const uint8_t *iq, int len);

bool spectrum_read_db(float *out, int n);

/* Drop the accumulator - call after retuning so bins from the old centre
   frequency cannot bleed into the new one. */
void spectrum_reset(void);

/* How many buffers are in the current average. */
int  spectrum_navg(void);

#ifdef __cplusplus
}
#endif

#endif
