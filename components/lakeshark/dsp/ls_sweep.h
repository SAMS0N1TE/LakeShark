#ifndef LS_SWEEP_H
#define LS_SWEEP_H

#include "ls_sweep_core.h"
#include "radio_endpoint.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Run a planned sweep against the radio. */

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
/* `fast` skips the driver's buffer reset on each retune, and MEASURED ON HARDWARE IT BUYS NOTHING. */

ls_radio_err_t ls_sweep_run(const ls_sweep_plan_t *plan, int gain_tenths,
                            uint32_t dwell_ffts, bool fast_retune, int8_t *dbfs,
                            ls_sweep_progress_fn progress, void *user);

#ifdef __cplusplus
}
#endif

#endif
