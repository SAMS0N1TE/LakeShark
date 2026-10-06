#ifndef LS_SWEEP_CORE_H
#define LS_SWEEP_CORE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Plan and stitch a wideband sweep. */

/* 300 MHz .. 1 GHz at 25 kHz bins is 28 000. Cap generously; the caller's
   allocation is what actually bounds this. */
#define LS_SWEEP_MAX_BINS 65536

/* Sentinel for "no tune covered this bin". Distinguishable from a real
   reading: -128 dBFS is below any noise floor an RTL can report. */
#define LS_SWEEP_NO_DATA  ((int8_t)-128)

#define LS_SWEEP_USABLE_PCT 75

/* How much spectrum around the tune centre to abandon, in Hz. */

#define LS_SWEEP_DC_GUARD_HZ 40000u

typedef struct {
    uint64_t start_hz;
    uint64_t stop_hz;
    uint32_t bin_hz;
    uint32_t n_bins;          /* output vector length */
    uint32_t sample_rate_hz;
    uint32_t half_span_hz;    /* trusted distance above centre */
    uint32_t strip_hz;        /* per-tune span actually folded in */
    uint32_t n_tunes;
    /* Overlapped plan (ls_sweep_plan_overlap). Both zero for the classic
       single-sideband plan, which is every caller but the search. */
    uint32_t hop_hz;          /* spacing of tune centres */
    uint32_t dc_guard_hz;     /* masked either side of each centre */
} ls_sweep_plan_t;

/* Work out the tune list and output geometry. False on nonsense: stop below
   start, a zero bin, a span that needs more than LS_SWEEP_MAX_BINS, or a bin
   finer than the FFT can resolve. */
bool ls_sweep_plan(uint64_t start_hz, uint64_t stop_hz, uint32_t bin_hz,
                   uint32_t sample_rate_hz, ls_sweep_plan_t *out);

/* A plan for repeated, artefact-averse searching. Both sidebands of every
   tune are used, but only inside +-usable_pct/2 of the sample rate (the
   outer part is analogue and decimation roll-off) and outside +-dc_guard_hz
   of the centre (the DC spike). Tunes are hop_hz apart, so every frequency
   is inside the good part of at least two of them, and each output bin is
   taken from the ONE tune that has it nearest its centre: the tune that
   owns it. A bin in the DC hole of its nearest tune is owned by the
   neighbour, which sees it a hop away from its own centre. False when the
   numbers cannot be made gap-free: the hop must leave the holes inside the
   neighbour's good part. At least two tunes are always planned. */
bool ls_sweep_plan_overlap(uint64_t start_hz, uint64_t stop_hz, uint32_t bin_hz,
                           uint32_t sample_rate_hz, uint32_t usable_pct,
                           uint32_t dc_guard_hz, uint32_t hop_hz,
                           ls_sweep_plan_t *out);

/* Which tune takes output bin `bin` in an overlapped plan. */
uint32_t ls_sweep_bin_owner(const ls_sweep_plan_t *plan, uint32_t bin);

/* Centre frequency for tune `i`, 0 <= i < n_tunes. */
uint64_t ls_sweep_tune_center(const ls_sweep_plan_t *plan, uint32_t i);

/* Frequency of one fftshifted FFT bin: index 0 is centre - rate/2 and index
   fft_n-1 is centre + rate/2, matching spectrum_read_db's output order. */
int64_t ls_sweep_bin_hz(const ls_sweep_plan_t *plan, uint64_t center_hz,
                        int fft_bin, int fft_n);

/* Which output bin an FFT bin belongs in, or -1 to discard it: at or below
   the tune's DC guard, above its strip, or outside the requested span. */
int ls_sweep_bin_index(const ls_sweep_plan_t *plan, uint64_t center_hz,
                       int fft_bin, int fft_n);

/* Set every bin to LS_SWEEP_NO_DATA. Call before the first fold. */
void ls_sweep_reset(const ls_sweep_plan_t *plan, int8_t *dbfs);

/* Fold one tune's dB spectrum into the output, peak-held.

   PEAK, not mean, for the same reason spectrum_read_db uses it: this is a
   signal finder, and averaging a narrow carrier against its silent neighbours
   is how you lose it. */
void ls_sweep_fold(const ls_sweep_plan_t *plan, uint64_t center_hz,
                   const float *db, int fft_n, int8_t *dbfs);

/* Centre frequency of output bin `i`. */
uint64_t ls_sweep_out_hz(const ls_sweep_plan_t *plan, uint32_t i);

/* How many bins never received a reading. Zero is the stitching invariant:
   a full sweep that leaves holes means the tune step and the usable window
   disagree, and every product built on this would inherit blind spots. */
uint32_t ls_sweep_gaps(const ls_sweep_plan_t *plan, const int8_t *dbfs);

#ifdef __cplusplus
}
#endif

#endif
