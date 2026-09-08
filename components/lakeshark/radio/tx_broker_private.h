/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. Not librtlsdr - see UPSTREAM.md in this
   directory for which files here are third-party and which are ours. */
#ifndef LS_RADIO_TX_BROKER_PRIVATE_H
#define LS_RADIO_TX_BROKER_PRIVATE_H

#include <stdint.h>

#include "tx_plan.h"

#ifdef __cplusplus
extern "C" {
#endif

/* This declaration is private to the shell confirmation controller. A radio
 * app can prepare/display/cancel a plan but has no public token minting API. */
ls_radio_tx_err_t ls_radio_tx_authorize_physical(
    const ls_radio_tx_plan_t *displayed_plan,
    uint64_t local_gesture_sequence,
    ls_radio_tx_token_t *one_shot);

void ls_radio_tx_broker_reset_for_test(void);

#ifdef __cplusplus
}
#endif

#endif
