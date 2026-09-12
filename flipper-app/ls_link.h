#pragma once

#include <furi.h>
#include <furi_hal_serial.h>
#include <furi_hal_serial_control.h>

#define LS_LINK_BAUD_DEFAULT 115200u

#ifndef LS_LINK_PORT
#define LS_LINK_PORT FuriHalSerialIdUsart
#endif

#define LS_LINK_PROBE_MS 4000u

#define LS_LINK_TIMEOUT_MS 4000u

typedef enum {
    LsModeP25,
    LsModeFm,
    LsModeAdsb,
    LsModeRec,
    LsModeUnknown,
} LsMode;

typedef enum {
    LsRecIdle,
    LsRecArmed,
    LsRecCapturing,
    LsRecDone,
} LsRecPhase;

#define LS_REC_MAX_EDGES 4096
#define LS_REC_CHUNK 32

typedef enum {
    LsFmListen,
    LsFmScan,
    LsFmPocsag,
    LsFmWfm,
} LsFmMode;

#define LS_AC_MAX 16
#define LS_CALL_MAX 9

typedef struct {
    uint32_t icao;
    char call[LS_CALL_MAX];
    int32_t altitude;
    int32_t velocity;
    int32_t heading;
    int32_t vert_rate;
    int32_t age_ms;
    int32_t msg_count;
    /**/
    bool pos_valid;
    int32_t lat_e4;
    int32_t lon_e4;
    bool seen;
} LsAircraft;

typedef struct {

    LsMode mode;
    char mode_name[8];
    uint32_t freq_hz;
    int32_t gain_tenths;
    int32_t agc_on;
    int32_t volume;
    int32_t muted;
    int32_t rtl_ready;
    int32_t iq_level;
    int32_t read_errors;
    uint32_t iq_bytes_sec;

    uint32_t uptime_s;
    uint32_t free_internal;
    uint32_t free_dma;

    int32_t sdr_stall_s;
    char rtl_health[12];

    int32_t eq_preset;
    int32_t eq_hp_hz;
    int32_t eq_bass_db;
    int32_t eq_treb_db;
    int32_t eq_punch;
    int32_t eq_loud;
    int32_t eq_gr_db10;

    int32_t demod_mode;
    char demod_name[16];
    int32_t nac;
    int32_t tg;
    int32_t src;

    int32_t nac_age_ms;
    int32_t tg_age_ms;
    int32_t src_age_ms;
    int32_t has_sync;
    int32_t voice_active;
    int32_t sync_count;
    int32_t voice_count;
    int32_t bch_ok;
    int32_t bch_fail;
    int32_t polarity_inverted;
    int32_t beep;
    int32_t voice_gate;
    int32_t ring_fill;
    int32_t ring_size;
    uint32_t audio_drops;
    int32_t decode_us;
    char ftype[16];
    char err[48];

    int32_t fm_submode;
    int32_t squelch_tenths;
    int32_t squelch_open;
    int32_t audio_level;
    uint32_t scan_start_hz;
    uint32_t scan_stop_hz;
    uint32_t scan_peak_hz;
    int32_t scan_peak_db;
    uint32_t scan_sweeps;
    int32_t pocsag_baud;
    int32_t pocsag_auto;
    int32_t pocsag_sync;
    uint32_t pocsag_pages;
    uint32_t pocsag_frames;
    uint32_t pocsag_last_addr;
    int32_t pocsag_last_baud;
    char pocsag_last_type[4];
    char pocsag_last_text[80];

    int32_t rec_phase;
    int32_t rec_edges;
    uint32_t rec_span_us;
    int32_t rec_mag;
    int32_t rec_floor;
    int32_t rec_thresh;
    int32_t rec_thresh_fixed;
    int32_t rec_gap_ms;
    uint32_t rec_captures;
    uint32_t rec_bw_hz;
    uint32_t rec_min_pulse_us;
    uint32_t rec_max_span_ms;
    int32_t rec_min_edges;
    int32_t rec_end_reason;
    uint32_t rec_min_mark_us;
    uint32_t rec_max_mark_us;
    uint32_t rec_baud_est;
    char rec_last_file[24];

    int32_t ac_tracked;
    int32_t ac_count;
    int32_t msgs_total;
    /**/
    int32_t tts_vol;
    int32_t msgs_sec;
    int32_t crc_good;
    int32_t crc_err;
    int32_t bursts_sec;
    int32_t mag_avg;
    int32_t mag_peak;
    int32_t last_msg_ms;
    LsAircraft ac[LS_AC_MAX];
} LsTelemetry;

typedef struct LsLink LsLink;

typedef enum {
    LsTransportUart,
    LsTransportBle,
} LsTransport;

typedef enum {
    LsStateUartWaiting,
    LsStateUartUp,
    LsStateBleStarting,
    LsStateBleAdvertising,
    LsStateBleConnected,
    LsStateBleUp,
    LsStateBleFailed,
} LsLinkState;

LsLink* ls_link_alloc(uint32_t baud);
void ls_link_free(LsLink* link);

bool ls_link_set_transport(LsLink* link, LsTransport t);
LsTransport ls_link_transport(LsLink* link);

bool ls_link_ble_connected(LsLink* link);

LsLinkState ls_link_state(LsLink* link);
const char* ls_link_state_str(LsLink* link);

bool ls_link_get(LsLink* link, LsTelemetry* out);

bool ls_link_is_up(LsLink* link);

void ls_link_stats(LsLink* link, uint32_t* frames, uint32_t* replies, uint32_t* bad);

void ls_link_last_reply(LsLink* link, char* out, size_t out_len);

uint32_t ls_link_reply_age_ms(LsLink* link);

void ls_link_send(LsLink* link, const char* fmt, ...);

bool ls_link_rec_take(LsLink* link, uint32_t* offset, int* count, int32_t* out, int max);

void ls_link_rec_reset(LsLink* link);

void ls_link_toggle_port(LsLink* link);

const char* ls_link_port_name(LsLink* link);

void ls_link_selftest(LsLink* link);
