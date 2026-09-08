#ifndef LS_RECEIVER_STATUS_H
#define LS_RECEIVER_STATUS_H

#include <stdbool.h>

#include "iq_app_control.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char connection[32];
    char frequency[80];
    char gain[64];
    bool available;
    bool tune_attention;
    bool gain_attention;
} ls_receiver_presentation_t;

/* Shared FM/REC rendering contract for receiver intent versus endpoint
   result. App-owned decoder detail stays outside this structure. */
void ls_receiver_present(const ls_iq_control_status_t *status,
                         ls_receiver_presentation_t *out);

#ifdef __cplusplus
}
#endif

#endif
