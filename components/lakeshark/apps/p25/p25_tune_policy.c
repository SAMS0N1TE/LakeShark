#include "p25_tune_policy.h"

bool p25_tune_policy_allows_grant(bool carrier_scan_active,
                                  bool control_survey_active)
{
    return !carrier_scan_active && !control_survey_active;
}
