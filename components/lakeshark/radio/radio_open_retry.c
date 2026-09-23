/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. Not librtlsdr - see UPSTREAM.md in this
   directory for which files here are third-party and which are ours. */
#include "radio_open_retry.h"

#include <string.h>

#include "radio_health.h"

/* A dongle that answers no control request made FM a log flood.
   Observed on the T-Display-P4, 2026-09-11: after the dongle's seventh
   attach every register access failed, and FM retried its open every
   ~150 ms for as long as it was left, each attempt logging
   "E fm: radio open failed: io". Leaving and re-entering FM changed nothing,
   because nothing about the dongle had changed.

   Bounded the way P25 bounds its no-memory opens: count the one
   kind of failure that repeating cannot fix, and stop at a limit. What
   differs is what lifts it. P25's no-memory case is cured by other apps
   letting go of memory, so re-entering P25 is the retry. An io failure is
   cured by the device changing - a replug, a root-port reset, a
   different receiver - so the hold lifts on the next endpoint attach or
   detach, and a recovered dongle is picked up with nobody re-entering the
   app. A terminal stop like P25's would strand FM behind the very reset
   that fixed the dongle.

   The limit must not cut in before the health watchdog has seen enough to
   act: every failed open adds at least one control io error on the
   endpoint, and the watchdog wants RADIO_HEALTH_CONTROL_IO_FAULTS of them
   in a row before it resets anything. */
_Static_assert(RADIO_OPEN_IO_ATTEMPTS >= RADIO_HEALTH_CONTROL_IO_FAULTS,
               "an app that stops retrying first starves the health watchdog");

void radio_open_retry_reset(radio_open_retry_t *retry)
{
    if (retry) memset(retry, 0, sizeof(*retry));
}

radio_open_retry_verdict_t radio_open_retry_failed(radio_open_retry_t *retry,
                                                   ls_radio_err_t error,
                                                   uint32_t generation)
{
    if (!retry || retry->holding) return RADIO_OPEN_RETRY_AGAIN;
    if (error != LS_RADIO_ERR_IO) {
        retry->io_failures = 0;
        return RADIO_OPEN_RETRY_AGAIN;
    }
    if (++retry->io_failures < RADIO_OPEN_IO_ATTEMPTS)
        return RADIO_OPEN_RETRY_AGAIN;
    retry->holding = true;
    retry->hold_generation = generation;
    return RADIO_OPEN_RETRY_HOLD;
}

void radio_open_retry_succeeded(radio_open_retry_t *retry)
{
    radio_open_retry_reset(retry);
}

bool radio_open_retry_may_try(radio_open_retry_t *retry, uint32_t generation)
{
    if (!retry || !retry->holding) return true;
    if (generation == retry->hold_generation) return false;
    retry->holding = false;
    retry->io_failures = 0;
    return true;
}
