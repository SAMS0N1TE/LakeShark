/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. Not librtlsdr - see UPSTREAM.md in this
   directory for which files here are third-party and which are ours. */
#ifndef LS_RADIO_ENDPOINT_H
#define LS_RADIO_ENDPOINT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LS_RADIO_ENDPOINT_ID_MAX       48
#define LS_RADIO_ENDPOINT_NAME_MAX     64
#define LS_RADIO_OWNER_MAX             32
#define LS_RADIO_MAX_FREQUENCY_RANGES   4
#define LS_RADIO_MAX_RATE_RANGES        4

/* Stable service identity used by explicit RTL diagnostics and recovery. */
#define LS_RADIO_ENDPOINT_RTL_USB "rtl-sdr.usb"
#define LS_RADIO_ENDPOINT_HACKRF_USB "hackrf.usb"

typedef struct ls_radio_session ls_radio_session_t;

typedef enum {
    LS_RADIO_ENDPOINT_ATTACHED = 0,
    LS_RADIO_ENDPOINT_DETACHED,
} ls_radio_endpoint_event_kind_t;

typedef struct {
    ls_radio_endpoint_event_kind_t kind;
    char endpoint_id[LS_RADIO_ENDPOINT_ID_MAX];
    uint32_t capabilities;
} ls_radio_endpoint_event_t;

typedef void (*ls_radio_endpoint_event_fn)(
    const ls_radio_endpoint_event_t *event, void *user);

typedef enum {
    LS_RADIO_OK               = 0,
    LS_RADIO_ERR_INVALID      = -1,
    LS_RADIO_ERR_UNAVAILABLE  = -2,
    LS_RADIO_ERR_BUSY         = -3,
    LS_RADIO_ERR_DISCONNECTED = -4,
    LS_RADIO_ERR_UNSUPPORTED  = -5,
    LS_RADIO_ERR_IO           = -6,
    LS_RADIO_ERR_TIMEOUT      = -7,
    LS_RADIO_ERR_STOPPED      = -8,
    LS_RADIO_ERR_NO_MEMORY    = -9,
    LS_RADIO_ERR_EXISTS       = -10,
} ls_radio_err_t;

typedef enum {
    LS_RADIO_RX_IQ_U8  = 1u << 0,
    LS_RADIO_RX_PACKET = 1u << 1,
    LS_RADIO_TX_PACKET = 1u << 2,
} ls_radio_cap_t;

typedef enum {
    LS_RADIO_IQ_FORMAT_NONE           = 0,
    LS_RADIO_IQ_FORMAT_U8_INTERLEAVED = 1u << 0,
} ls_radio_iq_format_t;

typedef enum {
    LS_RADIO_PACKET_FORMAT_NONE = 0,
    LS_RADIO_PACKET_FORMAT_RAW  = 1u << 0,
    LS_RADIO_PACKET_FORMAT_FSK  = 1u << 1,
    LS_RADIO_PACKET_FORMAT_LORA = 1u << 2,
} ls_radio_packet_format_t;

typedef enum {
    LS_RADIO_GAIN_AUTO = 0,
    LS_RADIO_GAIN_MANUAL,
} ls_radio_gain_mode_t;

typedef enum {
    LS_RADIO_DUPLEX_UNSPECIFIED = 0,
    LS_RADIO_DUPLEX_HALF,
    LS_RADIO_DUPLEX_FULL,
} ls_radio_duplex_t;

typedef struct {
    uint64_t min_hz;
    uint64_t max_hz;
} ls_radio_range_t;

typedef struct {
    uint32_t required_caps;
    uint64_t min_hz;
    uint64_t max_hz;
    uint32_t sample_rate_hz;
    uint32_t iq_format;
    uint32_t packet_format;
    const char *preferred_endpoint_id;
} ls_radio_requirements_t;

typedef struct {
    uint64_t center_hz;
    uint32_t sample_rate_hz;
    uint32_t bandwidth_hz;
    ls_radio_gain_mode_t gain_mode;
    int gain_tenths_db;
} ls_radio_iq_config_t;

#define LS_RADIO_PACKET_SYNC_MAX 8

typedef struct {
    uint64_t center_hz;
    uint32_t bandwidth_hz;
    uint32_t bitrate;
    uint32_t deviation_hz;
    uint32_t format;
    uint8_t sync_word[LS_RADIO_PACKET_SYNC_MAX];
    size_t sync_word_bytes;
    size_t max_payload_bytes;
} ls_radio_packet_config_t;

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t bytes;
    int rssi_tenths_dbm;
    int snr_tenths_db;
    uint64_t timestamp_us;
    bool crc_ok;
} ls_radio_packet_t;

/* Driver callbacks return ls_radio_err_t values. cancel_read must be safe to
 * call concurrently with read and must make a blocked read return promptly. */
typedef struct {
    ls_radio_err_t (*iq_configure)(void *driver_ctx,
                                   const ls_radio_iq_config_t *requested,
                                   ls_radio_iq_config_t *actual);
    ls_radio_err_t (*iq_set_gain)(void *driver_ctx,
                                  ls_radio_gain_mode_t mode,
                                  int tenths_db,
                                  int *actual_tenths_db);
    ls_radio_err_t (*iq_start)(void *driver_ctx);
    ls_radio_err_t (*iq_read)(void *driver_ctx, void *dst, size_t bytes,
                              uint32_t timeout_ms, size_t *read_bytes);
    ls_radio_err_t (*iq_retune)(void *driver_ctx, uint64_t center_hz,
                                bool fast, uint64_t *actual_hz);
    ls_radio_err_t (*iq_stop)(void *driver_ctx);
    ls_radio_err_t (*packet_rx_configure)(
        void *driver_ctx, const ls_radio_packet_config_t *requested,
        ls_radio_packet_config_t *actual);
    ls_radio_err_t (*packet_rx_start)(void *driver_ctx);
    ls_radio_err_t (*packet_rx_read)(void *driver_ctx,
                                     ls_radio_packet_t *packet,
                                     uint32_t timeout_ms);
    ls_radio_err_t (*packet_rx_stop)(void *driver_ctx);
    void (*cancel_read)(void *driver_ctx);
    /* Recover is transport-specific, but invocation and accounting are keyed
     * by the endpoint service. It is only called while the endpoint is idle. */
    ls_radio_err_t (*recover)(void *driver_ctx);
} ls_radio_driver_ops_t;

typedef struct {
    const char *endpoint_id;
    const char *name;
    uint32_t capabilities;
    ls_radio_duplex_t duplex;
    uint32_t iq_formats;
    uint32_t packet_formats;
    const ls_radio_range_t *frequency_ranges;
    size_t frequency_range_count;
    const ls_radio_range_t *sample_rate_ranges;
    size_t sample_rate_range_count;
    const ls_radio_driver_ops_t *ops;
    void *driver_ctx;
} ls_radio_endpoint_t;

typedef struct {
    char endpoint_id[LS_RADIO_ENDPOINT_ID_MAX];
    char name[LS_RADIO_ENDPOINT_NAME_MAX];
    uint32_t capabilities;
    ls_radio_duplex_t duplex;
    uint32_t iq_formats;
    uint32_t packet_formats;
    ls_radio_range_t frequency_ranges[LS_RADIO_MAX_FREQUENCY_RANGES];
    size_t frequency_range_count;
    ls_radio_range_t sample_rate_ranges[LS_RADIO_MAX_RATE_RANGES];
    size_t sample_rate_range_count;
    bool present;
    bool leased;
    bool streaming;
    char owner[LS_RADIO_OWNER_MAX];
    bool configured;
    ls_radio_iq_config_t actual_iq;
    ls_radio_packet_config_t actual_packet;
    uint64_t bytes_read;
    uint64_t packets_read;
    ls_radio_err_t last_error;
} ls_radio_endpoint_info_t;

ls_radio_err_t ls_radio_endpoint_register(const ls_radio_endpoint_t *endpoint);
ls_radio_err_t ls_radio_endpoint_unregister(const char *endpoint_id);

size_t ls_radio_endpoint_count(void);
ls_radio_err_t ls_radio_endpoint_info(size_t index,
                                      ls_radio_endpoint_info_t *out);
ls_radio_err_t ls_radio_endpoint_get(const char *endpoint_id,
                                     ls_radio_endpoint_info_t *out);
bool ls_radio_endpoint_available(
    const ls_radio_requirements_t *requirements);
int ls_radio_endpoint_subscribe(ls_radio_endpoint_event_fn callback,
                                void *user);
ls_radio_err_t ls_radio_endpoint_recover(const char *endpoint_id);

ls_radio_err_t ls_radio_acquire(const char *owner,
                                const ls_radio_requirements_t *requirements,
                                ls_radio_session_t **out);
void ls_radio_release(ls_radio_session_t *session);

ls_radio_err_t ls_radio_iq_configure(ls_radio_session_t *session,
                                     const ls_radio_iq_config_t *requested,
                                     ls_radio_iq_config_t *actual);
ls_radio_err_t ls_radio_iq_set_gain(ls_radio_session_t *session,
                                    ls_radio_gain_mode_t mode,
                                    int tenths_db,
                                    int *actual_tenths_db);
ls_radio_err_t ls_radio_iq_start(ls_radio_session_t *session);
ls_radio_err_t ls_radio_iq_read(ls_radio_session_t *session, void *dst,
                                size_t bytes, uint32_t timeout_ms,
                                size_t *read_bytes);
ls_radio_err_t ls_radio_iq_retune(ls_radio_session_t *session,
                                  uint64_t center_hz, bool fast,
                                  uint64_t *actual_hz);
ls_radio_err_t ls_radio_iq_stop(ls_radio_session_t *session);

ls_radio_err_t ls_radio_packet_rx_configure(
    ls_radio_session_t *session,
    const ls_radio_packet_config_t *requested,
    ls_radio_packet_config_t *actual);
ls_radio_err_t ls_radio_packet_rx_start(ls_radio_session_t *session);
ls_radio_err_t ls_radio_packet_rx_read(ls_radio_session_t *session,
                                       ls_radio_packet_t *packet,
                                       uint32_t timeout_ms);
ls_radio_err_t ls_radio_packet_rx_stop(ls_radio_session_t *session);

const char *ls_radio_err_name(ls_radio_err_t error);

#ifdef __cplusplus
}
#endif

#endif
