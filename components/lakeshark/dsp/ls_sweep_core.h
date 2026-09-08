#ifndef LS_SWEEP_CORE_H
#define LS_SWEEP_CORE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*LS-820  Plan and stitch a wideband sweep. No radio, no ESP-IDF.

   A sweep is: tune, take a spectrum, tune again, and glue the pieces into one
   power-versus-frequency vector. The radio part is three calls. The part that
   is actually easy to get wrong - and impossible to debug on hardware, because
   a wrong answer still looks like a plausible spectrum - is the arithmetic:
   which centre frequencies to visit, which FFT bins from each visit are
   trustworthy, and where each one lands in the output.

   So that arithmetic lives here, where bench/tests/test_ls_sweep.c can check
   it against known answers in a millisecond. ls_sweep.c owns the radio and
   calls into this. Same split as ble_link_core.c.

   Everything downstream - RF diff, the sound trigger, direction finding -
   reads the vector this produces, so an error here is an error in all of
   them. */

/* 300 MHz .. 1 GHz at 25 kHz bins is 28 000. Cap generously; the caller's
   allocation is what actually bounds this. */
#define LS_SWEEP_MAX_BINS 65536

/* Sentinel for "no tune covered this bin". Distinguishable from a real
   reading: -128 dBFS is below any noise floor an RTL can report. */
#define LS_SWEEP_NO_DATA  ((int8_t)-128)

/* How much of each tune's sample rate to trust.

   An RTL-SDR does not deliver a flat passband across the full sample rate:
   the analogue filter rolls off towards the edges, so bins out there read low
   and a signal parked at a tile boundary would be reported quieter than the
   same signal well inside the next one. Keep the central 75% - so the trusted
   region reaches 37.5% of the rate either side of centre - and take the strip
   from that. A finder must not lie about level. */
#define LS_SWEEP_USABLE_PCT 75

/* How much spectrum around the tune centre to abandon, in Hz.

   The RTL puts a large DC offset spike at the tune centre. It is an artefact
   of the receiver, not a signal, and folding it in would deposit a fake
   carrier at every tune centre - an evenly spaced comb that reads as real
   signals and would have RF diff reporting the receiver to itself.

   Expressed in Hz rather than FFT bins so the plan does not need to know the
   transform size. */
#define LS_SWEEP_DC_GUARD_HZ 40000u

/* SINGLE SIDEBAND, and this is the whole reason the tiling looks odd.

   The obvious scheme centres each tile on its tune and uses both sidebands.
   It does not work, and the host tests caught it: the DC guard blanks a strip
   in the MIDDLE of every tile, where no neighbouring tune can reach it. A
   sweep built that way has a hole at every tune centre - measured at 6 bins
   per tune with 5 kHz bins, 66 holes across 88-108 MHz - and on hardware that
   is invisible, because the plot stays continuous and the missing signal
   simply never appears.

   So each tune contributes only the spectrum ABOVE its own DC spike:

       tune i centre ─┐
                      ▼
       ... ───────────╳────[═══ used ═══)──────── ...
                    DC guard   the strip

   No tile contains its own spike, and the strips abut. It costs about twice
   the retunes of the naive scheme; the naive scheme was wrong. */

typedef struct {
    uint64_t start_hz;
    uint64_t stop_hz;
    uint32_t bin_hz;
    uint32_t n_bins;          /* output vector length */
    uint32_t sample_rate_hz;
    uint32_t half_span_hz;    /* trusted distance above centre */
    uint32_t strip_hz;        /* per-tune span actually folded in */
    uint32_t n_tunes;
} ls_sweep_plan_t;

/* Work out the tune list and output geometry. False on nonsense: stop below
   start, a zero bin, a span that needs more than LS_SWEEP_MAX_BINS, or a bin
   finer than the FFT can resolve. */
bool ls_sweep_plan(uint64_t start_hz, uint64_t stop_hz, uint32_t bin_hz,
                   uint32_t sample_rate_hz, ls_sweep_plan_t *out);

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
