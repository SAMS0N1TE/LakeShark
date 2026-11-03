/* Deterministic radio/NFC backend; no RF or device access. */
#include "ls_lap_radio_io.h"
#include "ls_lap_sys.h"
#include "tx_protection.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern int ls_lap_io_channels(const char *, uint32_t, lap_channel_t *, int, bool);
static struct {
    bool open, edges, nfc, tag, tx, hackrf_tx;
    unsigned tx_vga; bool tx_amp; int tx_power;
    lap_packet_config_t cfg;
    uint32_t modes;
    int rssi, error;
    unsigned seq;
    uint8_t packet[255];
    int bytes;
    int32_t edges_data[256];
    int edge_count;
    lap_nfc_event_t ev;
    uint8_t ndef[2048];
    unsigned ev_pending;
    int tx_wait;
} sim;
extern bool ls_lap_radio_sim_absent, ls_lap_radio_sim_hackrf;
static unsigned max_vga=47; static bool amp_allowed=true, rx_busy;
static int modes(int radio, lap_radio_modes_t *out) {
    if (ls_lap_radio_sim_absent)
        return LAP_RX_ENODEV;
    lap_radio_info_t r;
    if (ls_lap_radio_snapshot(radio, &r) < 0 || !r.present)
        return LAP_RX_ENODEV;
    out->size = sizeof(*out);
    out->version = 1;
    out->modes = sim.modes;
    out->min_power_dbm = -9;
    out->max_power_dbm = 22;
    if(r.kind==LAP_RADIO_KIND_HACKRF) { out->modes=LAP_MODE_TX_IQ; out->min_power_dbm=0; out->max_power_dbm=15; }
    out->max_tx_us = 10000000;
    out->duty_window_ms = 60000;
    out->duty_airtime_ms = 10000;
    return 0;
}
static int open_rx(int radio, const lap_packet_config_t *cfg, bool edges) {
    lap_radio_modes_t m;
    int e = modes(radio, &m);
    if (e)
        return e;
    if (!(m.modes & (1u << (cfg->modulation - 1))))
        return LAP_RX_EUNSUPPORTED;
    if (sim.open || sim.tx)
        return LAP_RX_EBUSY;
    if (cfg->modulation == LAP_MOD_FSK && cfg->crc)
        return LAP_RX_EUNSUPPORTED;
    sim.open = true;
    sim.cfg = *cfg;
    sim.edges = edges;
    return 0;
}
static int rssi(int16_t *out) {
    if (!sim.open)
        return LAP_RX_ENODEV;
    *out = sim.rssi;
    return 0;
}
static int read_rx(void *data, int cap, lap_packet_meta_t *meta) {
    lap_radio_modes_t m;
    if (modes(0, &m) < 0)
        return LAP_RX_ENODEV;
    if (sim.error)
        return sim.error;
    meta->flags = LAP_DATA_SYNTHETIC;
    meta->rssi_tenths_dbm = sim.rssi;
    meta->snr_tenths_db = sim.cfg.modulation == LAP_MOD_LORA ? 75 : INT16_MIN;
    int n = 0;
    if (sim.edges) {
        n = sim.edge_count * 4;
        if (n > cap)
            return LAP_IO_ESIZE;
        if (n) {
            memcpy(data, sim.edges_data, n);
            sim.edge_count = 0;
        }
    } else if (sim.bytes) {
        n = sim.bytes;
        if (n > cap)
            return LAP_IO_ESIZE;
        memcpy(data, sim.packet, n);
        sim.bytes = 0;
    } else if (!(++sim.seq % 10)) {
        n = 8;
        memcpy(data, "SIM-RX24", 8);
    }
    meta->length = n;
    return n;
}
static void close_rx(void) { sim.open = false; }
#ifdef ESP_PLATFORM
#include "esp_timer.h"
static uint64_t now_us(void) { return esp_timer_get_time(); }
#else
extern uint32_t (*ls_lap_api_host_clock)(void);
static uint64_t now_us(void) {
    return ls_lap_api_host_clock ? (uint64_t)ls_lap_api_host_clock() * 1000u : 0;
}
#endif
static int transmit(int radio, const lap_packet_config_t *cfg, int fmt, const void *data,
                    size_t bytes, uint32_t rate, unsigned vga, bool amp) {
    lap_radio_modes_t m;
    int e = modes(radio, &m);
    if (e)
        return e;
    if (sim.open || sim.tx || rx_busy)
        return LAP_RX_EBUSY;
    lap_radio_info_t radio_info; ls_lap_radio_snapshot(radio,&radio_info);
    bool hackrf=radio_info.kind==LAP_RADIO_KIND_HACKRF;
    unsigned us = 0;
    if (fmt == LAP_RECORD_PACKETS) {
        if (!(m.modes & LAP_MODE_TX_PACKET))
            return LAP_RX_EUNSUPPORTED;
        if (cfg->modulation == LAP_MOD_LORA)
            us = ls_tx_lora_airtime(bytes, cfg->sf, cfg->bandwidth_hz, cfg->coding_rate,
                                    cfg->preamble, cfg->crc, false);
        else
            us =
                (uint64_t)(bytes * 8 + cfg->sync_bits + cfg->preamble) * 1000000 / cfg->bitrate + 1;
    } else if (fmt == LAP_RECORD_IQ16) {
        if (!(m.modes & LAP_MODE_TX_IQ) || !rate)
            return LAP_RX_EUNSUPPORTED;
        if (hackrf) {
            if(rate<2000000||rate>20000000) return LAP_RX_ERANGE;
            if(vga>47||vga>max_vga||(amp&&!amp_allowed)) return LAP_IO_EPOWER;
            if(!bytes||bytes%2||bytes>65534) return LAP_IO_ESIZE;
            us=(uint64_t)(bytes/2)*1000000/rate+500001;
        } else {
        if (rate != 4800 && rate != 9600 && rate != 19200 && rate != 38400 && rate != 76800 &&
            rate != 125000)
            return LAP_RX_EUNSUPPORTED;
        if (cfg->power_dbm > 10)
            return LAP_IO_EPOWER;
        if (bytes % 2 || !bytes || bytes / 2 > 32767)
            return LAP_IO_ESIZE;
        const int8_t *iq = data;
        for (size_t i = 0; i < bytes; i += 2)
            if (iq[i] * iq[i] + iq[i + 1] * iq[i + 1] > 56 * 56)
                return LAP_RX_ERANGE;
        us = (uint64_t)(bytes / 2) * 1000000 / rate + 1;
        if (us > 900000)
            return LAP_IO_ESIZE;
        }
    } else if (fmt == LAP_RECORD_EDGES) {
        if (!(m.modes & LAP_MODE_TX_EDGES) || bytes % 4 || bytes / 4 > 4096)
            return LAP_RX_EUNSUPPORTED;
        const int32_t *p = data;
        uint64_t span = 0;
        for (size_t i = 0; i < bytes / 4; i++) {
            if (p[i] == 0)
                return LAP_IO_EFORMAT;
            span += p[i] < 0 ? -(int64_t)p[i] : p[i];
        }
        if (span > 10000000)
            return LAP_IO_EDUTY;
        us = span;
    } else
        return LAP_RX_EUNSUPPORTED;
    e = ls_tx_protect(hackrf?LS_TX_HACKRF:LS_TX_SIM, cfg->power_dbm, cfg->port == LAP_PORT_RX_ONLY, us,
                      now_us());
    if (e)
        return e;
    sim.tx = true; sim.hackrf_tx=hackrf; sim.tx_vga=vga; sim.tx_amp=amp; sim.tx_power=cfg->power_dbm;
    sim.tx_wait = 2;
    return 0;
}
static int tx_poll(void) {
    if (!sim.tx)
        return 1;
    if(ls_lap_radio_sim_absent) { sim.tx=false; return LAP_RX_ENODEV; }
    if(sim.hackrf_tx) {
        int e=sim.tx_vga>max_vga||(sim.tx_amp&&!amp_allowed)?LAP_IO_EPOWER:
            ls_tx_protect(LS_TX_HACKRF,sim.tx_power,false,1,now_us());
        if(e) { sim.tx=false; return e; }
    }
    if (--sim.tx_wait > 0)
        return 0;
    sim.tx = false;
    return sim.error ? sim.error : 1;
}
void ls_lap_io_tx_interrupt(void) { sim.tx = false; }
static void tx_cancel(void) { sim.tx = false; }
static uint32_t nfc_caps(void) { return sim.nfc ? (LAP_HW_NFC | LAP_HW_NDEF_WRITE) : 0; }
static int nfc_open(void) { return sim.nfc ? 0 : LAP_RX_ENODEV; }
static int nfc_read(lap_nfc_event_t *out, void *data, size_t cap) {
    if (!sim.ev_pending)
        return 0;
    if (sim.ev.ndef_bytes > cap)
        return LAP_IO_ESIZE;
    *out = sim.ev;
    out->flags |= LAP_DATA_SYNTHETIC;
    memcpy(data, sim.ndef, sim.ev.ndef_bytes);
    sim.ev_pending = 0;
    return 1;
}
static int nfc_write(const void *data, size_t len) {
    if (!sim.tag)
        return LAP_RX_ENODEV;
    if (!sim.ev.writable)
        return LAP_RX_EACCES;
    if (len + 3 > sim.ev.capacity)
        return LAP_IO_ESIZE;
    memcpy(sim.ndef, data, len);
    sim.ev.ndef_bytes = len;
    sim.ev.kind = 5;
    sim.ev_pending = 1;
    return 0;
}
static void nfc_close(void) { sim.ev_pending = 0; }
bool ls_lap_io_sim_command(const char *c) {
    if (!strncmp(c, "iorsti=", 8))
        return false;
    if (!strncmp(c, "iorssi=", 7)) {
        sim.rssi = atoi(c + 7);
        return true;
    }
    if (!strncmp(c, "iomodes=", 8)) {
        sim.modes = strtoul(c + 8, NULL, 0);
        return true;
    }
    if (!strncmp(c, "iofilefail=", 11)) {
        ls_lap_io_file_fault(atoi(c + 11));
        return true;
    }
    int a,b;
    if(sscanf(c,"hackrf=%d",&a)==1) { ls_lap_radio_sim_hackrf=a!=0; return true; }
    if(sscanf(c,"hackrflimits=%d,%d",&a,&b)==2) { if(a>=0&&a<=47) { max_vga=a; amp_allowed=b!=0; } return true; }
    if(sscanf(c,"hackrfrx=%d",&a)==1) { rx_busy=a!=0; return true; }
    if(sscanf(c,"hackrftemp=%d",&a)==1) { ls_tx_thermal(LS_TX_HACKRF,a!=0); return true; }
    if (!strncmp(c, "ioerror=", 8)) {
        sim.error = atoi(c + 8);
        return true;
    }
    if (!strncmp(c, "iotemp=", 7)) {
        ls_tx_thermal(LS_TX_SIM, atoi(c + 7) != 0);
        return true;
    }
    if (!strncmp(c, "packethex=", 10)) {
        sim.bytes = 0;
        const char *p = c + 10;
        while (p[0] && p[1] && sim.bytes < 255) {
            char pair[3] = {p[0], p[1], 0};
            char *end;
            long v = strtol(pair, &end, 16);
            if (*end)
                break;
            sim.packet[sim.bytes++] = v;
            p += 2;
        }
        return true;
    }
    if (!strncmp(c, "nfclock=", 8)) {
        sim.ev.writable = !atoi(c + 8);
        sim.ev.kind = 2;
        sim.ev_pending = 1;
        return true;
    }
    if (!strncmp(c, "packet=", 7)) {
        size_t len = strlen(c + 7);
        if (len > 255)
            len = 255;
        memcpy(sim.packet, c + 7, len);
        sim.bytes = len;
        return true;
    }
    if (!strncmp(c, "edges=", 6)) {
        const char *p = c + 6;
        sim.edge_count = 0;
        while (*p && sim.edge_count < 256) {
            char *end;
            long n = strtol(p, &end, 10);
            if (end == p || !n)
                break;
            sim.edges_data[sim.edge_count++] = n;
            p = *end == ',' ? end + 1 : end;
        }
        return true;
    }
    if (!strncmp(c, "nfc=", 4)) {
        sim.nfc = strcmp(c + 4, "none") != 0;
        sim.tag = sim.nfc && strcmp(c + 4, "removed") != 0;
        memset(&sim.ev, 0, sizeof(sim.ev));
        sim.ev.kind = sim.tag ? 2 : 3;
        sim.ev.uid_len = sim.tag ? 4 : 0;
        memcpy(sim.ev.uid, "SIM2", 4);
        sim.ev.capacity = 144;
        sim.ev.writable = true;
        if (sim.tag) {
            unsigned n = strlen(c + 4);
            if (n > 120)
                n = 120;
            sim.ndef[0] = 0xd1;
            sim.ndef[1] = 1;
            sim.ndef[2] = n + 3;
            sim.ndef[3] = 'T';
            sim.ndef[4] = 2;
            sim.ndef[5] = 'e';
            sim.ndef[6] = 'n';
            memcpy(sim.ndef + 7, c + 4, n);
            sim.ev.ndef_bytes = n + 7;
        }
        sim.ev_pending = 1;
        return true;
    }
    return false;
}
const ls_lap_io_backend_t *ls_lap_io_backend(void) {
    static bool init;
    if (!init) {
        init = true;
        sim.modes = 255;
        sim.rssi = -1000;
        sim.nfc = getenv("LAPSIM_NFC") != NULL;
    }
    static const ls_lap_io_backend_t be = {
        modes,     open_rx,  read_rx,  rssi,     close_rx,  transmit,  tx_poll,
        tx_cancel, nfc_caps, nfc_open, nfc_read, nfc_write, nfc_close, ls_lap_io_channels};
    return &be;
}
