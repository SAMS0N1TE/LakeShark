#include "sx1262_console.h"
#include "ls_lora.h"
#include "ls_mesh.h"
#include "pocsag.h"
#include "ls_fsk_capture.h"
#include "ls_sub_fsk.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <sys/stat.h>
#include <unistd.h>

static bool number(const char *s, double *out)
{
    char *end;
    *out = strtod(s, &end);
    return s != end && !*end && std::isfinite(*out);
}

int sx1262_receive_command(int argc, char **argv)
{
    const bool paging = argc > 1 && !strcmp(argv[1], "pocsag");
    if ((paging && (argc < 4 || argc > 6)) ||
        (!paging && (argc < 8 || argc > 9))) {
        printf("lora pocsag MHz baud [seconds=30] [invert=0]\n"
               "lora fsk MHz baud deviation_Hz bandwidth_Hz sync_hex bytes [seconds=30]\n");
        return 1;
    }
    double mhz, baud, seconds = 30, dev = 4500, bw = 19500, bytes = 64;
    const int secs_arg = paging ? 4 : 8;
    bool valid = number(argv[2], &mhz) && number(argv[3], &baud);
    if (argc > secs_arg) valid = number(argv[secs_arg], &seconds) && valid;
    if (!paging) valid = number(argv[4], &dev) && number(argv[5], &bw) &&
                         number(argv[7], &bytes) && valid;
    bool inverted = paging && argc > 5 && !strcmp(argv[5], "1");
    if (paging && argc > 5 && strcmp(argv[5], "0") && strcmp(argv[5], "1")) valid = false;
    if (!valid || mhz < 150 || mhz > 960 || baud < 600 || baud > 300000 ||
        floor(baud) != baud || seconds < 1 || seconds > 120 ||
        dev < 600 || dev > 200000 || bw < 4800 || bw > 467000 ||
        bytes < 1 || bytes > 255 || floor(bytes) != bytes ||
        (paging && baud != 1200 && baud != 2400)) {
        printf("lora: invalid receive settings; POCSAG supports 1200/2400 baud\n");
        return 1;
    }
    uint32_t sync = inverted ? ~0x7cd215d8u : 0x7cd215d8u;
    if (!paging) {
        char *end;
        unsigned long long value = strtoull(argv[6], &end, 16);
        if (end == argv[6] || *end || value > 0xffffffffull) return 1;
        sync = (uint32_t)value;
    }
    if (ls_lora_scanning() || ls_lora_fsk_active()) {
        printf("lora: radio busy\n");
        return 1;
    }
    fm_state_t *state = nullptr;
    pocsag_ctx_t *decoder = nullptr;
    if (paging) {
        state = (fm_state_t *)heap_caps_calloc(1, sizeof(*state), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (state) decoder = pocsag_create(state, (int)baud);
        if (!decoder) { heap_caps_free(state); printf("lora: no memory\n"); return 1; }
    }
    const int64_t hold_deadline = esp_timer_get_time() + 1000000;
    while (!ls_mesh_radio_hold(true) && esp_timer_get_time() < hold_deadline)
        vTaskDelay(pdMS_TO_TICKS(10));
    if (!ls_mesh_radio_held()) {
        ls_mesh_radio_hold(false);
        pocsag_destroy(decoder); heap_caps_free(state);
        printf("lora: radio busy\n");
        return 1;
    }
    ls_fsk_cfg_t cfg = {(uint32_t)llround(mhz * 1e6), (uint32_t)baud,
                       (uint32_t)dev, (uint32_t)bw, sync, (uint8_t)bytes};
    esp_err_t err = ls_lora_fsk_begin(&cfg);
    if (err == ESP_OK) {
        printf("lora: experimental %s RX %.6f MHz %u baud, %.0f seconds\n",
               paging ? "POCSAG" : "FSK", mhz, (unsigned)cfg.bitrate, seconds);
        /* Said before the session rather than after it: the receiver will
           happily read a longer frame, the capture ring will not hold one,
           and finding that out at the end costs the whole session. */
        if (!paging && bytes > LS_FSK_CAPTURE_BYTES)
            printf("lora: frames will be printed but not filed - a capture "
                   "holds %d bytes\n", LS_FSK_CAPTURE_BYTES);
        const int64_t end = esp_timer_get_time() + (int64_t)(seconds * 1e6);
        int64_t previous = 0;
        unsigned packets = 0, errors = 0;
        uint8_t packet[255];
        /* What the channel sounded like, decoded or not.

           A session that ends "0 packets" cannot tell you whether the band
           was quiet or the receiver was deaf, and those want completely
           different next steps. The floor and the peak separate them: a peak
           at the floor means nothing transmitted, and a peak well above it
           means something did and this did not decode it. */
        float floor_dbm = 0, peak_dbm = -200.0f;
        unsigned looks = 0;
        while (esp_timer_get_time() < end) {
            float rssi;
            int n = ls_lora_fsk_poll(packet, sizeof(packet), &rssi);
            int64_t now = esp_timer_get_time();
            if (n < 0) { errors++; previous = 0; }
            if (n > 0) {
                packets++;
                if (paging) {
                    const int64_t period = 544000000LL / cfg.bitrate;
                    const bool contiguous = previous &&
                        llabs(now - previous - period) <= period / 10 + 20000;
                    uint32_t before = pocsag_n_pages(decoder);
                    pocsag_process_batch(decoder, packet, n, inverted, contiguous);
                    uint32_t count = pocsag_n_pages(decoder) - before;
                    if (count > FM_PAGE_LOG_MAX) count = FM_PAGE_LOG_MAX;
                    for (uint32_t i = count; i > 0; i--) {
                        const fm_page_t *p = &state->pages[(state->page_head + FM_PAGE_LOG_MAX - i) % FM_PAGE_LOG_MAX];
                        printf("POCSAG %.1f dBm RIC=%lu F=%u %c %s\n", rssi,
                               (unsigned long)p->address, p->function, p->type, p->text);
                    }
                } else {
                    printf("FSK %.1f dBm", rssi);
                    for (int i = 0; i < n; i++) printf(" %02X", packet[i]);
                    /* Filed as well as printed: a session that showed a
                       frame and kept nothing is one you cannot replay from,
                       and the frame you wanted is always the one that went
                       past while you were reading the last. */
                    /* A frame longer than the ring's payload is printed and
                       not filed.  Keeping its first 64 bytes would look
                       exactly like keeping the frame, and replaying that is
                       not a weaker version of replaying this one - it is a
                       different transmission, sent on purpose, by someone
                       who believes they are repeating what they heard.  The
                       bytes are all on the line above either way, so nothing
                       is lost that was not already unreplayable. */
                    if (n > LS_FSK_CAPTURE_BYTES) {
                        printf("  -> not filed, %d bytes over the %d a capture"
                               " holds\n", n - LS_FSK_CAPTURE_BYTES,
                               LS_FSK_CAPTURE_BYTES);
                    } else {
                        ls_fsk_capture_t cap = {};
                        cap.freq_hz       = cfg.freq_hz;
                        cap.bitrate       = cfg.bitrate;
                        cap.deviation_hz  = cfg.deviation_hz;
                        cap.bandwidth_hz  = cfg.bandwidth_hz;
                        cap.sync_word     = cfg.sync_word;
                        cap.preamble_bits = cfg.preamble_bits
                                          ? cfg.preamble_bits : 32;
                        cap.len = (uint8_t)n;
                        memcpy(cap.data, packet, cap.len);
                        cap.rssi_dbm = rssi;
                        cap.first_us = now;
                        const int slot = ls_fsk_capture_add(&cap);
                        if (slot >= 0) printf("  -> [%d]", slot);
                        printf("\n");
                    }
                }
                previous = now;
            }
            float inst;
            if (ls_lora_rssi_inst(&inst) == ESP_OK) {
                if (!looks++ || inst < floor_dbm) floor_dbm = inst;
                if (inst > peak_dbm) peak_dbm = inst;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        if (looks)
            printf("lora: channel floor %.1f dBm, peak %.1f dBm over %u looks\n",
                   (double)floor_dbm, (double)peak_dbm, looks);
        printf("lora: %u packets, %u read errors", packets, errors);
        if (paging) printf(", %lu pages, %lu BCH errors",
            (unsigned long)pocsag_n_pages(decoder), (unsigned long)pocsag_n_cwerr(decoder));
        printf("\n");
        err = ls_lora_fsk_end();
    }
    ls_mesh_radio_hold(false);
    pocsag_destroy(decoder); heap_caps_free(state);
    if (err != ESP_OK) printf("lora: %s\n", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

/* ---- generic FSK transmit --------------------------------------------- */

/* The other half of `lora fsk`: same modem, pointed outwards.

   Everything a 2-FSK frame needs is an argument here - carrier, rate,
   deviation, filter, sync word, preamble length, power and the bytes - so
   this is the radio's whole transmit capability and not a helper for one
   protocol. Nothing about any particular product is encoded in it.

   Pulse shaping stays off and the hardware CRC stays off, which is what
   ls_lora_fsk_begin already configures: a protocol whose CRC the radio
   cannot generate simply puts its own in the payload, and one that wants no
   CRC at all sends the bytes as they are. */
static int fsk_transmit_command(int argc, char **argv)
{
    if (argc != 10) {
        printf("lora fsktx MHz baud deviation_Hz bandwidth_Hz sync_hex "
               "preamble_bits dBm payload_hex\n"
               "  sync_hex is 32 bits, sent ahead of the payload\n"
               "  preamble_bits is a multiple of 8, 8..1024\n"
               "  payload_hex is an even number of hex digits, 1..255 bytes\n");
        return 1;
    }

    double mhz, baud, dev, bw, preamble, dbm;
    if (!number(argv[2], &mhz) || !number(argv[3], &baud) ||
        !number(argv[4], &dev) || !number(argv[5], &bw) ||
        !number(argv[7], &preamble) || !number(argv[8], &dbm)) {
        printf("lora: those are not all numbers\n");
        return 1;
    }

    char *end;
    const unsigned long long sync = strtoull(argv[6], &end, 16);
    if (end == argv[6] || *end || sync > 0xffffffffull) {
        printf("lora: sync word must be up to 32 bits of hex\n");
        return 1;
    }

    const char *hex = argv[9];
    const size_t digits = strlen(hex);
    if (digits == 0 || digits % 2 || digits > 510) {
        printf("lora: payload must be an even number of hex digits, "
               "1..255 bytes\n");
        return 1;
    }
    uint8_t payload[255];
    for (size_t i = 0; i < digits; i += 2) {
        char pair[3] = {hex[i], hex[i + 1], 0};
        const unsigned long v = strtoul(pair, &end, 16);
        if (end != pair + 2) {
            printf("lora: '%s' is not a hex byte\n", pair);
            return 1;
        }
        payload[i / 2] = (uint8_t)v;
    }
    const size_t bytes = digits / 2;

    if (mhz < 150 || mhz > 960 || baud < 600 || baud > 300000 ||
        floor(baud) != baud || dev < 600 || dev > 200000 ||
        bw < 4800 || bw > 467000 || dbm < -9 || dbm > 22 ||
        floor(dbm) != dbm || preamble < 8 || preamble > 1024 ||
        floor(preamble) != preamble || (long)preamble % 8) {
        printf("lora: invalid transmit settings\n");
        return 1;
    }
    /* Refused here rather than by a session that starts and then hears
       nothing: the modem needs the signal to fit inside the filter. */
    if (baud + 2 * dev > bw) {
        printf("lora: %.0f Hz of signal will not fit a %.0f Hz filter - "
               "raise the bandwidth\n", baud + 2 * dev, bw);
        return 1;
    }

    if (ls_lora_scanning() || ls_lora_fsk_active()) {
        printf("lora: radio busy\n");
        return 1;
    }

    const int64_t hold_deadline = esp_timer_get_time() + 1000000;
    while (!ls_mesh_radio_hold(true) && esp_timer_get_time() < hold_deadline)
        vTaskDelay(pdMS_TO_TICKS(10));
    if (!ls_mesh_radio_held()) {
        ls_mesh_radio_hold(false);
        printf("lora: radio busy\n");
        return 1;
    }

    ls_fsk_cfg_t cfg = {};
    cfg.freq_hz       = (uint32_t)llround(mhz * 1e6);
    cfg.bitrate       = (uint32_t)baud;
    cfg.deviation_hz  = (uint32_t)dev;
    cfg.bandwidth_hz  = (uint32_t)bw;
    cfg.sync_word     = (uint32_t)sync;
    cfg.payload_bytes = (uint8_t)bytes;
    cfg.preamble_bits = (uint16_t)preamble;
    cfg.power_dbm     = (int8_t)dbm;

    esp_err_t err = ls_lora_fsk_begin(&cfg);
    if (err == ESP_OK) {
        const unsigned bits = (unsigned)(preamble + 32 + bytes * 8);
        printf("lora: FSK TX %.6f MHz, %u baud, %.1f kHz deviation, "
               "%u-bit preamble, sync %08lX, %d dBm\n",
               mhz, (unsigned)cfg.bitrate, dev / 1000.0,
               (unsigned)cfg.preamble_bits, (unsigned long)cfg.sync_word,
               (int)dbm);
        printf("  payload");
        for (size_t i = 0; i < bytes; i++) printf(" %02X", payload[i]);
        printf("\n");

        err = ls_lora_fsk_send(payload, bytes);
        if (err == ESP_OK) {
            const int64_t end_us = esp_timer_get_time() + 2000000;
            while (!ls_lora_send_done() && esp_timer_get_time() < end_us)
                vTaskDelay(pdMS_TO_TICKS(5));
            printf("lora: sent - %u bits, %u ms on air\n", bits,
                   (unsigned)(bits * 1000u / cfg.bitrate));
        }
        const esp_err_t ended = ls_lora_fsk_end();
        if (err == ESP_OK) err = ended;
    }
    ls_mesh_radio_hold(false);
    if (err != ESP_OK) printf("lora: %s\n", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

int sx1262_transmit_command(int argc, char **argv)
{
    return fsk_transmit_command(argc, argv);
}

/* ---- captures: list, replay, save ------------------------------------- */

/* Read, save, send - and the send half is the part this board could not do
   until the FSK path learned to transmit. A capture carries the settings that
   heard it, so replaying one never asks the operator to remember how they
   were tuned; getting that wrong is a frame on the wrong channel, which looks
   exactly like a frame that did not work. */

static void capture_print(int index, const ls_fsk_capture_t *c)
{
    printf("  [%d] %.4f MHz  %u baud  %.1f kHz dev  sync %08lX  %.0f dBm",
           index, c->freq_hz / 1e6, (unsigned)c->bitrate,
           c->deviation_hz / 1000.0, (unsigned long)c->sync_word,
           (double)c->rssi_dbm);
    const uint32_t period = ls_fsk_capture_period_ms(index);
    if (c->heard > 1) {
        printf("  x%lu", (unsigned long)c->heard);
        /* A transmitter on a rhythm names its own period, which is usually
           the most identifying thing about it. */
        if (period) printf(" every %lu.%03lu s",
                           (unsigned long)(period / 1000),
                           (unsigned long)(period % 1000));
    }
    printf("\n       ");
    for (uint8_t i = 0; i < c->len; i++) printf(" %02X", c->data[i]);
    printf("\n");
}

static int capture_list_command(void)
{
    const int n = ls_fsk_capture_count();
    if (!n) {
        printf("lora: nothing captured - run 'lora fsk' first\n");
        return 1;
    }
    printf("lora: %d capture(s)\n", n);
    for (int i = 0; i < n; i++) {
        ls_fsk_capture_t c;
        if (ls_fsk_capture_get(i, &c)) capture_print(i, &c);
    }
    const uint32_t lost = ls_fsk_capture_evicted();
    if (lost) printf("lora: %lu older capture(s) dropped, the ring holds %d\n",
                     (unsigned long)lost, LS_FSK_CAPTURE_MAX);
    return 0;
}

static int capture_play_command(int argc, char **argv)
{
    if (argc < 3) {
        printf("lora fskplay <index> [dBm -9..22] [repeats 1-10]\n");
        return 1;
    }
    char *end;
    const long index = strtol(argv[2], &end, 10);
    if (end == argv[2] || *end) { printf("lora: bad index\n"); return 1; }
    long dbm = 14, repeats = 1;
    if (argc > 3) { dbm = strtol(argv[3], &end, 10);
                    if (end == argv[3] || *end) { printf("lora: bad dBm\n"); return 1; } }
    if (argc > 4) { repeats = strtol(argv[4], &end, 10);
                    if (end == argv[4] || *end) { printf("lora: bad repeats\n"); return 1; } }
    if (dbm < -9 || dbm > 22 || repeats < 1 || repeats > 10) {
        printf("lora: dBm -9..22, repeats 1-10\n");
        return 1;
    }

    ls_fsk_capture_t c;
    if (!ls_fsk_capture_get((int)index, &c)) {
        printf("lora: no capture %ld - 'lora fskls' lists them\n", index);
        return 1;
    }
    if (ls_lora_scanning() || ls_lora_fsk_active()) {
        printf("lora: radio busy\n");
        return 1;
    }

    const int64_t hold_deadline = esp_timer_get_time() + 1000000;
    while (!ls_mesh_radio_hold(true) && esp_timer_get_time() < hold_deadline)
        vTaskDelay(pdMS_TO_TICKS(10));
    if (!ls_mesh_radio_held()) {
        ls_mesh_radio_hold(false);
        printf("lora: radio busy\n");
        return 1;
    }

    ls_fsk_cfg_t cfg = {};
    cfg.freq_hz       = c.freq_hz;
    cfg.bitrate       = c.bitrate;
    cfg.deviation_hz  = c.deviation_hz;
    cfg.bandwidth_hz  = c.bandwidth_hz;
    cfg.sync_word     = c.sync_word;
    cfg.payload_bytes = c.len;
    cfg.preamble_bits = c.preamble_bits;
    cfg.power_dbm     = (int8_t)dbm;

    esp_err_t err = ls_lora_fsk_begin(&cfg);
    if (err == ESP_OK) {
        printf("lora: replay [%ld] %.4f MHz, %d dBm", index, c.freq_hz / 1e6,
               (int)dbm);
        if (repeats > 1) printf(", %ld times", repeats);
        printf("\n       ");
        for (uint8_t i = 0; i < c.len; i++) printf(" %02X", c.data[i]);
        printf("\n");

        for (long r = 0; r < repeats && err == ESP_OK; r++) {
            err = ls_lora_fsk_send(c.data, c.len);
            if (err != ESP_OK) break;
            const int64_t end_us = esp_timer_get_time() + 2000000;
            while (!ls_lora_send_done() && esp_timer_get_time() < end_us)
                vTaskDelay(pdMS_TO_TICKS(5));
            /* A receiver re-triggers on every frame it hears, so a train of
               them back to back holds a relay closed for the whole train.
               Spacing them is what makes repeats read as separate presses. */
            if (r + 1 < repeats) vTaskDelay(pdMS_TO_TICKS(250));
        }
        if (err == ESP_OK) printf("lora: sent\n");
        const esp_err_t ended = ls_lora_fsk_end();
        if (err == ESP_OK) err = ended;
    }
    ls_mesh_radio_hold(false);
    if (err != ESP_OK) printf("lora: %s\n", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int capture_save_command(int argc, char **argv)
{
    if (argc < 3) {
        printf("lora fsksave <index> [name]\n"
               "  writes /sdcard/subghz/<name>.sub - a Flipper RAW file with\n"
               "  a custom 2-FSK preset, so the SubGhz app can replay it\n");
        return 1;
    }
    char *end;
    const long index = strtol(argv[2], &end, 10);
    if (end == argv[2] || *end) { printf("lora: bad index\n"); return 1; }

    ls_fsk_capture_t c;
    if (!ls_fsk_capture_get((int)index, &c)) {
        printf("lora: no capture %ld - 'lora fskls' lists them\n", index);
        return 1;
    }

    char name[32];
    if (argc > 3) snprintf(name, sizeof(name), "%s", argv[3]);
    else          snprintf(name, sizeof(name), "fsk_%ld", index);
    /* A name that walks out of the directory, or ends the line in the file's
       own header, is not one this writes. */
    for (char *p = name; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') *p = '_';

    mkdir("/sdcard/subghz", 0777);   /* already there is not an error */

    char path[80];
    snprintf(path, sizeof(path), "/sdcard/subghz/%s.sub", name);

    const size_t want = ls_sub_fsk_render(&c, name, nullptr, 0);
    if (!want) { printf("lora: could not render that capture\n"); return 1; }
    char *text = (char *)heap_caps_malloc(want + 1,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!text) { printf("lora: no memory for %u bytes\n", (unsigned)want); return 1; }
    const size_t wrote = ls_sub_fsk_render(&c, name, text, want + 1);
    if (wrote != want) {
        heap_caps_free(text);
        printf("lora: render disagreed with itself\n");
        return 1;
    }

    /* Exclusive create: a silent overwrite of a capture somebody spent a
       field session collecting is not a convenience. */
    FILE *f = fopen(path, "wx");
    if (!f) {
        heap_caps_free(text);
        printf("lora: %s already exists, or the card is not writable\n", path);
        return 1;
    }
    const bool ok = fwrite(text, 1, want, f) == want && fflush(f) == 0 &&
                    !ferror(f);
    const bool closed = fclose(f) == 0;
    heap_caps_free(text);
    if (!ok || !closed) {
        unlink(path);
        printf("lora: write to %s did not complete - discarded\n", path);
        return 1;
    }

    printf("lora: %s, %u bytes\n", path, (unsigned)want);
    printf("  %.4f MHz, %u baud, %.1f kHz deviation, custom 2-FSK preset\n",
           c.freq_hz / 1e6, (unsigned)c.bitrate, c.deviation_hz / 1000.0);
    printf("  copy to the Flipper at /ext/subghz/ and play it from SubGhz\n");
    return 0;
}

int sx1262_capture_command(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "fskls"))   return capture_list_command();
    if (argc > 1 && !strcmp(argv[1], "fskplay")) return capture_play_command(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "fsksave")) return capture_save_command(argc, argv);
    return 1;
}
