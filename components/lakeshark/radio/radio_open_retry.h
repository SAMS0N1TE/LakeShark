/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. Not librtlsdr - see UPSTREAM.md in this
   directory for which files here are third-party and which are ours. */
#ifndef RADIO_OPEN_RETRY_H
#define RADIO_OPEN_RETRY_H

#include <stdbool.h>
#include <stdint.h>

#include "radio_endpoint.h"

#ifdef __cplusplus
extern "C" {
#endif

/* How many radio opens in a row may fail with io before an app stops
   asking. radio_open_retry.c says why the limit is lifted by an endpoint
   change and not by re-entering the app. */
#define RADIO_OPEN_IO_ATTEMPTS 5u

typedef struct {
    unsigned io_failures;
    bool holding;
    uint32_t hold_generation;
} radio_open_retry_t;

typedef enum {
    RADIO_OPEN_RETRY_AGAIN = 0, /* pause as usual, then try again */
    RADIO_OPEN_RETRY_HOLD,      /* this failure reached the limit: say so */
} radio_open_retry_verdict_t;

void radio_open_retry_reset(radio_open_retry_t *retry);

/* generation is ls_radio_endpoint_generation() read BEFORE the attempt, so an
   attach that lands while the attempt was failing still releases the hold
   instead of being taken for the state that failed. */
radio_open_retry_verdict_t radio_open_retry_failed(radio_open_retry_t *retry,
                                                   ls_radio_err_t error,
                                                   uint32_t generation);
void radio_open_retry_succeeded(radio_open_retry_t *retry);

/* False while holding and nothing has attached or detached since the hold
   began. A change releases the hold and starts the count again. */
bool radio_open_retry_may_try(radio_open_retry_t *retry, uint32_t generation);

#ifdef __cplusplus
}
#endif

#endif
