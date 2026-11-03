/* API 2.4: packet/edge sniffer, recorder and explicit sender. */
#include "lakeshark_app.h"
#include <stdio.h>
#include <string.h>
static void print(int x, int y, uint8_t attr, const char *fmt, ...) {
    char text[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    lap_put_str(x, y, text, attr);
}

static int radio = -1, rxh, recording, replay, txh, error;
static unsigned received, replayed;
static uint32_t modes;
static char filename[33], last[64];
static lap_packet_config_t cfg;
static lap_record_status_t recstat;
static void close_rx(void) {
    if (rxh > 0)
        lap_packet_close(rxh);
    rxh = 0;
    if (replay > 0)
        lap_replay_close(replay);
    replay = 0;
}
static void listen(void) {
    if (radio < 0)
        return;
    rxh =
        cfg.modulation == LAP_MOD_OOK ? lap_edges_open(radio, &cfg) : lap_packet_open(radio, &cfg);
    if (rxh < 0) {
        error = rxh;
        rxh = 0;
    }
}
static void enter(void) {
    radio = -1;
    rxh = recording = replay = txh = error = 0;
    received = replayed = 0;
    filename[0] = 0;
    last[0] = 0;
    recstat = (lap_record_status_t){0};
    cfg = (lap_packet_config_t){.size = sizeof(cfg),
                                .version = 1,
                                .frequency_hz = 433920000,
                                .bitrate = 4800,
                                .bandwidth_hz = 125000,
                                .deviation_hz = 5000,
                                .sync_word = 0x12,
                                .preamble = 8,
                                .modulation = LAP_MOD_LORA,
                                .sf = 7,
                                .coding_rate = 5,
                                .crc = 1,
                                .power_dbm = 0};
    lap_caps_t caps;
    if (lap_caps(&caps, sizeof(caps)) < 0 || !(caps.hardware & LAP_HW_PACKET))
        return;
    for (int i = 0; i < caps.radio_count; i++) {
        lap_radio_modes_t m;
        if (!lap_radio_modes(i, &m) && (m.modes & LAP_MODE_LORA)) {
            radio = i;
            modes = m.modes;
            break;
        }
    }
    listen();
}
static void leave(void) {
    close_rx();
    if (recording > 0 && recstat.state <= LAP_IO_RUNNING)
        lap_record_stop(recording);
    if (txh > 0)
        lap_tx_cancel(txh);
}
static void draw(void) {
    if (radio < 0 && !(lap_frame() % 25)) {
        lap_caps_t c;
        lap_caps(&c, sizeof(c));
        for (int i = 0; i < c.radio_count; i++) {
            lap_radio_modes_t m;
            if (!lap_radio_modes(i, &m) && (m.modes & LAP_MODE_LORA)) {
                radio = i;
                modes = m.modes;
                listen();
                break;
            }
        }
    }
    if (txh > 0) {
        lap_io_status_t s;
        if (!lap_tx_status(txh, &s) && s.state >= LAP_IO_DONE) {
            error = s.error;
            txh = 0;
            listen();
        }
    }
    if (recording > 0)
        lap_record_status(recording, &recstat);
    if (replay > 0) {
        lap_record_status_t s;
        lap_replay_status(replay, &s);
    }
    lap_packet_meta_t meta;
    uint8_t packet[255];
    int32_t edges[256];
    int h = replay > 0 ? replay : rxh;
    if (h > 0) {
        int n = cfg.modulation == LAP_MOD_OOK
                    ? lap_edges_read(h, edges, 256, &meta)
                    : (replay > 0 ? lap_replay_read(h, packet, sizeof(packet), &meta)
                                  : lap_packet_read(h, packet, sizeof(packet), &meta));
        if (n > 0) {
            received++;
            if (meta.flags & LAP_DATA_REPLAY)
                replayed++;
            snprintf(last, sizeof(last), "%u bytes %s RSSI %d", meta.length,
                     (meta.flags & LAP_DATA_REPLAY) ? "REPLAY" : "RX", meta.rssi_tenths_dbm);
        }
        if (rxh > 0) {
            lap_io_status_t s;
            lap_packet_status(rxh, &s);
            if (s.error)
                error = s.error;
            lap_radio_rssi(rxh, &meta);
        }
    }
    lap_put_str(1, 1, "PACKET I/O: LoRa / FSK / OOK", LAP_ATTR(LAP_CYAN, LAP_BLACK));
    print(1, 3, LAP_ATTR(LAP_WHITE, LAP_BLACK), "radio %d packets %u replay %u error %d", radio,
          received, replayed, error);
    lap_put_str(1, 5, radio < 0 ? "No supported packet radio fitted" : last,
                LAP_ATTR(LAP_WHITE, LAP_BLACK));
    lap_put_str(1, 7, "F mode  T send  R record/stop  P local replay",
                LAP_ATTR(LAP_GREEN, LAP_BLACK));
    lap_put_str(1, 8, "M mark  K seek  B RF replay  I IQ8 TX  X stop",
                LAP_ATTR(LAP_GREEN, LAP_BLACK));
    print(1, 10, LAP_ATTR(LAP_WHITE, LAP_BLACK), "capture %s state %ld", filename,
          (long)recstat.state);
}
static bool key(lap_key_t key, char ch) {
    if (key != LAP_KEY_CHAR || radio < 0)
        return false;
    if (ch == 'x') {
        close_rx();
        if (recording > 0)
            lap_record_stop(recording);
        if (txh > 0)
            lap_tx_cancel(txh);
        return true;
    }
    if (ch == 'f' && (!recording || recstat.state >= LAP_IO_DONE) && !txh) {
        close_rx();
        if (cfg.modulation == LAP_MOD_LORA && (modes & LAP_MODE_FSK)) {
            cfg.modulation = LAP_MOD_FSK;
            cfg.sync_bits = 32;
            cfg.sync_word = 0x2dd42dd4;
            cfg.payload_bytes = 16;
            cfg.preamble = 32;
            cfg.crc = 0;
        } else if (cfg.modulation == LAP_MOD_FSK && (modes & LAP_MODE_EDGES)) {
            cfg.modulation = LAP_MOD_OOK;
            cfg.sync_bits = 8;
            cfg.sync_word = 0xaa;
            cfg.payload_bytes = 64;
        } else {
            cfg.modulation = LAP_MOD_LORA;
            cfg.sync_word = 0x12;
            cfg.sync_bits = 0;
            cfg.preamble = 8;
            cfg.payload_bytes = 0;
            cfg.crc = 1;
        }
        listen();
        return true;
    }
    if (ch == 'r' && rxh > 0) {
        if (recording > 0 && recstat.state <= LAP_IO_RUNNING) {
            error = lap_record_stop(recording);
        } else {
            snprintf(filename, sizeof(filename), "capture-%lu.lsr", (unsigned long)lap_millis());
            lap_record_config_t c = {.size = sizeof(c),
                                     .version = 1,
                                     .source = rxh,
                                     .format = cfg.modulation == LAP_MOD_OOK ? LAP_RECORD_EDGES
                                                                             : LAP_RECORD_PACKETS,
                                     .frequency_hz = cfg.frequency_hz,
                                     .rate_hz = cfg.bitrate,
                                     .max_bytes = 1024 * 1024};
            strcpy(c.name, filename);
            recording = lap_record_start(&c);
            if (recording < 0) {
                error = recording;
                recording = 0;
            }
        }
        return true;
    }
    if (ch == 'm' && recording > 0) {
        error = lap_record_mark(recording, "operator mark");
        return true;
    }
    if (ch == 'p' && filename[0] && recstat.state == LAP_IO_DONE) {
        close_rx();
        recording = 0;
        replay = lap_replay_open(filename);
        if (replay < 0) {
            error = replay;
            replay = 0;
        }
        return true;
    }
    if (ch == 'k' && replay > 0) {
        error = lap_replay_seek(replay, 0);
        return true;
    }
    if ((ch == 't' || ch == 'i' || ch == 'b') && !txh) {
        lap_caps_t c;
        lap_caps(&c, sizeof(c));
        if (!(c.granted & LAP_CAP_RADIO_TX))
            return true;
        close_rx();
        if (ch == 't' && (modes & LAP_MODE_TX_PACKET)) {
            static const uint8_t frame[16] = "LakeShark SDK24";
            txh = lap_tx_packet(radio, &cfg, frame, sizeof(frame));
        } else if (ch == 'i' && (modes & LAP_MODE_TX_IQ)) {
            int8_t iq[256] = {0};
            txh = lap_tx_iq(radio, &cfg, iq, 128, 125000);
        } else if (ch == 'b' && filename[0] && recstat.state == LAP_IO_DONE)
            txh = lap_tx_replay(radio, &cfg, filename);
        if (txh < 0) {
            error = txh;
            txh = 0;
        }
        return true;
    }
    return false;
}
LAP_APP(.id = "packet_io", .name = "PACKET I/O", .sub = "sniff / record / send", .version = "1.0",
        .author = "LakeShark", .icon = LAP_ICON_WAVE, .color = LAP_CYAN,
        .flags = LAP_CAP_RADIO_RX | LAP_CAP_RADIO_TX, .enter = enter, .leave = leave, .draw = draw,
        .key = key);
