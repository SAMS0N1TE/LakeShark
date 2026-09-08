#ifndef P25_TUNE_POLICY_H
#define P25_TUNE_POLICY_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A profile control survey and the shared carrier scanner both need the
   tuner to remain on their candidate for a complete dwell. */
bool p25_tune_policy_allows_grant(bool carrier_scan_active,
                                  bool control_survey_active);

#ifdef __cplusplus
}
#endif

#endif
