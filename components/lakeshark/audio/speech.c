#include "speech.h"
#include "audio_out.h"
#include "settings.h"

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <string.h>

static const char *TAG = "speech";

/* 12.2 KB of parameter tracks per second of speech; 64 KB holds a
   five-second sentence. Longer sentences are split, not refused. */
#define SPEECH_ARENA_BYTES (64 * 1024)
/* The longest contact phrase peaked at 2.6 KB of stack on the board; `say`
   prints what is left. PSRAM: the worker never touches flash or NVS. */
#define SPEECH_STACK_BYTES 8192
#define SPEECH_QUEUE_LEN   4
#define SPEECH_PRIO        3
#define SPEECH_CORE        1

typedef struct {
    char     text[SPEECH_TEXT_MAX];
    uint32_t gen;
    uint32_t id;      /* nonzero for a caller waiting in speech_say() */
} speech_req_t;

typedef struct {
    int64_t first_us;
    int64_t sink_us;
    bool    stopped;
    uint32_t gen;
} speech_job_t;

static QueueHandle_t     s_q;
static TaskHandle_t      s_task;
static SemaphoreHandle_t s_sync_lock;
static SemaphoreHandle_t s_done;
static uint8_t          *s_arena;
static volatile bool     s_ready;
static volatile uint32_t s_gen;
static uint32_t          s_next_id;
static volatile uint32_t s_done_id;
static volatile speech_voice_t  s_voice = SPEECH_VOICE_GLITCH;
static volatile int              s_volume = 100;
static volatile speech_result_t s_sync_result;
static speech_stats_t    s_stats;

static bool speech_sink(const int16_t *pcm, int n, void *ctx)
{
    speech_job_t *j = (speech_job_t *)ctx;
    if (j->gen != __atomic_load_n(&s_gen, __ATOMIC_ACQUIRE)) { j->stopped = true; return false; }

    /* The speech level is not applied here: audio_out applies it after the
       equalizer, whose leveler would otherwise undo it. */
    const int64_t t0 = esp_timer_get_time();
    const bool ok = audio_write_speech(pcm, n);
    const int64_t t1 = esp_timer_get_time();
    j->sink_us += t1 - t0;
    if (!ok) { j->stopped = true; return false; }
    if (!j->first_us) j->first_us = t1;
    return true;
}

static speech_result_t speak_one(const speech_req_t *r)
{
    if (r->gen != __atomic_load_n(&s_gen, __ATOMIC_ACQUIRE)) {
        s_stats.stopped++;
        return SPEECH_STOPPED;
    }

    audio_out_speech_begin();
    speech_job_t job = { .gen = r->gen };
    speech_engine_stats_t st;
    const int64_t t0 = esp_timer_get_time();
    const int rc = speech_engine_say(s_voice, r->text, s_arena, SPEECH_ARENA_BYTES,
                                     speech_sink, &job, &st);
    const int64_t total = esp_timer_get_time() - t0;

    const uint32_t first_ms = job.first_us ? (uint32_t)((job.first_us - t0) / 1000) : 0;
    const uint32_t synth_ms = (uint32_t)((total - job.sink_us) / 1000);
    s_stats.last_first_ms = first_ms;
    s_stats.last_synth_ms = synth_ms;
    s_stats.last_audio_ms = st.samples / (SPEECH_RATE_HZ / 1000);
    s_stats.last_units    = st.units;
    if (first_ms > s_stats.max_first_ms) s_stats.max_first_ms = first_ms;
    if (synth_ms > s_stats.max_synth_ms) s_stats.max_synth_ms = synth_ms;
    if (st.arena_peak > s_stats.arena_peak) s_stats.arena_peak = st.arena_peak;

    if (rc == SPEECH_ENGINE_OK) { s_stats.spoken++; return SPEECH_SPOKEN; }
    if (rc == SPEECH_ENGINE_STOPPED || job.stopped) { s_stats.stopped++; return SPEECH_STOPPED; }
    s_stats.failed++;
    s_stats.last_error = rc;
    ESP_LOGW(TAG, "not spoken (%d): \"%s\"", rc, r->text);
    return SPEECH_FAILED;
}

static void speech_task(void *arg)
{
    (void)arg;
    static speech_req_t r;
    for (;;) {
        if (xQueueReceive(s_q, &r, portMAX_DELAY) != pdTRUE) continue;
        const speech_result_t res = speak_one(&r);
        if (r.id) {
            s_sync_result = res;
            s_done_id = r.id;
            xSemaphoreGive(s_done);
        }
    }
}

esp_err_t speech_init(void)
{
    if (s_ready) return ESP_OK;

    /* Everything below is PSRAM; this proves it stays that way. */
    const size_t internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const int rc = speech_engine_init();
    if (rc != SPEECH_ENGINE_OK) {
        ESP_LOGW(TAG, "engine init failed (%d); speech off", rc);
        return ESP_ERR_NO_MEM;
    }
    s_arena = heap_caps_malloc(SPEECH_ARENA_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_q = xQueueCreateWithCaps(SPEECH_QUEUE_LEN, sizeof(speech_req_t),
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_sync_lock = xSemaphoreCreateMutexWithCaps(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_done = xSemaphoreCreateBinaryWithCaps(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_arena || !s_q || !s_sync_lock || !s_done ||
        xTaskCreatePinnedToCoreWithCaps(speech_task, "speech", SPEECH_STACK_BYTES, NULL,
                                        SPEECH_PRIO, &s_task, SPEECH_CORE,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdTRUE) {
        ESP_LOGW(TAG, "no memory for speech; speech off");
        if (s_done) vSemaphoreDeleteWithCaps(s_done);
        if (s_sync_lock) vSemaphoreDeleteWithCaps(s_sync_lock);
        if (s_q) vQueueDeleteWithCaps(s_q);
        heap_caps_free(s_arena);
        s_done = s_sync_lock = NULL;
        s_q = NULL;
        s_arena = NULL;
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_stats.arena_bytes = SPEECH_ARENA_BYTES;
    s_stats.stack_bytes = SPEECH_STACK_BYTES;
    s_ready = true;
    ESP_LOGI(TAG, "ready: voice %s, arena %u B, internal RAM used %d B",
             speech_voice_name(s_voice), (unsigned)SPEECH_ARENA_BYTES,
             (int)internal_before - (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return ESP_OK;
}

bool speech_available(void) { return s_ready; }

/* Copies the text, cutting at the last space that fits. */
static bool fill_req(speech_req_t *r, const char *text)
{
    if (!text || !*text) return false;
    size_t n = strlen(text);
    if (n >= sizeof(r->text)) {
        n = sizeof(r->text) - 1;
        while (n > 0 && text[n] != ' ') n--;
        if (n == 0) return false;
    }
    memcpy(r->text, text, n);
    r->text[n] = 0;
    r->gen = __atomic_load_n(&s_gen, __ATOMIC_ACQUIRE);
    r->id = 0;
    return true;
}

speech_result_t speech_say(const char *text)
{
    if (!s_ready) return SPEECH_UNAVAILABLE;
    speech_req_t r;
    if (!fill_req(&r, text)) return SPEECH_FAILED;

    /* One waiter at a time; a completion is matched to its request by id,
       so one left behind by a caller that gave up is not taken as this. */
    xSemaphoreTake(s_sync_lock, portMAX_DELAY);
    if (++s_next_id == 0) s_next_id = 1;
    r.id = s_next_id;
    speech_result_t res = SPEECH_FAILED;
    if (xQueueSend(s_q, &r, pdMS_TO_TICKS(200)) != pdTRUE) {
        s_stats.busy++;
        res = SPEECH_BUSY;
    } else {
        const TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(30000);
        for (;;) {
            const TickType_t now = xTaskGetTickCount();
            if ((int32_t)(until - now) <= 0) break;
            if (xSemaphoreTake(s_done, until - now) != pdTRUE) break;
            if (s_done_id == r.id) { res = s_sync_result; break; }
        }
    }
    xSemaphoreGive(s_sync_lock);
    return res;
}

speech_result_t speech_say_async(const char *text)
{
    if (!s_ready) return SPEECH_UNAVAILABLE;
    speech_req_t r;
    if (!fill_req(&r, text)) return SPEECH_FAILED;
    if (xQueueSend(s_q, &r, 0) != pdTRUE) {
        s_stats.busy++;
        return SPEECH_BUSY;
    }
    return SPEECH_SPOKEN;
}

void speech_cancel(void)
{
    __atomic_add_fetch(&s_gen, 1, __ATOMIC_RELEASE);
    audio_out_speech_discard();
}

speech_voice_t speech_voice_get(void) { return s_voice; }

void speech_voice_set(speech_voice_t v)
{
    if ((int)v < 0 || v >= SPEECH_VOICE_COUNT) v = SPEECH_VOICE_GLITCH;
    s_voice = v;
}

int  speech_volume_get(void) { return s_volume; }

void speech_volume_set(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    s_volume = pct;
    audio_out_speech_level_set(pct);
}

speech_voice_t speech_voice_step(int dir)
{
    const int n = SPEECH_VOICE_COUNT;
    const speech_voice_t v = (speech_voice_t)((((int)s_voice + dir) % n + n) % n);
    speech_voice_set(v);
    settings_speech_voice_set((int)v);
    return v;
}

void speech_stats_get(speech_stats_t *out)
{
    if (!out) return;
    *out = s_stats;
    out->available = s_ready;
    out->stack_unused = s_task ? (unsigned)uxTaskGetStackHighWaterMark(s_task) : 0;
}
