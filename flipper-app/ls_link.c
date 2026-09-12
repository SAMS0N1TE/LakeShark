#include "ls_link.h"

#include <expansion/expansion.h>
#include <bt/bt_service/bt.h>
#include <furi_hal_bt.h>
#include "ls_ble_profile.h"
#include "ls_dbg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "LsLink"

#define RX_STREAM_SZ 1024

#define LINE_MAX 640

struct LsLink {
    FuriHalSerialHandle* serial;
    FuriStreamBuffer* rx_stream;
    FuriThread* worker;
    FuriMutex* lock;
    Expansion* expansion;

    FuriHalSerialId port;
    uint32_t baud;

    volatile bool running;

    LsTelemetry tel;

    LsTelemetry parsed;
    bool have_tel;
    uint32_t last_tel_tick;

    uint32_t frames;
    uint32_t replies;
    uint32_t bad;

    char last_reply[64];
    uint32_t last_reply_tick;

    int32_t rec_chunk[LS_REC_CHUNK];
    uint32_t rec_offset;
    int rec_count;
    bool rec_pending;

    LsTransport transport;
    Bt* bt;
    FuriHalBleProfileBase* ble_profile;
    volatile bool ble_connected;
    volatile bool ble_starting;
    volatile bool ble_failed;
};

static const char* kv(const char* tok, const char* key) {
    size_t n = strlen(key);
    if(strncmp(tok, key, n) != 0) return NULL;
    if(tok[n] != '=') return NULL;
    return tok + n + 1;
}

static void copy_field(char* dst, size_t dst_len, const char* src) {
    if(!strcmp(src, "-")) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';

    for(char* p = dst; *p; p++)
        if(*p == '_') *p = ' ';
}

static void parse_aircraft(LsTelemetry* t, int slot, char* v) {
    if(slot < 0 || slot >= LS_AC_MAX) return;

    /**/
    char* field[11] = {0};
    int n = 0;
    char* p = v;
    field[n++] = p;
    while(*p && n < 11) {
        if(*p == ',') {
            *p++ = '\0';
            field[n++] = p;
            continue;
        }
        p++;
    }
    if(n < 8) return;

    LsAircraft* a = &t->ac[slot];
    a->icao = (uint32_t)strtoul(field[0], NULL, 16);
    copy_field(a->call, sizeof(a->call), field[1]);
    a->altitude = atoi(field[2]);
    a->velocity = atoi(field[3]);
    a->heading = atoi(field[4]);
    a->vert_rate = atoi(field[5]);
    a->age_ms = atoi(field[6]);
    a->msg_count = atoi(field[7]);

    a->pos_valid = (n >= 11) && atoi(field[8]) != 0;
    a->lat_e4 = (n >= 11) ? (int32_t)strtol(field[9], NULL, 10) : 0;
    a->lon_e4 = (n >= 11) ? (int32_t)strtol(field[10], NULL, 10) : 0;

    a->seen = true;
}

static void apply_kv(LsTelemetry* t, char* tok) {
    const char* v;

    if(tok[0] == 'a' && tok[1] >= '0' && tok[1] <= '9') {
        char* eq = strchr(tok, '=');
        if(eq) {
            *eq = '\0';
            parse_aircraft(t, atoi(tok + 1), eq + 1);
            *eq = '=';
            return;
        }
    }

    if((v = kv(tok, "f")))
        t->freq_hz = (uint32_t)strtoul(v, NULL, 10);
    else if((v = kv(tok, "dmn")))
        copy_field(t->demod_name, sizeof(t->demod_name), v);
    else if((v = kv(tok, "dm")))
        t->demod_mode = atoi(v);
    else if((v = kv(tok, "agc")))
        t->agc_on = atoi(v);
    else if((v = kv(tok, "g")))
        t->gain_tenths = atoi(v);
    else if((v = kv(tok, "v")))
        t->volume = atoi(v);
    else if((v = kv(tok, "mu")))
        t->muted = atoi(v);
    else if((v = kv(tok, "rtl")))
        t->rtl_ready = atoi(v);
    else if((v = kv(tok, "iq")))
        t->iq_level = atoi(v);
    else if((v = kv(tok, "re")))
        t->read_errors = atoi(v);
    else if((v = kv(tok, "bps")))
        t->iq_bytes_sec = (uint32_t)strtoul(v, NULL, 10);
    else if((v = kv(tok, "md")))
        copy_field(t->mode_name, sizeof(t->mode_name), v);

    else if((v = kv(tok, "up")))
        t->uptime_s = (uint32_t)strtoul(v, NULL, 10);
    else if((v = kv(tok, "fi")))
        t->free_internal = (uint32_t)strtoul(v, NULL, 10);
    else if((v = kv(tok, "fd")))
        t->free_dma = (uint32_t)strtoul(v, NULL, 10);
    else if((v = kv(tok, "stl")))
        t->sdr_stall_s = atoi(v);
    else if((v = kv(tok, "rhs")))
        copy_field(t->rtl_health, sizeof(t->rtl_health), v);

    else if((v = kv(tok, "eq")))
        t->eq_preset = atoi(v);
    else if((v = kv(tok, "egr")))
        t->eq_gr_db10 = atoi(v);
    else if((v = kv(tok, "eh")))
        t->eq_hp_hz = atoi(v);
    else if((v = kv(tok, "eb")))
        t->eq_bass_db = atoi(v);
    else if((v = kv(tok, "et")))
        t->eq_treb_db = atoi(v);
    else if((v = kv(tok, "ep")))
        t->eq_punch = atoi(v);
    else if((v = kv(tok, "el")))
        t->eq_loud = atoi(v);

    else if((v = kv(tok, "nac")))
        t->nac = atoi(v);
    else if((v = kv(tok, "tg")))
        t->tg = atoi(v);
    else if((v = kv(tok, "src")))
        t->src = atoi(v);
    else if((v = kv(tok, "na")))
        t->nac_age_ms = atoi(v);
    else if((v = kv(tok, "ta")))
        t->tg_age_ms = atoi(v);
    else if((v = kv(tok, "sa")))
        t->src_age_ms = atoi(v);
    else if((v = kv(tok, "sy")))
        t->has_sync = atoi(v);
    else if((v = kv(tok, "vo")))
        t->voice_active = atoi(v);
    else if((v = kv(tok, "sc")))
        t->sync_count = atoi(v);
    else if((v = kv(tok, "vc")))
        t->voice_count = atoi(v);
    else if((v = kv(tok, "bo")))
        t->bch_ok = atoi(v);
    else if((v = kv(tok, "bf")))
        t->bch_fail = atoi(v);
    else if((v = kv(tok, "pol")))
        t->polarity_inverted = atoi(v);
    else if((v = kv(tok, "bp")))
        t->beep = atoi(v);
    else if((v = kv(tok, "vg")))
        t->voice_gate = atoi(v);
    else if((v = kv(tok, "rf")))
        t->ring_fill = atoi(v);
    else if((v = kv(tok, "rs")))
        t->ring_size = atoi(v);
    else if((v = kv(tok, "ad")))
        t->audio_drops = (uint32_t)strtoul(v, NULL, 10);
    else if((v = kv(tok, "dus")))
        t->decode_us = atoi(v);
    else if((v = kv(tok, "ft")))
        copy_field(t->ftype, sizeof(t->ftype), v);
    else if((v = kv(tok, "e")))
        copy_field(t->err, sizeof(t->err), v);

    else if((v = kv(tok, "fm"))) {
        if(!strcmp(v, "listen"))
            t->fm_submode = LsFmListen;
        else if(!strcmp(v, "scan"))
            t->fm_submode = LsFmScan;
        else if(!strcmp(v, "pocsag"))
            t->fm_submode = LsFmPocsag;
        else if(!strcmp(v, "wfm"))
            t->fm_submode = LsFmWfm;
    } else if((v = kv(tok, "sq")))
        t->squelch_tenths = atoi(v);
    else if((v = kv(tok, "so")))
        t->squelch_open = atoi(v);
    else if((v = kv(tok, "au")))
        t->audio_level = atoi(v);
    else if((v = kv(tok, "ss")))
        t->scan_start_hz = (uint32_t)strtoul(v, NULL, 10);
    else if((v = kv(tok, "se")))
        t->scan_stop_hz = (uint32_t)strtoul(v, NULL, 10);
    else if((v = kv(tok, "spk")))
        t->scan_peak_hz = (uint32_t)strtoul(v, NULL, 10);
    else if((v = kv(tok, "sdb")))
        t->scan_peak_db = atoi(v);
    else if((v = kv(tok, "sw")))
        t->scan_sweeps = (uint32_t)strtoul(v, NULL, 10);
    else if((v = kv(tok, "pbd")))
        t->pocsag_last_baud = atoi(v);
    else if((v = kv(tok, "pb")))
        t->pocsag_baud = atoi(v);
    else if((v = kv(tok, "pau")))
        t->pocsag_auto = atoi(v);
    else if((v = kv(tok, "psy")))
        t->pocsag_sync = atoi(v);
    else if((v = kv(tok, "pp")))
        t->pocsag_pages = (uint32_t)strtoul(v, NULL, 10);
    else if((v = kv(tok, "pf")))
        t->pocsag_frames = (uint32_t)strtoul(v, NULL, 10);
    else if((v = kv(tok, "pa")))
        t->pocsag_last_addr = (uint32_t)strtoul(v, NULL, 10);
    else if((v = kv(tok, "pty")))
        copy_field(t->pocsag_last_type, sizeof(t->pocsag_last_type), v);
    else if((v = kv(tok, "ptx")))
        copy_field(t->pocsag_last_text, sizeof(t->pocsag_last_text), v);

    else if((v = kv(tok, "rph")))
        t->rec_phase = atoi(v);
    else if((v = kv(tok, "red")))
        t->rec_edges = atoi(v);
    else if((v = kv(tok, "rsp")))
        t->rec_span_us = (uint32_t)strtoul(v, NULL, 10);
    else if((v = kv(tok, "rmg")))
        t->rec_mag = atoi(v);
    else if((v = kv(tok, "rfl")))
        t->rec_floor = atoi(v);
    else if((v = kv(tok, "rth")))
        t->rec_thresh = atoi(v);
    else if((v = kv(tok, "rtf")))
        t->rec_thresh_fixed = atoi(v);
    else if((v = kv(tok, "rgp")))
        t->rec_gap_ms = atoi(v);
    else if((v = kv(tok, "rcp")))
        t->rec_captures = (uint32_t)strtoul(v, NULL, 10);
    else if((v = kv(tok, "rbw")))
        t->rec_bw_hz = (uint32_t)strtoul(v, NULL, 10);
    else if((v = kv(tok, "rmp")))
        t->rec_min_pulse_us = (uint32_t)strtoul(v, NULL, 10);
    else if((v = kv(tok, "rms")))
        t->rec_max_span_ms = (uint32_t)strtoul(v, NULL, 10);
    else if((v = kv(tok, "rme")))
        t->rec_min_edges = atoi(v);
    else if((v = kv(tok, "ren")))
        t->rec_end_reason = atoi(v);
    else if((v = kv(tok, "rmn")))
        t->rec_min_mark_us = (uint32_t)strtoul(v, NULL, 10);
    else if((v = kv(tok, "rmx")))
        t->rec_max_mark_us = (uint32_t)strtoul(v, NULL, 10);
    else if((v = kv(tok, "rbd")))
        t->rec_baud_est = (uint32_t)strtoul(v, NULL, 10);
    else if((v = kv(tok, "rlf")))
        copy_field(t->rec_last_file, sizeof(t->rec_last_file), v);

    else if((v = kv(tok, "ac")))
        t->ac_tracked = atoi(v);
    else if((v = kv(tok, "acn")))
        t->ac_count = atoi(v);
    else if((v = kv(tok, "mt")))
        t->msgs_total = atoi(v);
    else if((v = kv(tok, "tv")))
        t->tts_vol = atoi(v);
    else if((v = kv(tok, "mps")))
        t->msgs_sec = atoi(v);
    else if((v = kv(tok, "cg")))
        t->crc_good = atoi(v);
    else if((v = kv(tok, "ce")))
        t->crc_err = atoi(v);
    else if((v = kv(tok, "bps1")))
        t->bursts_sec = atoi(v);
    else if((v = kv(tok, "mga")))
        t->mag_avg = atoi(v);
    else if((v = kv(tok, "mgp")))
        t->mag_peak = atoi(v);
    else if((v = kv(tok, "lms")))
        t->last_msg_ms = atoi(v);
}

static LsMode mode_from_name(const char* name) {
    if(!strcmp(name, "FM")) return LsModeFm;
    if(!strcmp(name, "ADSB") || !strcmp(name, "ADS-B")) return LsModeAdsb;
    if(!strcmp(name, "P25")) return LsModeP25;
    if(!strcmp(name, "REC")) return LsModeRec;
    return LsModeUnknown;
}

static void parse_telemetry(LsLink* link, char* line) {
    LsTelemetry* t = &link->parsed;

    LsAircraft carry[LS_AC_MAX];
    memcpy(carry, t->ac, sizeof(carry));

    uint32_t carry_up = t->uptime_s;
    uint32_t carry_fi = t->free_internal;
    uint32_t carry_fd = t->free_dma;

    int32_t carry_eq[7] = {
        t->eq_preset,
        t->eq_hp_hz,
        t->eq_bass_db,
        t->eq_treb_db,
        t->eq_punch,
        t->eq_loud,
        t->eq_gr_db10,
    };

    memset(t, 0, sizeof(*t));
    memcpy(t->ac, carry, sizeof(carry));
    t->uptime_s = carry_up;
    t->free_internal = carry_fi;
    t->free_dma = carry_fd;

    t->eq_preset = carry_eq[0];
    t->eq_hp_hz = carry_eq[1];
    t->eq_bass_db = carry_eq[2];
    t->eq_treb_db = carry_eq[3];
    t->eq_punch = carry_eq[4];
    t->eq_loud = carry_eq[5];
    t->eq_gr_db10 = carry_eq[6];

    t->nac_age_ms = -1;
    t->tg_age_ms = -1;
    t->src_age_ms = -1;
    t->last_msg_ms = -1;

    char* p = line;
    while(*p) {
        while(*p == ' ' || *p == '\t')
            p++;
        if(!*p) break;
        char* tok = p;
        while(*p && *p != ' ' && *p != '\t')
            p++;
        if(*p) *p++ = '\0';
        apply_kv(t, tok);
    }

    t->mode = mode_from_name(t->mode_name);

    if(t->mode == LsModeAdsb) {
        for(int i = t->ac_count; i < LS_AC_MAX; i++) {
            memset(&t->ac[i], 0, sizeof(t->ac[i]));
        }
    }

    furi_mutex_acquire(link->lock, FuriWaitForever);
    link->tel = *t;
    link->have_tel = true;
    link->last_tel_tick = furi_get_tick();
    link->frames++;
    furi_mutex_release(link->lock);
}

static void parse_eq_line(LsLink* link, char* line) {
    LsTelemetry* t = &link->parsed;

    char* p = line;
    while(*p) {
        while(*p == ' ' || *p == '\t')
            p++;
        if(!*p) break;
        char* tok = p;
        while(*p && *p != ' ' && *p != '\t')
            p++;
        if(*p) *p++ = '\0';
        apply_kv(t, tok);
    }

    furi_mutex_acquire(link->lock, FuriWaitForever);
    link->tel.eq_preset = t->eq_preset;
    link->tel.eq_hp_hz = t->eq_hp_hz;
    link->tel.eq_bass_db = t->eq_bass_db;
    link->tel.eq_treb_db = t->eq_treb_db;
    link->tel.eq_punch = t->eq_punch;
    link->tel.eq_loud = t->eq_loud;
    link->tel.eq_gr_db10 = t->eq_gr_db10;
    furi_mutex_release(link->lock);
}

static void parse_rec_chunk(LsLink* link, char* line) {
    if(line[0] != 'D') return;

    char* p = line + 1;
    char* end = NULL;

    uint32_t off = (uint32_t)strtoul(p, &end, 10);
    if(end == p) return;
    p = end;

    long count = strtol(p, &end, 10);
    if(end == p || count < 0 || count > LS_REC_CHUNK) return;
    p = end;

    int32_t vals[LS_REC_CHUNK];
    int n = 0;
    while(n < count) {
        long v = strtol(p, &end, 10);
        if(end == p) break;
        vals[n++] = (int32_t)v;
        p = end;
    }
    if(n != count) return;

    furi_mutex_acquire(link->lock, FuriWaitForever);
    memcpy(link->rec_chunk, vals, sizeof(int32_t) * (size_t)n);
    link->rec_offset = off;
    link->rec_count = n;
    link->rec_pending = true;
    furi_mutex_release(link->lock);
}

bool ls_link_rec_take(LsLink* link, uint32_t* offset, int* count, int32_t* out, int max) {
    furi_mutex_acquire(link->lock, FuriWaitForever);
    bool got = link->rec_pending;
    if(got) {
        link->rec_pending = false;
        int n = link->rec_count;
        if(n > max) n = max;
        if(offset) *offset = link->rec_offset;
        if(count) *count = n;
        if(out && n > 0) memcpy(out, link->rec_chunk, sizeof(int32_t) * (size_t)n);
    }
    furi_mutex_release(link->lock);
    return got;
}

void ls_link_rec_reset(LsLink* link) {
    furi_mutex_acquire(link->lock, FuriWaitForever);
    link->rec_pending = false;
    link->rec_count = 0;
    link->rec_offset = 0;
    furi_mutex_release(link->lock);
}

static void handle_line(LsLink* link, char* line) {
    if(line[0] == '%') {
        parse_rec_chunk(link, line + 1);
    } else if(line[0] == '&') {
        parse_eq_line(link, line + 1);
    } else if(line[0] == '$') {

        if((link->frames % 25) == 0) FURI_LOG_I(TAG, "rx frame: %s", line);
        parse_telemetry(link, line + 1);
    } else if(line[0] == '+' || line[0] == '-') {
        furi_mutex_acquire(link->lock, FuriWaitForever);
        strncpy(link->last_reply, line, sizeof(link->last_reply) - 1);
        link->last_reply[sizeof(link->last_reply) - 1] = '\0';
        link->last_reply_tick = furi_get_tick();
        link->replies++;
        furi_mutex_release(link->lock);
    } else {
        FURI_LOG_W(TAG, "rx junk: %s", line);
        furi_mutex_acquire(link->lock, FuriWaitForever);
        link->bad++;
        furi_mutex_release(link->lock);
    }
}

static void rx_isr(FuriHalSerialHandle* handle, FuriHalSerialRxEvent event, void* ctx) {
    LsLink* link = ctx;
    if(event & FuriHalSerialRxEventData) {

        uint8_t b = furi_hal_serial_async_rx(handle);

        if(!link || !link->rx_stream) return;

        furi_stream_buffer_send(link->rx_stream, &b, 1, 0);
    }
}

static int32_t worker(void* ctx) {
    LsLink* link = ctx;
    char line[LINE_MAX];
    size_t pos = 0;
    uint8_t buf[64];

    while(link->running) {
        size_t n = furi_stream_buffer_receive(link->rx_stream, buf, sizeof(buf), 50);
        for(size_t i = 0; i < n; i++) {
            char c = (char)buf[i];
            if(c == '\n' || c == '\r') {
                if(pos > 0) {
                    line[pos] = '\0';
                    handle_line(link, line);
                    pos = 0;
                }
            } else if(pos < LINE_MAX - 1) {
                line[pos++] = c;
            } else {
                pos = 0;
            }
        }

    }
    return 0;
}

static void ls_ble_rx_cb(const uint8_t* data, uint16_t len, void* ctx) {
    LsLink* link = ctx;
    furi_stream_buffer_send(link->rx_stream, data, len, 0);
}

static void ls_bt_status_cb(BtStatus status, void* ctx) {
    LsLink* link = ctx;
    link->ble_connected = (status == BtStatusConnected);
    FURI_LOG_I(
        TAG,
        "BT status: %s",
        status == BtStatusConnected   ? "connected" :
        status == BtStatusAdvertising ? "advertising" :
        status == BtStatusOff         ? "off" :
                                        "unavailable");

}

static bool ls_ble_start(LsLink* link) {
    if(link->ble_profile) return true;

    if(!furi_hal_bt_is_active()) {
        FURI_LOG_E(TAG, "Bluetooth is off - enable it in Settings > Bluetooth");
        link->ble_failed = true;
        return false;
    }

    ls_dbg("  ble_start: opening BT record");
    link->bt = furi_record_open(RECORD_BT);

    bt_disconnect(link->bt);
    furi_delay_ms(200);

    ls_dbg("  ble_start: disconnect done, setting status cb");
    bt_set_status_changed_callback(link->bt, ls_bt_status_cb, link);

    ls_dbg("  ble_start: calling bt_profile_start(ls_ble_profile)");
    link->ble_profile = bt_profile_start(link->bt, ls_ble_profile, NULL);
    ls_dbg("  ble_start: bt_profile_start returned %s", link->ble_profile ? "a profile" : "NULL");
    if(!link->ble_profile) {
        FURI_LOG_E(TAG, "bt_profile_start failed");
        bt_set_status_changed_callback(link->bt, NULL, NULL);
        furi_record_close(RECORD_BT);
        link->bt = NULL;
        link->ble_failed = true;
        return false;
    }

    ls_ble_profile_set_rx_callback(link->ble_profile, ls_ble_rx_cb, link);

    furi_hal_bt_start_advertising();
    link->ble_failed = false;
    FURI_LOG_I(TAG, "LakeShark BLE profile active, advertising");
    return true;
}

static void ls_ble_stop(LsLink* link) {
    if(!link->ble_profile) return;
    ls_ble_profile_set_rx_callback(link->ble_profile, NULL, NULL);
    link->ble_profile = NULL;
    link->ble_connected = false;

    if(link->bt) {
        bt_set_status_changed_callback(link->bt, NULL, NULL);
        bt_profile_restore_default(link->bt);
        furi_record_close(RECORD_BT);
        link->bt = NULL;
    }
}

bool ls_link_ble_connected(LsLink* link) {
    return link && link->transport == LsTransportBle && link->ble_connected;
}

LsTransport ls_link_transport(LsLink* link) {
    return link ? link->transport : LsTransportUart;
}

LsLinkState ls_link_state(LsLink* link) {
    if(!link) return LsStateUartWaiting;

    bool up = ls_link_is_up(link);

    if(link->transport == LsTransportUart) {
        return up ? LsStateUartUp : LsStateUartWaiting;
    }

    if(link->ble_starting) return LsStateBleStarting;
    if(link->ble_failed) return LsStateBleFailed;
    if(!link->ble_connected) return LsStateBleAdvertising;
    return up ? LsStateBleUp : LsStateBleConnected;
}

const char* ls_link_state_str(LsLink* link) {
    switch(ls_link_state(link)) {
    case LsStateUartWaiting:
        return "waiting";
    case LsStateUartUp:
        return "linked";
    case LsStateBleStarting:
        return "starting";
    case LsStateBleAdvertising:
        return "advertising";
    case LsStateBleConnected:
        return "connected";
    case LsStateBleUp:
        return "linked";
    case LsStateBleFailed:
        return "BT off?";
    }
    return "?";
}

bool ls_link_set_transport(LsLink* link, LsTransport t) {
    furi_assert(link);
    if(link->transport == t) return true;

    if(t == LsTransportBle) {

        link->ble_starting = true;
        bool ok = ls_ble_start(link);
        link->ble_starting = false;
        if(!ok) return false;

        furi_hal_serial_async_rx_stop(link->serial);
    } else {
        ls_ble_stop(link);
        link->ble_failed = false;
        furi_hal_serial_async_rx_start(link->serial, rx_isr, link, false);
    }

    furi_mutex_acquire(link->lock, FuriWaitForever);
    link->have_tel = false;
    link->last_tel_tick = 0;
    furi_mutex_release(link->lock);

    link->transport = t;
    FURI_LOG_I(TAG, "transport -> %s", t == LsTransportBle ? "BLE" : "UART");
    return true;
}

LsLink* ls_link_alloc(uint32_t baud) {
    LsLink* link = malloc(sizeof(LsLink));
    memset(link, 0, sizeof(LsLink));

    link->expansion = furi_record_open(RECORD_EXPANSION);
    expansion_disable(link->expansion);

    link->lock = furi_mutex_alloc(FuriMutexTypeNormal);
    link->rx_stream = furi_stream_buffer_alloc(RX_STREAM_SZ, 1);

    link->port = LS_LINK_PORT;
    link->baud = baud;
    link->serial = furi_hal_serial_control_acquire(link->port);
    furi_check(link->serial);
    furi_hal_serial_init(link->serial, baud);
    FURI_LOG_I(TAG, "acquired %s @%lu baud", ls_link_port_name(link), (unsigned long)baud);

    link->running = true;

    link->worker = furi_thread_alloc_ex("LsLinkWorker", 3072, worker, link);
    furi_thread_start(link->worker);

    furi_hal_serial_async_rx_start(link->serial, rx_isr, link, false);
    return link;
}

void ls_link_free(LsLink* link) {
    furi_assert(link);

    if(link->transport == LsTransportBle) {
        ls_ble_stop(link);
    } else {
        furi_hal_serial_async_rx_stop(link->serial);
    }
    link->running = false;
    furi_thread_join(link->worker);
    furi_thread_free(link->worker);

    furi_hal_serial_deinit(link->serial);
    furi_hal_serial_control_release(link->serial);

    furi_stream_buffer_free(link->rx_stream);
    furi_mutex_free(link->lock);

    expansion_enable(link->expansion);
    furi_record_close(RECORD_EXPANSION);

    free(link);
}

void ls_link_selftest(LsLink* link) {
    char sample[] = "$ f=851012500 dm=1 dmn=CQPSK g=340 agc=0 v=72 mu=1 nac=659 "
                    "tg=1201 src=987654 na=120 ta=250 sa=250 sy=1 vo=1 sc=12 vc=340 "
                    "bo=142 bf=3 iq=372 pol=1 bp=1 vg=25 rtl=1 rf=4096 rs=8192 re=0 "
                    "bps=491520 ad=2 dus=8100 md=P25 ft=LDU1 e=no_sync "
                    "up=3600 fi=14163 fd=3311";
    handle_line(link, sample);

    char adsb[] = "$ f=1090000000 g=496 v=72 mu=0 rtl=1 bps=2000000 md=ADSB "
                  "ac=3 mt=1840 mps=12 cg=1802 ce=38 bps1=140 mga=41 mgp=97 lms=320 "
                  "aci=0 acn=3 a0=A4E1BF,UAL123,35000,470,271,0,850,42 "
                  "a1=AB12CD,-,12250,220,88,-1200,1500,7 "
                  "a2=3C6444,DLH44,38000,505,95,0,300,61 "
                  "up=3601 fi=14100 fd=3300";
    handle_line(link, adsb);

    LsTelemetry* tp = malloc(sizeof(LsTelemetry));
    if(!tp) return;

    furi_mutex_acquire(link->lock, FuriWaitForever);
    *tp = link->tel;

    memset(&link->tel, 0, sizeof(link->tel));
    memset(&link->parsed, 0, sizeof(link->parsed));
    link->have_tel = false;
    link->last_tel_tick = 0;
    link->frames = 0;
    furi_mutex_release(link->lock);

    FURI_LOG_I(
        TAG,
        "selftest adsb md=%s tracked=%ld n=%ld crc=%ld/%ld mag=%ld/%ld last=%ldms",
        tp->mode_name,
        (long)tp->ac_tracked,
        (long)tp->ac_count,
        (long)tp->crc_good,
        (long)tp->crc_err,
        (long)tp->mag_avg,
        (long)tp->mag_peak,
        (long)tp->last_msg_ms);
    for(int i = 0; i < 3; i++) {
        FURI_LOG_I(
            TAG,
            "selftest ac[%d] %06lX '%s' alt=%ld spd=%ld hdg=%ld vs=%ld age=%ldms",
            i,
            (unsigned long)tp->ac[i].icao,
            tp->ac[i].call,
            (long)tp->ac[i].altitude,
            (long)tp->ac[i].velocity,
            (long)tp->ac[i].heading,
            (long)tp->ac[i].vert_rate,
            (long)tp->ac[i].age_ms);
    }
    FURI_LOG_I(
        TAG,
        "selftest sys up=%lus int=%lu dma=%lu",
        (unsigned long)tp->uptime_s,
        (unsigned long)tp->free_internal,
        (unsigned long)tp->free_dma);

    free(tp);
}

const char* ls_link_port_name(LsLink* link) {

    if(link->transport == LsTransportBle) return "BLE";
    return (link->port == FuriHalSerialIdLpuart) ? "LPUART 15/16" : "USART 13/14";
}

void ls_link_toggle_port(LsLink* link) {

    static bool told = false;
    if(!told) {
        told = true;
        FURI_LOG_I(TAG, "port probing disabled - staying on %s", ls_link_port_name(link));
    }
}

bool ls_link_get(LsLink* link, LsTelemetry* out) {
    furi_mutex_acquire(link->lock, FuriWaitForever);
    bool ok = link->have_tel;
    if(ok) *out = link->tel;
    furi_mutex_release(link->lock);
    return ok;
}

bool ls_link_is_up(LsLink* link) {
    furi_mutex_acquire(link->lock, FuriWaitForever);
    bool up = link->have_tel &&
              (furi_get_tick() - link->last_tel_tick) < furi_ms_to_ticks(LS_LINK_TIMEOUT_MS);
    furi_mutex_release(link->lock);
    return up;
}

void ls_link_stats(LsLink* link, uint32_t* frames, uint32_t* replies, uint32_t* bad) {
    furi_mutex_acquire(link->lock, FuriWaitForever);
    if(frames) *frames = link->frames;
    if(replies) *replies = link->replies;
    if(bad) *bad = link->bad;
    furi_mutex_release(link->lock);
}

void ls_link_last_reply(LsLink* link, char* out, size_t out_len) {
    furi_mutex_acquire(link->lock, FuriWaitForever);
    strncpy(out, link->last_reply, out_len - 1);
    out[out_len - 1] = '\0';
    furi_mutex_release(link->lock);
}

uint32_t ls_link_reply_age_ms(LsLink* link) {
    furi_mutex_acquire(link->lock, FuriWaitForever);
    uint32_t tick = link->last_reply_tick;
    furi_mutex_release(link->lock);
    if(!tick) return UINT32_MAX;
    return (furi_get_tick() - tick) * 1000 / furi_kernel_get_tick_frequency();
}

void ls_link_send(LsLink* link, const char* fmt, ...) {
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if(n < 0) return;
    if(n > (int)sizeof(buf) - 2) n = (int)sizeof(buf) - 2;
    FURI_LOG_I(TAG, "tx: %.*s", n, buf);
    buf[n++] = '\n';

    if(link->transport == LsTransportBle) {

        if(link->ble_profile) {

            bool ok = ls_ble_profile_tx(link->ble_profile, (const uint8_t*)buf, (uint16_t)n);
            if(!ok) FURI_LOG_W(TAG, "ble tx reported failure (%d B)", n);
        }
        return;
    }

    furi_hal_serial_tx(link->serial, (const uint8_t*)buf, (size_t)n);
    furi_hal_serial_tx_wait_complete(link->serial);
}
