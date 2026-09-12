/* LS_TEST_SOURCES: ${FW}/components/lakeshark/radio/radio_endpoint.c ${FW}/components/lakeshark/radio/tx_policy.c ${FW}/components/lakeshark/radio/tx_broker.c ${FW}/components/apps/shell/ls_tx_confirmation.c */
#include "ls_test.h"
#include "shell/ls_input_gesture.h"
#include "shell/ls_tx_confirmation.h"
#include "tx_broker_private.h"

LS_CASE(headless_build_has_no_physical_authorization_path)
{
    ls_radio_tx_plan_t plan = {.id = 1};
    ls_radio_tx_token_t token = {{0}};
    ls_input_gesture_id_t gesture = {
        .sequence = 1, .source = LS_INPUT_GESTURE_LOCAL_KEY};
    LS_EQ_INT(ls_tx_confirmation_present(&plan), LS_RADIO_TX_ERR_HEADLESS);
    LS_EQ_INT(ls_tx_confirmation_accept(&plan, &token),
              LS_RADIO_TX_ERR_HEADLESS);
    LS_EQ_INT(ls_radio_tx_authorize_physical(&plan, gesture.sequence, &token),
              LS_RADIO_TX_ERR_HEADLESS);
    LS_EQ_INT(ls_radio_tx_commit(plan.id, token), LS_RADIO_TX_ERR_AUTH);
}
