/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. Not librtlsdr - see UPSTREAM.md in this
   directory for which files here are third-party and which are ours. */
#ifndef LS_RADIO_TX_POLICY_PRIVATE_H
#define LS_RADIO_TX_POLICY_PRIVATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tx_driver_private.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LS_RADIO_TX_POLICY_BANDS_MAX 4

typedef struct {
    uint32_t band_id;
    uint32_t min_hz;
    uint32_t max_hz;
    uint32_t modulation_mask;
    uint32_t min_bandwidth_hz;
    uint32_t max_bandwidth_hz;
    int32_t max_power_tenths_dbm; /* EIRP ceiling after known antenna gain */
    uint32_t max_time_on_air_us;
    uint32_t max_dwell_us;
    uint32_t duty_window_us;
    uint32_t duty_airtime_us;
    bool operator_license_required;
} ls_radio_tx_band_policy_t;

typedef struct {
    uint32_t profile_id;
    uint32_t regulatory_domain_id;
    const ls_radio_tx_band_policy_t *bands;
    size_t band_count;
} ls_radio_tx_policy_t;

typedef struct {
    char endpoint_id[LS_RADIO_ENDPOINT_ID_MAX];
    uint32_t profile_id;
    uint32_t band_id;
    uint64_t timestamp_us;
    uint32_t planned_airtime_us;
    uint32_t actual_airtime_us;
    uint32_t charged_airtime_us;
    bool rf_started;
    bool completed;
    ls_radio_err_t driver_result;
} ls_radio_tx_audit_t;

/* No profile is installed by firmware today: that is the reviewed deny-all
 * default. A future region table may select only a reviewed profile ID. */
ls_radio_tx_err_t ls_radio_tx_policy_install(
    const ls_radio_tx_policy_t *policy);
void ls_radio_tx_policy_clear(void);
bool ls_radio_tx_policy_is_configured(void);
bool ls_radio_tx_policy_latest_audit(ls_radio_tx_audit_t *out);

typedef struct {
    uint32_t profile_id;
    uint32_t regulatory_domain_id;
    const char *endpoint_id;
    uint32_t frequency_hz;
    uint32_t modulation;
    uint32_t bandwidth_hz;
    int32_t power_tenths_dbm;
    int32_t antenna_gain_tenths_dbi;
    bool antenna_gain_known;
    uint32_t airtime_us;
} ls_radio_tx_policy_check_t;

typedef uint32_t ls_radio_tx_reservation_t;

ls_radio_tx_err_t ls_radio_tx_policy_check(
    const ls_radio_tx_policy_check_t *check, uint64_t now_us,
    uint32_t *band_id, uint32_t *max_dwell_us,
    uint32_t *window_us, uint32_t *remaining_us);
ls_radio_tx_err_t ls_radio_tx_policy_reserve(
    const ls_radio_tx_policy_check_t *check, uint64_t now_us,
    ls_radio_tx_reservation_t *reservation);
void ls_radio_tx_policy_finish(ls_radio_tx_reservation_t reservation,
    const ls_radio_tx_report_t *report, ls_radio_err_t driver_result,
    uint64_t now_us);

#ifdef __cplusplus
}
#endif

#endif
