#include "sx1262_console.h"
#include "ls_lora.h"
#include "ls_mesh.h"
#include "pocsag.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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
        const int64_t end = esp_timer_get_time() + (int64_t)(seconds * 1e6);
        int64_t previous = 0;
        unsigned packets = 0, errors = 0;
        uint8_t packet[255];
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
                    printf("\n");
                }
                previous = now;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }
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
