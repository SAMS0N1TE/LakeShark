#ifndef LS_SPECTRUM_H
#define LS_SPECTRUM_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*LS-748*/
/* A POWER SPECTRUM OF THE IQ WE ALREADY HAVE.
   The band sweep tuned, dwelled, and reduced the ENTIRE passband to one RMS
   number - FM.iq_level - then stepped by scan_step_hz and did it again. At
   FM_RTL_RATE (256 kSPS) the receiver is showing +/-128 kHz on every buffer,
   so that threw away essentially all of it. LS-710 already noted the width of
   this passband as the reason nearby channels "read identically"; the same
   fact is what makes one tune worth hundreds of bins.

   512 bins across 256 kHz is 500 Hz resolution, from a SINGLE dwell. Sweeping
   151-152 MHz went from 81 tunes at 12.5 kHz resolution to about 5 tunes at
   500 Hz - fewer retunes AND finer, which is the opposite of the usual trade.

   DELIBERATELY NOT esp-dsp. That component is vendored in managed_components
   but nothing REQUIRES it, so the component manager deletes it on every build
   and the session close already tells people to `git checkout --
   managed_components` afterwards. Depending on it would make the spectrum
   vanish on a clean build. This is a plain radix-2 FFT we own; it is not the
   fastest possible, and it does not need to be - one 512-point transform per
   buffer is far cheaper than the FM demodulator already running beside it. */

#define SPEC_FFT_N   512

void spectrum_init(void);

/* Fold one interleaved u8 IQ buffer into the running average. `len` is BYTES
   (two per complex sample). Safe to call from the rx task; it does no
   allocation and touches no LVGL. */
void spectrum_accum(const uint8_t *iq, int len);

/* Copy the averaged spectrum out as dB (relative to full scale, negative),
   resampled to `n` output bins by taking the MAX of each group.
   MAX, NOT MEAN, on purpose: this is a signal FINDER. A narrow carrier that
   lands in one FFT bin must survive being binned down to a 460 px display, and
   averaging it against its silent neighbours is exactly how you lose it.
   Output is fftshifted - out[0] is centre-rate/2, out[n-1] is centre+rate/2.
   Returns false if nothing has been accumulated since the last read. */
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
