#ifndef LS_SCAN_FEEDBACK_H
#define LS_SCAN_FEEDBACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "iq_app_control.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SCAN_PHASE_OFF = 0,
    SCAN_PHASE_STARTING,
    SCAN_PHASE_SCANNING,
    SCAN_PHASE_HELD,
    SCAN_PHASE_NO_CANDIDATES,
    SCAN_PHASE_ERROR,
} scan_phase_t;

typedef struct {
    bool task_ready;
    bool enabled;
    bool switching;
    bool parked;
    bool foreground_supported;
    bool receiver_present;
    bool started;
    bool holding;
    int candidates;
    ls_radio_err_t scan_error;
    ls_iq_control_status_t receiver;
} scan_feedback_input_t;

scan_phase_t scan_feedback_classify(const scan_feedback_input_t *input);
const char *scan_feedback_phase_name(scan_phase_t phase);
void scan_feedback_format(const scan_feedback_input_t *input,
                          const char *source, const char *detail,
                          char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif
