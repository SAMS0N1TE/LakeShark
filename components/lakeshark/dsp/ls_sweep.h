#ifndef LS_SWEEP_H
#define LS_SWEEP_H

#include "ls_sweep_core.h"
#include "radio_endpoint.h"

#ifdef __cplusplus
extern "C" {
#endif

/*LS-820  Run a planned sweep against the radio.

   ls_sweep_core.h holds the arithmetic and is host-tested. This is the part
   that needs a dongle: acquire, retune, dwell, hand each spectrum to the fold,
   release. Nothing here decides where a bin goes.

   The radio is acquired for the duration and released before returning, so a
   sweep cannot run while an app owns the receiver - it returns
   LS_RADIO_ERR_BUSY instead of quietly stealing the dongle from a decoder. */

/* Called after each tune so a caller can show progress. Optional. */
typedef void (*ls_sweep_progress_fn)(uint32_t tune, uint32_t n_tunes,
                                     void *user);

/* How many 512-point transforms to average per tune.

   Cheap: one transform covers 512 complex samples, which at 2.4 MS/s is
   213 microseconds. The retune settle either side of it dominates by two
   orders of magnitude, so averaging several costs almost nothing and buys a
   steadier floor for the diff that Phase 1 builds on top. */
#define LS_SWEEP_DEFAULT_DWELL_FFTS 8

/* Run `plan`, writing plan->n_bins entries into `dbfs` (caller-allocated).
   `gain_tenths` of 0 asks for AGC. Returns LS_RADIO_OK, or the first radio
   error - the output is left partially filled in that case, with
   LS_SWEEP_NO_DATA wherever nothing was measured. */
/*LS-825  `fast` skips the driver's buffer reset on each retune, and MEASURED
   ON HARDWARE IT BUYS NOTHING. Kept, with the number, so nobody tries it again
   expecting a win.

   88-108 MHz, 800 bins, 24 tunes, LCD 4.3:

       fast = 0    3556 ms
       fast = 1    3506 ms

   1.4%, which is noise. The reason is in rtl_iq_retune: `fast` only skips
   rtlsdr_reset_buffer. The stream is stopped and started around every retune
   either way, and that is where the time goes - about 137 ms of the 146 ms
   per tune, against roughly 9 ms of actual dwell (4 ms settle plus three
   8 KB reads at 4.8 MB/s). Every tune tears down 16 x 16 KB of USB transfers
   and re-posts them; the "pump up" line in the log is that happening.

   So a genuinely fast sweep needs a retune that does not stop the stream at
   all - which our driver does not currently offer, and which is the one thing
   OrcSDR's clean-room driver does better ("apply PLL retune without tearing
   down the IQ stream"). That is a change to rtl_iq_retune, not a flag here.
   It matters at scale: 300 MHz - 1 GHz is about 814 tunes, so two minutes at
   146 ms versus eight seconds at 10 ms. */
ls_radio_err_t ls_sweep_run(const ls_sweep_plan_t *plan, int gain_tenths,
                            uint32_t dwell_ffts, bool fast_retune, int8_t *dbfs,
                            ls_sweep_progress_fn progress, void *user);

#ifdef __cplusplus
}
#endif

#endif
