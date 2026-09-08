/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. Not librtlsdr - see UPSTREAM.md in this
   directory for which files here are third-party and which are ours. */
#ifndef LS_RADIO_TX_DRIVER_PRIVATE_H
#define LS_RADIO_TX_DRIVER_PRIVATE_H

/* Private driver-to-broker boundary. Applications include tx_plan.h only. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "radio_endpoint.h"
#include "tx_plan.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t frequency_hz;
    uint32_t modulation;
    uint32_t bandwidth_hz;
    uint32_t bitrate;
    uint32_t deviation_hz;
    int32_t power_tenths_dbm;
    const uint8_t *payload;
    size_t payload_bytes;
} ls_radio_tx_packet_t;

typedef struct {
    bool rf_started;
    bool completed;
    uint64_t rf_start_us;
    uint64_t rf_end_us;
} ls_radio_tx_report_t;

typedef struct {
    ls_radio_err_t (*estimate_airtime)(void *ctx,
        const ls_radio_tx_packet_t *packet, uint32_t *airtime_us);
    ls_radio_err_t (*transmit)(void *ctx,
        const ls_radio_tx_packet_t *packet, ls_radio_tx_report_t *report);
} ls_radio_tx_driver_ops_t;

typedef struct {
    const char *endpoint_id;
    const ls_radio_range_t *frequency_ranges;
    size_t frequency_range_count;
    uint32_t modulation_mask;
    uint32_t min_bandwidth_hz;
    uint32_t max_bandwidth_hz;
    int32_t min_power_tenths_dbm;
    int32_t max_power_tenths_dbm;
    size_t max_payload_bytes;
    uint32_t max_time_on_air_us;
    const ls_radio_tx_driver_ops_t *ops;
    void *driver_ctx;
} ls_radio_tx_driver_t;

ls_radio_tx_err_t ls_radio_tx_driver_register(
    const ls_radio_tx_driver_t *driver);
ls_radio_tx_err_t ls_radio_tx_driver_unregister(const char *endpoint_id);

#ifdef __cplusplus
}
#endif

#endif
