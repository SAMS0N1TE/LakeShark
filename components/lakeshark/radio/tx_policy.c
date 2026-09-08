/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. Not librtlsdr - see UPSTREAM.md in this
   directory for which files here are third-party and which are ours. */
#include "tx_policy_private.h"

#include <limits.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TX_LEDGER_MAX 16

typedef struct {
    bool used;
    bool pending;
    ls_radio_tx_reservation_t reservation;
    ls_radio_tx_audit_t audit;
} ledger_entry_t;

static ls_radio_tx_policy_t s_policy;
static ls_radio_tx_band_policy_t s_bands[LS_RADIO_TX_POLICY_BANDS_MAX];
static bool s_configured;
static ledger_entry_t s_ledger[TX_LEDGER_MAX];
static uint32_t s_next_reservation = 1;
static SemaphoreHandle_t s_lock;
static volatile int s_lock_init;

static bool ensure_lock(void)
{
    if (__atomic_load_n(&s_lock_init, __ATOMIC_ACQUIRE) != 2) {
        int expected = 0;
        if (__atomic_compare_exchange_n(&s_lock_init, &expected, 1, false,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            s_lock = xSemaphoreCreateMutex();
            __atomic_store_n(&s_lock_init, s_lock ? 2 : 0,
                             __ATOMIC_RELEASE);
        } else {
            while (__atomic_load_n(&s_lock_init, __ATOMIC_ACQUIRE) == 1)
                vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    return s_lock != NULL;
}

static bool valid_band(const ls_radio_tx_band_policy_t *band)
{
    return band && band->band_id != 0 && band->min_hz <= band->max_hz &&
           band->modulation_mask != 0 &&
           band->min_bandwidth_hz != 0 &&
           band->min_bandwidth_hz <= band->max_bandwidth_hz &&
           band->max_time_on_air_us != 0 && band->max_dwell_us != 0 &&
           band->duty_window_us != 0 && band->duty_airtime_us != 0 &&
           band->duty_airtime_us <= band->duty_window_us;
}

ls_radio_tx_err_t ls_radio_tx_policy_install(
    const ls_radio_tx_policy_t *policy)
{
    if (!policy || policy->profile_id == 0 ||
        policy->regulatory_domain_id == 0 || !policy->bands ||
        policy->band_count == 0 ||
        policy->band_count > LS_RADIO_TX_POLICY_BANDS_MAX || !ensure_lock())
        return LS_RADIO_TX_ERR_INVALID;
    for (size_t i = 0; i < policy->band_count; ++i)
        if (!valid_band(&policy->bands[i]))
            return LS_RADIO_TX_ERR_INVALID;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_bands, policy->bands,
           policy->band_count * sizeof(s_bands[0]));
    s_policy = *policy;
    s_policy.bands = s_bands;
    s_configured = true;
    xSemaphoreGive(s_lock);
    return LS_RADIO_TX_OK;
}

void ls_radio_tx_policy_clear(void)
{
    if (!ensure_lock()) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s_policy, 0, sizeof(s_policy));
    memset(s_bands, 0, sizeof(s_bands));
    memset(s_ledger, 0, sizeof(s_ledger));
    s_configured = false;
    xSemaphoreGive(s_lock);
}

bool ls_radio_tx_policy_is_configured(void)
{
    if (!ensure_lock()) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool configured = s_configured;
    xSemaphoreGive(s_lock);
    return configured;
}

static const ls_radio_tx_band_policy_t *find_band_locked(uint32_t frequency_hz)
{
    for (size_t i = 0; i < s_policy.band_count; ++i)
        if (frequency_hz >= s_bands[i].min_hz &&
            frequency_hz <= s_bands[i].max_hz)
            return &s_bands[i];
    return NULL;
}

static bool in_window(uint64_t timestamp_us, uint64_t now_us,
                      uint32_t window_us)
{
    /* A monotonic-clock discontinuity is uncertainty, so retain the charge. */
    return now_us < timestamp_us || now_us - timestamp_us < window_us;
}

static uint64_t used_airtime_locked(const char *endpoint_id,
                                    uint32_t profile_id, uint32_t band_id,
                                    uint64_t now_us, uint32_t window_us)
{
    uint64_t used = 0;
    for (size_t i = 0; i < TX_LEDGER_MAX; ++i) {
        const ledger_entry_t *entry = &s_ledger[i];
        if (entry->used && entry->audit.profile_id == profile_id &&
            entry->audit.band_id == band_id &&
            strcmp(entry->audit.endpoint_id, endpoint_id) == 0 &&
            in_window(entry->audit.timestamp_us, now_us, window_us))
            used += entry->audit.charged_airtime_us;
    }
    return used;
}

static ls_radio_tx_err_t check_locked(
    const ls_radio_tx_policy_check_t *check, uint64_t now_us,
    const ls_radio_tx_band_policy_t **matched, uint32_t *remaining_us)
{
    if (!s_configured) return LS_RADIO_TX_ERR_NO_POLICY;
    if (check->profile_id != s_policy.profile_id ||
        check->regulatory_domain_id != s_policy.regulatory_domain_id)
        return LS_RADIO_TX_ERR_DOMAIN;
    const ls_radio_tx_band_policy_t *band =
        find_band_locked(check->frequency_hz);
    if (!band) return LS_RADIO_TX_ERR_BAND;
    if (band->operator_license_required)
        return LS_RADIO_TX_ERR_DOMAIN;
    if (check->modulation == 0 ||
        (check->modulation & (check->modulation - 1u)) != 0 ||
        (band->modulation_mask & check->modulation) == 0)
        return LS_RADIO_TX_ERR_MODULATION;
    if (check->bandwidth_hz < band->min_bandwidth_hz ||
        check->bandwidth_hz > band->max_bandwidth_hz)
        return LS_RADIO_TX_ERR_BANDWIDTH;
    int64_t eirp_tenths_dbm = (int64_t)check->power_tenths_dbm +
                              check->antenna_gain_tenths_dbi;
    if (!check->antenna_gain_known ||
        eirp_tenths_dbm > band->max_power_tenths_dbm)
        return LS_RADIO_TX_ERR_POWER;
    if (check->airtime_us == 0 ||
        check->airtime_us > band->max_time_on_air_us)
        return LS_RADIO_TX_ERR_AIRTIME;
    if (check->airtime_us > band->max_dwell_us)
        return LS_RADIO_TX_ERR_DWELL;

    uint64_t used = used_airtime_locked(check->endpoint_id,
        check->profile_id, band->band_id, now_us, band->duty_window_us);
    if (used > band->duty_airtime_us ||
        check->airtime_us > band->duty_airtime_us - used)
        return LS_RADIO_TX_ERR_DUTY;
    if (remaining_us)
        *remaining_us = (uint32_t)(band->duty_airtime_us - used);
    if (matched) *matched = band;
    return LS_RADIO_TX_OK;
}

ls_radio_tx_err_t ls_radio_tx_policy_check(
    const ls_radio_tx_policy_check_t *check, uint64_t now_us,
    uint32_t *band_id, uint32_t *max_dwell_us,
    uint32_t *window_us, uint32_t *remaining_us)
{
    if (!check || !check->endpoint_id || !check->endpoint_id[0] ||
        !ensure_lock())
        return LS_RADIO_TX_ERR_INVALID;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const ls_radio_tx_band_policy_t *band = NULL;
    ls_radio_tx_err_t error = check_locked(check, now_us, &band, remaining_us);
    if (error == LS_RADIO_TX_OK) {
        if (band_id) *band_id = band->band_id;
        if (max_dwell_us) *max_dwell_us = band->max_dwell_us;
        if (window_us) *window_us = band->duty_window_us;
    }
    xSemaphoreGive(s_lock);
    return error;
}

static ledger_entry_t *alloc_entry_locked(uint64_t now_us,
                                          uint32_t window_us)
{
    ledger_entry_t *oldest = NULL;
    for (size_t i = 0; i < TX_LEDGER_MAX; ++i) {
        ledger_entry_t *entry = &s_ledger[i];
        if (!entry->used) return entry;
        if (!entry->pending && entry->audit.charged_airtime_us == 0)
            return entry;
        if (!entry->pending &&
            !in_window(entry->audit.timestamp_us, now_us, window_us) &&
            (!oldest || entry->audit.timestamp_us < oldest->audit.timestamp_us))
            oldest = entry;
    }
    return oldest;
}

ls_radio_tx_err_t ls_radio_tx_policy_reserve(
    const ls_radio_tx_policy_check_t *check, uint64_t now_us,
    ls_radio_tx_reservation_t *reservation)
{
    if (!check || !reservation || !ensure_lock())
        return LS_RADIO_TX_ERR_INVALID;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const ls_radio_tx_band_policy_t *band = NULL;
    ls_radio_tx_err_t error = check_locked(check, now_us, &band, NULL);
    ledger_entry_t *entry = NULL;
    if (error == LS_RADIO_TX_OK) {
        entry = alloc_entry_locked(now_us, band->duty_window_us);
        if (!entry) error = LS_RADIO_TX_ERR_DUTY;
    }
    if (error == LS_RADIO_TX_OK) {
        memset(entry, 0, sizeof(*entry));
        entry->used = true;
        entry->pending = true;
        entry->reservation = s_next_reservation++;
        if (entry->reservation == 0) entry->reservation = s_next_reservation++;
        strncpy(entry->audit.endpoint_id, check->endpoint_id,
                sizeof(entry->audit.endpoint_id) - 1);
        entry->audit.profile_id = check->profile_id;
        entry->audit.band_id = band->band_id;
        entry->audit.timestamp_us = now_us;
        entry->audit.planned_airtime_us = check->airtime_us;
        entry->audit.charged_airtime_us = check->airtime_us;
        *reservation = entry->reservation;
    }
    xSemaphoreGive(s_lock);
    return error;
}

static uint32_t elapsed_us(const ls_radio_tx_report_t *report,
                           uint32_t planned)
{
    if (!report || !report->rf_started) return 0;
    if (report->rf_end_us <= report->rf_start_us) return planned;
    uint64_t elapsed = report->rf_end_us - report->rf_start_us;
    return elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
}

void ls_radio_tx_policy_finish(ls_radio_tx_reservation_t reservation,
    const ls_radio_tx_report_t *report, ls_radio_err_t driver_result,
    uint64_t now_us)
{
    if (!reservation || !ensure_lock()) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < TX_LEDGER_MAX; ++i) {
        ledger_entry_t *entry = &s_ledger[i];
        if (!entry->used || entry->reservation != reservation) continue;
        entry->pending = false;
        entry->audit.driver_result = driver_result;
        entry->audit.rf_started = report && report->rf_started;
        entry->audit.completed = report && report->completed;
        entry->audit.actual_airtime_us =
            elapsed_us(report, entry->audit.planned_airtime_us);
        if (!entry->audit.rf_started) {
            entry->audit.charged_airtime_us = 0;
        } else if (driver_result != LS_RADIO_OK || !entry->audit.completed) {
            uint32_t actual = entry->audit.actual_airtime_us;
            entry->audit.charged_airtime_us =
                actual > entry->audit.planned_airtime_us
                    ? actual : entry->audit.planned_airtime_us;
        } else {
            entry->audit.charged_airtime_us =
                entry->audit.actual_airtime_us;
        }
        entry->audit.timestamp_us = now_us;
        break;
    }
    xSemaphoreGive(s_lock);
}

bool ls_radio_tx_policy_latest_audit(ls_radio_tx_audit_t *out)
{
    if (!out || !ensure_lock()) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const ledger_entry_t *latest = NULL;
    for (size_t i = 0; i < TX_LEDGER_MAX; ++i)
        if (s_ledger[i].used && !s_ledger[i].pending &&
            (!latest || s_ledger[i].audit.timestamp_us >=
                            latest->audit.timestamp_us))
            latest = &s_ledger[i];
    if (latest) *out = latest->audit;
    xSemaphoreGive(s_lock);
    return latest != NULL;
}
