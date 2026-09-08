#include "shell/ls_tx_confirmation.h"

#include <string.h>

#include "ls_board.h"
#include "shell/ls_input_gesture.h"
#include "tx_broker_private.h"

static bool s_pending;
static bool s_tracking;
static ls_radio_tx_plan_t s_displayed;
#if LS_USE_DISPLAY
static uint64_t s_last_gesture;
#endif

ls_radio_tx_err_t ls_tx_confirmation_present(
    const ls_radio_tx_plan_t *displayed_plan)
{
#if LS_USE_DISPLAY
    if (!displayed_plan || displayed_plan->id == 0)
        return LS_RADIO_TX_ERR_INVALID;
    if (s_tracking) ls_radio_tx_cancel(s_displayed.id);
    s_displayed = *displayed_plan;
    s_pending = true;
    s_tracking = true;
    return LS_RADIO_TX_OK;
#else
    (void)displayed_plan;
    return LS_RADIO_TX_ERR_HEADLESS;
#endif
}

ls_radio_tx_err_t ls_tx_confirmation_accept(
    const ls_radio_tx_plan_t *displayed_plan,
    ls_radio_tx_token_t *one_shot)
{
#if LS_USE_DISPLAY
    ls_input_gesture_id_t gesture = {0};
    const bool have_local_gesture = ls_input_local_gesture(&gesture);
    if (!s_pending || !displayed_plan || !one_shot ||
        !have_local_gesture ||
        (gesture.source != LS_INPUT_GESTURE_LOCAL_TOUCH &&
         gesture.source != LS_INPUT_GESTURE_LOCAL_KEY) ||
        gesture.sequence == 0 || gesture.sequence <= s_last_gesture ||
        memcmp(displayed_plan, &s_displayed, sizeof(s_displayed)) != 0) {
        if (s_tracking) ls_radio_tx_cancel(s_displayed.id);
        s_pending = false;
        s_tracking = false;
        return LS_RADIO_TX_ERR_AUTH;
    }
    s_last_gesture = gesture.sequence;
    ls_radio_tx_err_t error = ls_radio_tx_authorize_physical(
        displayed_plan, gesture.sequence, one_shot);
    if (error != LS_RADIO_TX_OK) {
        ls_radio_tx_cancel(s_displayed.id);
        s_tracking = false;
    }
    s_pending = false;
    if (!s_tracking) memset(&s_displayed, 0, sizeof(s_displayed));
    return error;
#else
    (void)displayed_plan;
    (void)one_shot;
    return LS_RADIO_TX_ERR_HEADLESS;
#endif
}

void ls_tx_confirmation_dismiss(void)
{
    if (s_tracking) ls_radio_tx_cancel(s_displayed.id);
    s_pending = false;
    s_tracking = false;
    memset(&s_displayed, 0, sizeof(s_displayed));
}

bool ls_tx_confirmation_pending(void)
{
    return s_pending;
}
