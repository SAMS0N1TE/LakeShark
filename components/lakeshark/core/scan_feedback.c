#include "scan_feedback.h"

#include <stdio.h>

scan_phase_t scan_feedback_classify(const scan_feedback_input_t *input)
{
    if (!input || !input->task_ready) return SCAN_PHASE_ERROR;
    if (!input->enabled) return SCAN_PHASE_OFF;
    if (input->switching) return SCAN_PHASE_STARTING;
    if (input->parked || !input->foreground_supported)
        return SCAN_PHASE_ERROR;
    if (input->candidates == 0) return SCAN_PHASE_NO_CANDIDATES;
    if (!input->receiver_present) return SCAN_PHASE_ERROR;
    if (input->scan_error != LS_RADIO_OK) return SCAN_PHASE_ERROR;
    if (!input->receiver.receiver_streaming) {
        if (input->receiver.receiver_error != LS_RADIO_ERR_UNAVAILABLE &&
            input->receiver.receiver_error != LS_RADIO_ERR_STOPPED)
            return SCAN_PHASE_ERROR;
        return SCAN_PHASE_STARTING;
    }
    if (input->receiver.tune_state == LS_IQ_RESULT_FAILED)
        return SCAN_PHASE_ERROR;
    if (!input->started) return SCAN_PHASE_STARTING;
    return input->holding ? SCAN_PHASE_HELD : SCAN_PHASE_SCANNING;
}

const char *scan_feedback_phase_name(scan_phase_t phase)
{
    switch (phase) {
    case SCAN_PHASE_OFF:           return "OFF";
    case SCAN_PHASE_STARTING:      return "STARTING";
    case SCAN_PHASE_SCANNING:      return "SCANNING";
    case SCAN_PHASE_HELD:          return "HELD";
    case SCAN_PHASE_NO_CANDIDATES: return "NO CANDIDATES";
    case SCAN_PHASE_ERROR:         return "ERROR";
    default:                       return "ERROR";
    }
}

static const char *error_detail(const scan_feedback_input_t *input)
{
    if (!input || !input->task_ready) return "scan task unavailable";
    if (input->parked) return "radio app parked";
    if (!input->foreground_supported) return "open P25 or FM";
    if (!input->receiver_present) return "no IQ receiver";
    if (input->scan_error != LS_RADIO_OK)
        return ls_radio_err_name(input->scan_error);
    if (!input->receiver.receiver_streaming &&
        input->receiver.receiver_error != LS_RADIO_OK)
        return ls_radio_err_name(input->receiver.receiver_error);
    if (input->receiver.tune_state == LS_IQ_RESULT_FAILED)
        return ls_radio_err_name(input->receiver.tune_error);
    return "unknown";
}

void scan_feedback_format(const scan_feedback_input_t *input,
                          const char *source, const char *detail,
                          char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    scan_phase_t phase = scan_feedback_classify(input);
    const char *why = detail ? detail : "";
    if (phase == SCAN_PHASE_ERROR) why = error_detail(input);

    char actual[32];
    if (input && input->receiver.effective_center_known) {
        uint64_t hz = input->receiver.effective_center_hz;
        snprintf(actual, sizeof(actual), "%llu.%06llu MHz",
                 (unsigned long long)(hz / 1000000ULL),
                 (unsigned long long)(hz % 1000000ULL));
    } else {
        snprintf(actual, sizeof(actual), "-- MHz");
    }

    snprintf(out, out_size, "CARRIER SCAN %s [%s] ACT %s%s%s",
             scan_feedback_phase_name(phase), source ? source : "?", actual,
             why[0] ? "  " : "", why);
}
