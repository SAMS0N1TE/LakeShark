#include "p25_p2_bench.h"
#include "p25_phase2.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { CAPACITY = 8192, CHUNK = 208, MAX_SYMBOLS = 2000000 };
typedef struct {
    uint8_t queue[CAPACITY];
    unsigned read, write, queued;
    uint32_t wacn;
    uint16_t system, nac;
    unsigned slot;
    bool finish;
    int64_t last_feed;
} replay_t;
typedef struct {
    p25p2_status_t decoder;
    uint64_t decode_us, energy;
    uint32_t accepted, max_us, samples, nonzero, peak, stack_free;
    bool failed;
} result_t;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static replay_t *active;
static result_t result;

static void pcm(const int16_t *samples, size_t count, void *context)
{
    result_t *r = context;
    r->samples += count;
    for (size_t i = 0; i < count; ++i) {
        int32_t v = samples[i];
        r->energy += (uint64_t)((int64_t)v * v);
        if (v) ++r->nonzero;
        uint32_t magnitude = (uint32_t)(v < 0 ? -v : v);
        if (magnitude > r->peak) r->peak = magnitude;
    }
}

static void worker(void *context)
{
    replay_t *q = context;
    result_t local = {0};
    p25p2_decoder_t *decoder = p25p2_create(pcm, &local);
    if (!decoder || !p25p2_configure(decoder, q->wacn, q->system, q->nac, q->slot))
        local.failed = true;
    while (!local.failed) {
        uint8_t block[CHUNK];
        portENTER_CRITICAL(&lock);
        unsigned count = q->queued < CHUNK ? q->queued : CHUNK;
        for (unsigned i = 0; i < count; ++i) {
            block[i] = q->queue[q->read];
            q->read = (q->read + 1) % CAPACITY;
        }
        q->queued -= count;
        bool finish = q->finish;
        int64_t last_feed = q->last_feed;
        portEXIT_CRITICAL(&lock);
        if (count) {
            int64_t start = esp_timer_get_time();
            p25p2_push(decoder, block, count);
            uint32_t elapsed = (uint32_t)(esp_timer_get_time() - start);
            local.decode_us += elapsed;
            if (elapsed > local.max_us) local.max_us = elapsed;
            p25p2_status(decoder, &local.decoder);
            local.stack_free = uxTaskGetStackHighWaterMark(NULL);
            portENTER_CRITICAL(&lock);
            local.accepted = result.accepted;
            result = local;
            portEXIT_CRITICAL(&lock);
        } else if (finish) break;
        if (esp_timer_get_time() - last_feed > 30000000LL) {
            local.failed = true;
            break;
        }
        vTaskDelay(1);
    }
    p25p2_destroy(decoder);
    portENTER_CRITICAL(&lock);
    local.accepted = result.accepted;
    result = local;
    active = NULL;
    portEXIT_CRITICAL(&lock);
    heap_caps_free(q);
    vTaskDeleteWithCaps(NULL);
}

static int hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int p25_p2_bench_command(int argc, char **argv)
{
    if (argc == 7 && !strcmp(argv[2], "begin")) {
        unsigned long ids[4];
        for (int i = 0; i < 4; ++i) {
            char *end;
            ids[i] = strtoul(argv[i + 3], &end, i == 3 ? 10 : 16);
            if (end == argv[i + 3] || *end) return 1;
        }
        if (ids[0] > 0xfffff || ids[1] > 0xfff || ids[2] > 0xfff || ids[3] < 1 || ids[3] > 2)
            return 1;
        portENTER_CRITICAL(&lock);
        bool busy = active != NULL;
        portEXIT_CRITICAL(&lock);
        if (busy) { puts("P2TEST BUSY"); return 1; }
        replay_t *q = heap_caps_calloc(1, sizeof(*q), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!q) { puts("P2TEST NO_MEMORY"); return 1; }
        q->wacn = ids[0]; q->system = ids[1]; q->nac = ids[2]; q->slot = ids[3] - 1;
        q->last_feed = esp_timer_get_time();
        portENTER_CRITICAL(&lock);
        memset(&result, 0, sizeof(result));
        active = q;
        portEXIT_CRITICAL(&lock);
        if (xTaskCreatePinnedToCoreWithCaps(worker, "p2_replay", 16384, q, 1, NULL, 1,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            portENTER_CRITICAL(&lock); active = NULL; portEXIT_CRITICAL(&lock);
            heap_caps_free(q); puts("P2TEST NO_TASK"); return 1;
        }
        puts("P2TEST READY"); return 0;
    }
    if (argc == 5 && !strcmp(argv[2], "data")) {
        char *end;
        unsigned long count = strtoul(argv[3], &end, 10);
        size_t length = strlen(argv[4]);
        if (end == argv[3] || *end || !count || count > CHUNK || length != ((count + 3) / 4) * 2)
            return 1;
        uint8_t unpacked[CHUNK];
        for (unsigned i = 0; i < count; ++i) {
            int a = hex(argv[4][(i / 4) * 2]), b = hex(argv[4][(i / 4) * 2 + 1]);
            if (a < 0 || b < 0) return 1;
            unpacked[i] = ((a * 16 + b) >> (6 - (i % 4) * 2)) & 3;
        }
        portENTER_CRITICAL(&lock);
        replay_t *q = active;
        bool ok = q && !q->finish && q->queued + count <= CAPACITY && result.accepted + count <= MAX_SYMBOLS;
        if (ok) {
            for (unsigned i = 0; i < count; ++i) {
                q->queue[q->write] = unpacked[i];
                q->write = (q->write + 1) % CAPACITY;
            }
            q->queued += count;
            q->last_feed = esp_timer_get_time();
            result.accepted += count;
        }
        portEXIT_CRITICAL(&lock);
        puts(ok ? "P2DATA OK" : "P2DATA BUSY");
        return ok ? 0 : 1;
    }
    if (argc == 3 && !strcmp(argv[2], "end")) {
        portENTER_CRITICAL(&lock);
        if (active) active->finish = true;
        portEXIT_CRITICAL(&lock);
    } else if (argc != 3 || strcmp(argv[2], "status")) {
        puts("p2 test begin WACN SYS NAC slot | data count packed_hex | end | status"); return 1;
    }
    portENTER_CRITICAL(&lock);
    result_t r = result;
    bool running = active != NULL;
    unsigned queued = active ? active->queued : 0;
    portEXIT_CRITICAL(&lock);
    printf("P2TEST run=%u failed=%u queued=%u accepted=%lu symbols=%lu voice=%lu audio=%lu muted=%lu samples=%lu nonzero=%lu peak=%lu energy=%llu decode_us=%llu max_us=%lu stack_free=%lu\n",
           running, r.failed, queued, (unsigned long)r.accepted, (unsigned long)r.decoder.symbols,
           (unsigned long)r.decoder.voice_frames, (unsigned long)r.decoder.audio_frames,
           (unsigned long)r.decoder.muted_frames, (unsigned long)r.samples, (unsigned long)r.nonzero,
           (unsigned long)r.peak, (unsigned long long)r.energy, (unsigned long long)r.decode_us,
           (unsigned long)r.max_us, (unsigned long)r.stack_free);
    return 0;
}
