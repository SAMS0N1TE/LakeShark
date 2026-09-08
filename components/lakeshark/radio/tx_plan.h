/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. Not librtlsdr - see UPSTREAM.md in this
   directory for which files here are third-party and which are ours. */
#ifndef LS_RADIO_TX_PLAN_H
#define LS_RADIO_TX_PLAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "radio_endpoint.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LS_RADIO_TX_PAYLOAD_MAX 255
#define LS_RADIO_TX_DIGEST_BYTES 32
#define LS_RADIO_TX_TOKEN_BYTES 32

typedef uint64_t ls_radio_tx_plan_id_t;

typedef enum {
    LS_RADIO_TX_MOD_FSK  = 1u << 0,
    LS_RADIO_TX_MOD_LORA = 1u << 1,
    LS_RADIO_TX_MOD_OOK  = 1u << 2,
} ls_radio_tx_modulation_t;

typedef enum {
    LS_RADIO_TX_OK                    = 0,
    LS_RADIO_TX_ERR_INVALID           = -100,
    LS_RADIO_TX_ERR_NO_POLICY         = -101,
    LS_RADIO_TX_ERR_ENDPOINT          = -102,
    LS_RADIO_TX_ERR_DOMAIN            = -103,
    LS_RADIO_TX_ERR_BAND              = -104,
    LS_RADIO_TX_ERR_MODULATION        = -105,
    LS_RADIO_TX_ERR_BANDWIDTH         = -106,
    LS_RADIO_TX_ERR_POWER             = -107,
    LS_RADIO_TX_ERR_AIRTIME           = -108,
    LS_RADIO_TX_ERR_DWELL             = -109,
    LS_RADIO_TX_ERR_DUTY              = -110,
    LS_RADIO_TX_ERR_AUTH              = -111,
    LS_RADIO_TX_ERR_EXPIRED           = -112,
    LS_RADIO_TX_ERR_CANCELLED         = -113,
    LS_RADIO_TX_ERR_DRIVER            = -114,
    LS_RADIO_TX_ERR_NO_SPACE          = -115,
    LS_RADIO_TX_ERR_HEADLESS          = -116,
} ls_radio_tx_err_t;

typedef struct {
    const char *endpoint_id;
    uint32_t policy_profile_id;
    uint32_t regulatory_domain_id;
    uint32_t frequency_hz;
    uint32_t modulation;
    uint32_t bandwidth_hz;
    uint32_t bitrate;
    uint32_t deviation_hz;
    int32_t power_tenths_dbm;
    int32_t antenna_gain_tenths_dbi;
    bool antenna_gain_known;
    const uint8_t *payload;
    size_t payload_bytes;
} ls_radio_tx_request_t;

/* A plan is a display snapshot. The broker owns a separate immutable copy;
 * callers identify that copy with id and cannot use edits to this structure
 * to alter what is eventually transmitted. */
typedef struct {
    ls_radio_tx_plan_id_t id;
    char endpoint_id[LS_RADIO_ENDPOINT_ID_MAX];
    uint32_t policy_profile_id;
    uint32_t regulatory_domain_id;
    uint32_t policy_band_id;
    uint32_t frequency_hz;
    uint32_t modulation;
    uint32_t bandwidth_hz;
    uint32_t bitrate;
    uint32_t deviation_hz;
    int32_t power_tenths_dbm;
    int32_t antenna_gain_tenths_dbi;
    bool antenna_gain_known;
    uint32_t time_on_air_us;
    uint32_t max_dwell_us;
    uint32_t duty_window_us;
    uint32_t duty_remaining_us;
    uint8_t payload[LS_RADIO_TX_PAYLOAD_MAX];
    size_t payload_bytes;
    uint8_t digest[LS_RADIO_TX_DIGEST_BYTES];
} ls_radio_tx_plan_t;

typedef struct {
    uint8_t bytes[LS_RADIO_TX_TOKEN_BYTES];
} ls_radio_tx_token_t;

ls_radio_tx_err_t ls_radio_tx_plan(const ls_radio_tx_request_t *request,
                                   ls_radio_tx_plan_t *out);
ls_radio_tx_err_t ls_radio_tx_commit(ls_radio_tx_plan_id_t plan,
                                     ls_radio_tx_token_t one_shot);
void ls_radio_tx_cancel(ls_radio_tx_plan_id_t plan);

const char *ls_radio_tx_err_name(ls_radio_tx_err_t error);

#ifdef __cplusplus
}
#endif

#endif
