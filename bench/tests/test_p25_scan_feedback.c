/* LS_TEST_SOURCES: ${FW}/components/lakeshark/core/scan_feedback.c ${APP}/p25/p25_tune_policy.c */
#include "ls_test.h"

#include "scan_feedback.h"
#include "p25_tune_policy.h"

/* scan_feedback only needs the stable error names; endpoint behavior is
   already covered by test_radio_endpoint and test_iq_app_control. */
const char *ls_radio_err_name(ls_radio_err_t error)
{
    switch (error) {
    case LS_RADIO_OK:              return "ok";
    case LS_RADIO_ERR_UNAVAILABLE: return "unavailable";
    case LS_RADIO_ERR_BUSY:        return "busy";
    case LS_RADIO_ERR_IO:          return "io";
    case LS_RADIO_ERR_TIMEOUT:     return "timeout";
    case LS_RADIO_ERR_STOPPED:     return "stopped";
    case LS_RADIO_ERR_NO_MEMORY:   return "no-memory";
    default:                       return "other";
    }
}

static scan_feedback_input_t running_input(void)
{
    scan_feedback_input_t input = {0};
    input.task_ready = true;
    input.enabled = true;
    input.foreground_supported = true;
    input.receiver_present = true;
    input.started = true;
    input.candidates = 3;
    input.scan_error = LS_RADIO_OK;
    input.receiver.receiver_streaming = true;
    input.receiver.receiver_error = LS_RADIO_OK;
    input.receiver.tune_state = LS_IQ_RESULT_EFFECTIVE;
    input.receiver.effective_center_known = true;
    input.receiver.effective_center_hz = 155250125ULL;
    input.receiver.requested_center_hz = 155262500ULL;
    return input;
}

LS_CASE(carrier_scan_exposes_every_operator_state)
{
    scan_feedback_input_t input = running_input();
    LS_EQ_INT(scan_feedback_classify(&input), SCAN_PHASE_SCANNING);

    input.holding = true;
    LS_EQ_INT(scan_feedback_classify(&input), SCAN_PHASE_HELD);

    input.holding = false;
    input.started = false;
    LS_EQ_INT(scan_feedback_classify(&input), SCAN_PHASE_STARTING);

    input.candidates = 0;
    LS_EQ_INT(scan_feedback_classify(&input), SCAN_PHASE_NO_CANDIDATES);

    input.candidates = 3;
    input.receiver.tune_state = LS_IQ_RESULT_FAILED;
    input.receiver.tune_error = LS_RADIO_ERR_IO;
    LS_EQ_INT(scan_feedback_classify(&input), SCAN_PHASE_ERROR);
}

LS_CASE(inert_causes_are_errors_instead_of_a_false_running_state)
{
    scan_feedback_input_t input = running_input();

    input.task_ready = false;
    LS_EQ_INT(scan_feedback_classify(&input), SCAN_PHASE_ERROR);

    input = running_input();
    input.parked = true;
    LS_EQ_INT(scan_feedback_classify(&input), SCAN_PHASE_ERROR);

    input = running_input();
    input.receiver_present = false;
    input.receiver.receiver_streaming = false;
    input.receiver.receiver_error = LS_RADIO_ERR_UNAVAILABLE;
    LS_EQ_INT(scan_feedback_classify(&input), SCAN_PHASE_ERROR);

    input = running_input();
    input.receiver.receiver_streaming = false;
    input.receiver.receiver_error = LS_RADIO_ERR_BUSY;
    LS_EQ_INT(scan_feedback_classify(&input), SCAN_PHASE_ERROR);

    input = running_input();
    input.receiver.receiver_streaming = false;
    input.receiver.receiver_error = LS_RADIO_ERR_NO_MEMORY;
    LS_EQ_INT(scan_feedback_classify(&input), SCAN_PHASE_ERROR);

    input = running_input();
    input.receiver.receiver_streaming = false;
    input.receiver.receiver_error = LS_RADIO_ERR_UNAVAILABLE;
    LS_EQ_INT(scan_feedback_classify(&input), SCAN_PHASE_STARTING);
}

LS_CASE(status_prints_endpoint_actual_not_the_optimistic_request)
{
    scan_feedback_input_t input = running_input();
    char text[160];
    scan_feedback_format(&input, "PRESET", "FIRE p=12",
                         text, sizeof(text));
    LS_CHECK(strstr(text, "CARRIER SCAN SCANNING") != NULL);
    LS_CHECK(strstr(text, "ACT 155.250125 MHz") != NULL);
    LS_CHECK(strstr(text, "155.262500") == NULL);

    input.receiver.effective_center_known = false;
    scan_feedback_format(&input, "BAND", "", text, sizeof(text));
    LS_CHECK(strstr(text, "ACT -- MHz") != NULL);
}

LS_CASE(carrier_scan_and_control_survey_hold_tune_ownership)
{
    LS_CHECK(p25_tune_policy_allows_grant(false, false));
    LS_CHECK(!p25_tune_policy_allows_grant(true, false));
    LS_CHECK(!p25_tune_policy_allows_grant(false, true));
    LS_CHECK(!p25_tune_policy_allows_grant(true, true));
}
