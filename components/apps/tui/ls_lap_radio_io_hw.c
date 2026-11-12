/* Board packet/edge adapter. Only lap_io touches these bus operations. */
#if defined(ESP_PLATFORM) && !defined(LS_LAP_HOST_TEST)
#include "ls_tx_unlock.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lr_iq_spectrum.h"
#include "ls_cc_capture.h"
#include "ls_field.h"
#include "ls_lap_radio_io.h"
#include "ls_lap_features.h"
#include "ls_lap_service.h"
#include "ls_lora.h"
#include "ls_mesh.h"
#include "ls_mixrf.h"
#include "ls_nfc_suite.h"
#include "tx_protection.h"
#include "hackrf_radio.h"
#include <limits.h>
#include <math.h>
#include <string.h>
extern int ls_lap_io_channels(const char *, uint32_t, lap_channel_t *, int, bool);
static struct {
    bool lease, svc, opened, edges, tx, mix, hackrf;
    int mod;
    lap_packet_config_t cfg;
    uint32_t drops;
} hw;
static int map(esp_err_t e) {
    return e == ESP_OK                  ? 0
           : e == ESP_ERR_NOT_SUPPORTED ? LAP_RX_EUNSUPPORTED
           : e == ESP_ERR_NOT_FOUND     ? LAP_RX_ENODEV
           : e == ESP_ERR_INVALID_ARG   ? LAP_RX_ERANGE
           : e == ESP_ERR_INVALID_STATE ? LAP_RX_EBUSY
                                        : LAP_RX_EIO;
}
static int16_t level(float f) { return isfinite(f) ? (int16_t)(f * 10) : INT16_MIN; }
static int modes(int radio, lap_radio_modes_t *out) {
    lap_radio_info_t r;
    int e = ls_lap_radio_snapshot(radio, &r);
    if (e < 0 || !r.present)
        return LAP_RX_ENODEV;
    memset(out, 0, sizeof(*out));
    out->size = sizeof(*out);
    out->version = 1;
    out->max_tx_us = 10000000;
    out->duty_window_ms = 60000;
    out->duty_airtime_ms = 10000;
    if (r.kind == LAP_RADIO_KIND_HACKRF) {
        out->modes=LAP_HACKRF_TX ? LAP_MODE_TX_IQ : 0; out->min_power_dbm=0; out->max_power_dbm=15; return 0;
    }
    if (r.kind == LAP_RADIO_KIND_MIXRF) {
        out->modes = LAP_MODE_OOK | LAP_MODE_EDGES | LAP_MODE_RSSI | LAP_MODE_TX_EDGES;
        out->min_power_dbm = -10;
        out->max_power_dbm = 10;
        return 0;
    }
    if (r.kind != LAP_RADIO_KIND_LR2021 && r.kind != LAP_RADIO_KIND_SX1262)
        return LAP_RX_EUNSUPPORTED;
    uint32_t c = ls_lora_fitted_caps();
    out->min_power_dbm = -9;
    out->max_power_dbm = 22;
    if (c & LS_LORA_CAP_LORA)
        out->modes |= LAP_MODE_LORA | LAP_MODE_TX_PACKET;
    if (c & LS_LORA_CAP_FSK)
        out->modes |= LAP_MODE_FSK;
    if (c & LS_LORA_CAP_MODES_RX)
        out->modes |= LAP_MODE_OOK | LAP_MODE_EDGES;
    if (c & LS_LORA_CAP_RSSI_INST)
        out->modes |= LAP_MODE_RSSI;
    /* IQ upload is ROM-version-dependent; the send reports unsupported ROMs. */
    if (r.kind == LAP_RADIO_KIND_LR2021)
        out->modes |= LAP_MODE_TX_IQ | LAP_MODE_TX_EDGES;
    return 0;
}
static void close_rx(void) {
    if (hw.mix) {
        ls_mixrf_receive_owned(LS_MIXRF_OWNER_APP, false, 0);
        hw.mix = false;
    }
    if (hw.opened) {
        if (hw.mod == LAP_MOD_FSK)
            ls_lora_fsk_end();
        else if (hw.mod == LAP_MOD_OOK)
            ls_lora_ook_end();
        else
            ls_lora_park();
        hw.opened = false;
    }
    if (hw.lease) {
        if (hw.svc) ls_mesh_radio_hold_svc(false);
        else ls_mesh_radio_hold(false);
        hw.lease = hw.svc = false;
    }
}
/* The app is running with its screen closed: a background service, which
   holds the LoRa chip below every radio app (ls_mesh_radio_hold_svc). */
static bool in_background(void) { return ls_lap_service_on() && !ls_lap_service_shown(); }
static int configure(const lap_packet_config_t *c) {
    if (c->modulation == LAP_MOD_LORA) {
        ls_lora_cfg_t l;
        ls_lora_cfg_default(&l);
        l.freq_hz = c->frequency_hz;
        l.sf = c->sf;
        l.bw_hz = c->bandwidth_hz;
        l.cr = c->coding_rate;
        l.preamble = c->preamble;
        l.sync_word = c->sync_word;
        l.crc_on = c->crc;
        l.power_dbm = c->power_dbm;
        l.implicit_rx_bytes = c->payload_bytes;
        /* Image calibration selects the physical frequency, not a region. */
        l.cal_min_mhz = (uint32_t)c->frequency_hz / 1000000;
        l.cal_max_mhz = l.cal_min_mhz + 1;
        return map(ls_lora_configure(&l));
    }
    if (c->modulation == LAP_MOD_FSK) {
        if (c->crc)
            return LAP_RX_EUNSUPPORTED;
        ls_fsk_cfg_t f = {.freq_hz = c->frequency_hz,
                          .bitrate = c->bitrate,
                          .deviation_hz = c->deviation_hz,
                          .bandwidth_hz = ls_lora_fsk_bw_snap(c->bandwidth_hz),
                          .sync_word = c->sync_word,
                          .payload_bytes = c->payload_bytes,
                          .preamble_bits = c->preamble,
                          .power_dbm = c->power_dbm,
                          .sync_bits = c->sync_bits};
        return map(ls_lora_fsk_begin(&f));
    }
    if (c->crc)
        return LAP_RX_EUNSUPPORTED;
    ls_ook_cfg_t o = {.freq_hz = c->frequency_hz,
                      .bitrate = c->bitrate,
                      .bandwidth_hz = c->bandwidth_hz,
                      .pattern = c->sync_word,
                      .pattern_bits = c->sync_bits,
                      .frame_bytes = c->payload_bytes ? c->payload_bytes : 64,
                      .boost = -1};
    return map(ls_lora_ook_begin(&o));
}
static int lease(void) {
    if (!hw.lease) {
        if (ls_field_owned() || ls_lora_scanning() || lr_spec_center())
            return LAP_RX_EBUSY;
        hw.svc = in_background();
        if (!hw.svc)
            ls_mesh_radio_hold(true);
        hw.lease = true;  /* asked; close_rx lets go of whichever kind */
    }
    if (!hw.svc)
        return ls_mesh_radio_held() ? 0 : LAP_RX_EAGAIN;
    /* A session already on the part never drops its hold here: step_aside
       closes it, parking the chip before the hold goes. */
    if (hw.opened)
        return ls_mesh_radio_held_svc() ? 0 : LAP_RX_EAGAIN;
    /* Ask, or poll the request. Refused (and nothing held) while a radio
       app or MeshCore wants the chip: queued. */
    return ls_mesh_radio_hold_svc(true) ? 0 : LAP_RX_EAGAIN;
}
/* Before every worker pass. A transmission in flight finishes first; the
   send owns its lease until ls_lora_send_done. */
static bool step_aside(int radio) {
    if (hw.mix || hw.hackrf || hw.tx) return false;
    if (hw.lease) {
        /* Screen opened or closed: the lease is the wrong kind now. Reopen. */
        if (hw.svc != in_background()) return true;
        return hw.svc && ls_mesh_radio_svc_yield();
    }
    /* Not holding: would a service lease on the LoRa chip be refused now? */
    lap_radio_info_t r;
    if (!in_background() || ls_lap_radio_snapshot(radio, &r) < 0 ||
        (r.kind != LAP_RADIO_KIND_LR2021 && r.kind != LAP_RADIO_KIND_SX1262))
        return false;
    return ls_mesh_radio_svc_yield();
}
static int open_rx(int radio, const lap_packet_config_t *cfg, bool edges) {
    lap_radio_info_t r;
    if (ls_lap_radio_snapshot(radio, &r) < 0 || !r.present)
        return LAP_RX_ENODEV;
    if (r.kind == LAP_RADIO_KIND_MIXRF) {
        if (!edges || cfg->modulation != LAP_MOD_OOK || cfg->crc)
            return LAP_RX_EUNSUPPORTED;
        if (!ls_mixrf_receive_owned(LS_MIXRF_OWNER_APP, true, cfg->frequency_hz))
            return LAP_RX_EBUSY;
        ls_cc_capture_clear();
        hw.mix = true;
        hw.edges = true;
        hw.cfg = *cfg;
        return 0;
    }
    if (r.kind != LAP_RADIO_KIND_LR2021 && r.kind != LAP_RADIO_KIND_SX1262)
        return LAP_RX_EUNSUPPORTED;
    int e = lease();
    if (e)
        return e;
    e = configure(cfg);
    if (e < 0) {
        close_rx();
        return e;
    }
    hw.opened = true;
    hw.mod = cfg->modulation;
    hw.edges = edges;
    hw.cfg = *cfg;
    if (cfg->modulation == LAP_MOD_LORA)
        e = map(ls_lora_receive());
    if (e < 0)
        close_rx();
    return e;
}
static int rssi(int16_t *out) {
    if (hw.mix) {
        ls_mixrf_status_t s;
        ls_mixrf_snapshot(&s);
        if (!s.cc || !s.receiving)
            return LAP_RX_EAGAIN;
        *out = level(s.rssi);
        return 0;
    }
    float f = NAN;
    int e = map(ls_lora_rssi_inst(&f));
    if (!e)
        *out = level(f);
    return e;
}
static int read_rx(void *data, int cap, lap_packet_meta_t *m) {
    if (hw.mix) {
        unsigned dropped = 0;
        int n = ls_cc_capture_read(data, cap / 4, &dropped);
        m->dropped = dropped;
        if (dropped)
            m->flags |= LAP_DATA_GAP;
        m->length = n * 4;
        return n * 4;
    }
    uint8_t bits[255];
    float r = NAN, s = NAN;
    int n = hw.mod == LAP_MOD_LORA  ? ls_lora_poll(bits, sizeof(bits), &r, &s)
            : hw.mod == LAP_MOD_FSK ? ls_lora_fsk_poll(bits, sizeof(bits), &r)
                                    : ls_lora_ook_poll(bits, sizeof(bits), &r);
    if (n < 0) {
        hw.drops++;
        return 0;
    }
    if (!n)
        return 0;
    m->dropped = hw.drops;
    if (hw.drops)
        m->flags |= LAP_DATA_GAP;
    m->rssi_tenths_dbm = level(r);
    m->snr_tenths_db = level(s);
    if (!hw.edges) {
        if (n > cap)
            return LAP_IO_ESIZE;
        memcpy(data, bits, n);
        m->length = n;
        return n;
    }
    int32_t *out = data;
    unsigned used = 0, run = 0;
    int prev = (bits[0] >> 7) & 1;
    for (int i = 0; i < n * 8; i++) {
        int v = (bits[i / 8] >> (7 - i % 8)) & 1;
        if (v == prev)
            run++;
        else {
            if (used >= (unsigned)cap / 4) {
                m->flags |= LAP_DATA_GAP;
                break;
            }
            unsigned us = ((uint64_t)run * 1000000 + hw.cfg.bitrate / 2) / hw.cfg.bitrate;
            out[used++] = prev ? (int32_t)(us ? us : 1) : -(int32_t)(us ? us : 1);
            prev = v;
            run = 1;
        }
    }
    if (used < (unsigned)cap / 4 && run) {
        unsigned us = ((uint64_t)run * 1000000 + hw.cfg.bitrate / 2) / hw.cfg.bitrate;
        out[used++] = prev ? (int32_t)(us ? us : 1) : -(int32_t)(us ? us : 1);
    }
    m->length = used * 4;
    return used * 4;
}
static int tx_start(int radio, const lap_packet_config_t *c, int format, const void *data,
                    size_t bytes, uint32_t rate, unsigned vga, bool amp) {
    ls_tx_clear_error(LS_TX_SOCKET);
    ls_tx_clear_error(LS_TX_MIXRF);
    if (c->port == LAP_PORT_RX_ONLY)
        return LAP_IO_ERXPORT;
    lap_radio_info_t r;
    if (ls_lap_radio_snapshot(radio, &r) < 0 || !r.present)
        return LAP_RX_ENODEV;
    if (r.kind == LAP_RADIO_KIND_HACKRF) {
#if LAP_HACKRF_TX
        if(format!=LAP_RECORD_IQ16) return LAP_RX_EUNSUPPORTED;
        int e=ls_hackrf_tx_start(c->frequency_hz,rate,vga,amp,c->power_dbm,data,bytes);
        if(!e) hw.hackrf=true;
        return e;
#else
        return LAP_RX_EUNSUPPORTED;
#endif
    }
    if (r.kind == LAP_RADIO_KIND_MIXRF) {
        if (format != LAP_RECORD_EDGES || c->modulation != LAP_MOD_OOK)
            return LAP_RX_EUNSUPPORTED;
        if (bytes % 4 || bytes / 4 > 4096)
            return LAP_IO_ESIZE;
        bool ok = ls_mixrf_replay(c->frequency_hz, data, bytes / 4, c->power_dbm);
        int e = ls_tx_last_error(LS_TX_MIXRF);
        return ok ? 0 : e < 0 ? e : LAP_RX_EIO;
    }
    if(r.kind!=LAP_RADIO_KIND_LR2021&&r.kind!=LAP_RADIO_KIND_SX1262) return LAP_RX_EUNSUPPORTED;
    int e = lease();
    if (e)
        return e;
    if (format == LAP_RECORD_IQ16) {
        if (!LS_TX_UNLOCKED && c->power_dbm > 10) {
            close_rx();
            return LAP_IO_EPOWER;
        }
        e = configure(c);
        if (!e)
            e = map(ls_lora_iq_send_rate_reason(data, bytes, c->frequency_hz, c->power_dbm, rate,
                                                NULL));
        int p = ls_tx_last_error(LS_TX_SOCKET);
        close_rx();
        return e < 0 && p < 0 ? p : e;
    }
    uint8_t chips[255];
    lap_packet_config_t adapted = *c;
    if (format == LAP_RECORD_EDGES) {
        if (c->modulation != LAP_MOD_OOK || bytes % 4 || bytes / 4 > 4096) {
            close_rx();
            return LAP_RX_EUNSUPPORTED;
        }
        const int32_t *p = data;
        uint64_t elapsed = 0;
        unsigned at = 0;
        memset(chips, 0, sizeof(chips));
        for (size_t i = 0; i < bytes / 4; i++) {
            if (!p[i]) {
                close_rx();
                return LAP_IO_EFORMAT;
            }
            elapsed += p[i] < 0 ? -(int64_t)p[i] : p[i];
            uint64_t end = (elapsed * c->bitrate + 500000) / 1000000;
            if (end > sizeof(chips) * 8) {
                close_rx();
                return LAP_IO_ESIZE;
            }
            while (at < end) {
                if (p[i] > 0)
                    chips[at / 8] |= 1u << (7 - at % 8);
                at++;
            }
        }
        if (!at) {
            close_rx();
            return LAP_IO_EFORMAT;
        }
        data = chips;
        bytes = (at + 7) / 8;
        adapted.modulation = LAP_MOD_OOK;
        c = &adapted;
    }
    if (c->modulation == LAP_MOD_OOK) {
        ls_fsk_cfg_t f = {.freq_hz = c->frequency_hz,
                          .bitrate = c->bitrate,
                          .deviation_hz = c->deviation_hz ? c->deviation_hz : 1000,
                          .bandwidth_hz = ls_lora_fsk_bw_snap(c->bandwidth_hz),
                          .payload_bytes = bytes,
                          .power_dbm = c->power_dbm,
                          .sync_bits = c->sync_bits};
        e = map(ls_lora_fsk_begin(&f));
        hw.mod = LAP_MOD_FSK;
    } else {
        e = configure(c);
        hw.mod = c->modulation;
    }
    if (e < 0) {
        close_rx();
        return e;
    }
    hw.opened = true;
    e = map(c->modulation == LAP_MOD_LORA  ? ls_lora_send(data, bytes)
            : c->modulation == LAP_MOD_FSK ? ls_lora_fsk_send(data, bytes)
                                           : ls_lora_ook_send(data, bytes, c->bitrate));
    if (e < 0) {
        int p = ls_tx_last_error(LS_TX_SOCKET);
        close_rx();
        return p < 0 ? p : e;
    }
    hw.tx = true;
    return 0;
}
static int tx_poll(void) {
#if LAP_HACKRF_TX
    if(hw.hackrf) { int e=ls_hackrf_tx_poll(); if(e) hw.hackrf=false; return e; }
#endif
    if (!hw.tx)
        return 1;
    if (!ls_lora_send_done())
        return 0;
    hw.tx = false;
    close_rx();
    return 1;
}
void ls_lap_io_tx_interrupt(void) {
#if LAP_HACKRF_TX
    ls_hackrf_tx_cancel();
#endif
    ls_lora_tx_cancel_request();
    ls_mixrf_replay_cancel_request();
}
static void tx_cancel(void) {
#if LAP_HACKRF_TX
    if(hw.hackrf) { ls_hackrf_tx_cancel(); return; }
#endif
    ls_lora_tx_cancel_request(); /* Bounded packet owns lease until completion. */
    if (!hw.tx || ls_lora_send_done()) {
        hw.tx = false;
        close_rx();
    }
}
#if LAP_NFC
static uint32_t nfc_caps(void) {
    ls_mixrf_status_t s;
    ls_mixrf_snapshot(&s);
    return s.nfc ? (LAP_HW_NFC | LAP_HW_NDEF_WRITE) : 0;
}
static int nfc_open(void) { return ls_nfc_tag_open() ? 0 : LAP_RX_EBUSY; }
static int nfc_read(lap_nfc_event_t *out, void *data, size_t cap) {
    if (!(nfc_caps() & LAP_HW_NFC))
        return LAP_RX_ENODEV;
    ls_nfc_tag_result_t r;
    int e = ls_nfc_tag_read(&r, data, cap);
    if (e <= 0)
        return e;
    memset(out, 0, sizeof(*out));
    out->kind = r.kind;
    out->error = r.error;
    out->capacity = r.capacity;
    out->ndef_bytes = r.len;
    out->uid_len = r.uid_len;
    memcpy(out->uid, r.uid, r.uid_len);
    out->atqa = r.atqa;
    out->sak = r.sak;
    out->writable = r.writable;
    return 1;
}
static int nfc_write(const void *data, size_t len) { return ls_nfc_tag_write(data, len); }
static void nfc_close(void) { ls_nfc_tag_close(); }
#else
static uint32_t nfc_caps(void) { return 0; }
static int nfc_open(void) { return LAP_RX_EUNSUPPORTED; }
static int nfc_read(lap_nfc_event_t *out, void *data, size_t cap) { return LAP_RX_EUNSUPPORTED; }
static int nfc_write(const void *data, size_t len) { return LAP_RX_EUNSUPPORTED; }
static void nfc_close(void) {}
#endif
const ls_lap_io_backend_t *ls_lap_io_backend(void) {
    static const ls_lap_io_backend_t be = {
        modes,     open_rx,  read_rx,  rssi,     close_rx,  tx_start,  tx_poll,
        tx_cancel, nfc_caps, nfc_open, nfc_read, nfc_write, nfc_close, ls_lap_io_channels,
        step_aside};
    return &be;
}
bool ls_lap_io_sim_command(const char *command) {
    (void)command;
    return false;
}
#endif
