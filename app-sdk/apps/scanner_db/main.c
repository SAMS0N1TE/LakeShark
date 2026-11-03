/* API 2.4: firmware channel database -> app-defined scanner workflow. */
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

static lap_channel_t rows[64];
static int job, scan, radio = -1, count, error;
static bool writing, held;
static char bank[33] = "active";
static unsigned page, hits;
static void start_scan(void) {
    if (scan > 0)
        lap_scan_stop(scan);
    scan = 0;
    if (radio < 0 || !count)
        return;
    lap_scan_config_t c = {.size = sizeof(c),
                           .version = 1,
                           .radio = radio,
                           .receiver = {.size = sizeof(c.receiver),
                                        .version = 1,
                                        .frequency_hz = 433920000,
                                        .bandwidth_hz = 125000,
                                        .sync_word = 0x12,
                                        .preamble = 8,
                                        .modulation = LAP_MOD_LORA,
                                        .sf = 7,
                                        .coding_rate = 5,
                                        .crc = 1},
                           .dwell_ms = 120,
                           .hang_ms = 200,
                           .squelch_tenths_dbm = -750};
    for (int i = 0; i < count; i++)
        if ((rows[i].flags & 1) && !(rows[i].flags & 2))
            c.frequencies[c.count++] = rows[i].frequency_hz;
    if (c.count) {
        scan = lap_scan_start(&c);
        if (scan < 0) {
            error = scan;
            scan = 0;
        }
    }
}
static void query(void) {
    job = lap_channels_query(bank, page, 64);
    writing = false;
    if (job < 0) {
        error = job;
        job = 0;
    }
}
static void enter(void) {
    radio = -1;
    job = scan = count = error = 0;
    page = hits = 0;
    writing = held = false;
    strcpy(bank, "active");
    lap_caps_t c;
    lap_caps(&c, sizeof(c));
    for (int i = 0; i < c.radio_count; i++) {
        lap_radio_modes_t m;
        if (!lap_radio_modes(i, &m) && (m.modes & LAP_MODE_LORA) && (m.modes & LAP_MODE_RSSI)) {
            radio = i;
            break;
        }
    }
    query();
}
static void leave(void) {
    if (scan > 0)
        lap_scan_stop(scan);
}
static void draw(void) {
    if (radio < 0 && !(lap_frame() % 25)) {
        lap_caps_t c;
        lap_caps(&c, sizeof(c));
        for (int i = 0; i < c.radio_count; i++) {
            lap_radio_modes_t m;
            if (!lap_radio_modes(i, &m) && (m.modes & LAP_MODE_LORA) && (m.modes & LAP_MODE_RSSI)) {
                radio = i;
                start_scan();
                break;
            }
        }
    }
    if (job > 0) {
        lap_io_status_t s;
        if (!lap_io_status(job, &s) && s.state >= LAP_IO_DONE) {
            if (s.error) {
                error = s.error;
                job = 0;
            } else if (writing) {
                job = 0;
                query();
            } else {
                int n = lap_channels_read(job, rows, 64);
                job = 0;
                if (n >= 0) {
                    count = n;
                    start_scan();
                } else
                    error = n;
            }
        }
    }
    lap_scan_status_t s = {0};
    if (scan > 0) {
        lap_scan_status(scan, &s);
        lap_packet_meta_t hit;
        while (lap_scan_next(scan, &hit) > 0)
            hits++;
    }
    lap_put_str(1, 1, "SCANNER / FIRMWARE CHANNEL BANKS", LAP_ATTR(LAP_CYAN, LAP_BLACK));
    print(1, 3, LAP_ATTR(LAP_WHITE, LAP_BLACK), "bank %s rows %d page %u", bank, count, page);
    print(1, 4, LAP_ATTR(LAP_WHITE, LAP_BLACK), "radio %d frequency %lu hits %u error %d", radio,
          (unsigned long)s.frequency_hz, hits, error);
    lap_put_str(1, 6, "H hold/resume  N next  S skip  P page  X stop",
                LAP_ATTR(LAP_GREEN, LAP_BLACK));
    lap_put_str(1, 7, "W write example bank sdk-scanner (2 channels)",
                LAP_ATTR(LAP_GREEN, LAP_BLACK));
    if (radio < 0)
        lap_put_str(1, 9, "No packet/RSSI radio; database still available",
                    LAP_ATTR(LAP_YELLOW, LAP_BLACK));
}
static bool key(lap_key_t key, char ch) {
    if (key != LAP_KEY_CHAR)
        return false;
    if (ch == 'w' && !job) {
        lap_channel_t c[2] = {0};
        for (int i = 0; i < 2; i++) {
            c[i].size = sizeof(c[i]);
            c[i].version = 1;
            c[i].frequency_hz = 433920000 + i * 125000;
            c[i].mode = 1;
            c[i].flags = 1;
            snprintf(c[i].name, sizeof(c[i].name), "SDK channel %d", i + 1);
        }
        strcpy(bank, "sdk-scanner");
        page = 0;
        job = lap_channels_write(bank, c, 2);
        writing = true;
        if (job < 0) {
            error = job;
            job = 0;
        }
        return true;
    }
    if (ch == 'p' && !job) {
        page += 64;
        query();
        return true;
    }
    if (ch == 'x') {
        leave();
        scan = 0;
        return true;
    }
    if (scan > 0) {
        if (ch == 'h') {
            held = !held;
            error = lap_scan_control(scan, held ? LAP_SCAN_HOLD : LAP_SCAN_RESUME);
            return true;
        }
        if (ch == 'n' || ch == 's') {
            error = lap_scan_control(scan, ch == 'n' ? LAP_SCAN_NEXT : LAP_SCAN_SKIP);
            return true;
        }
    }
    return false;
}
LAP_APP(.id = "scanner_db", .name = "SCANNER DB", .sub = "channel banks / squelch",
        .version = "1.0", .author = "LakeShark", .icon = LAP_ICON_TOWER, .color = LAP_GREEN,
        .flags = LAP_CAP_RADIO_RX, .enter = enter, .leave = leave, .draw = draw, .key = key);
