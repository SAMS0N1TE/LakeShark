#ifndef LS_TX_CONFIRMATION_H
#define LS_TX_CONFIRMATION_H

#include <stdbool.h>

#include "tx_plan.h"

#ifdef __cplusplus
extern "C" {
#endif

/* present() is called only after every field in this exact snapshot is on the
 * local confirmation screen. accept() is called by that screen's event
 * handler with an identity obtained from ls_input_local_gesture(). */
ls_radio_tx_err_t ls_tx_confirmation_present(
    const ls_radio_tx_plan_t *displayed_plan);
ls_radio_tx_err_t ls_tx_confirmation_accept(
    const ls_radio_tx_plan_t *displayed_plan,
    ls_radio_tx_token_t *one_shot);
void ls_tx_confirmation_dismiss(void);
bool ls_tx_confirmation_pending(void);

#ifdef __cplusplus
}
#endif

#endif
