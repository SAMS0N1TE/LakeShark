
#include "audio_out.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "bsp_board_extra.h"
#include <math.h>

static const char *TAG = "audio_out";

#define RING_MS         256
#define PREBUF_MS       120
#define RING_BYTES      (AUDIO_RATE_HZ * RING_MS  / 1000 * (int)sizeof(int16_t))
#define PREBUF_BYTES    (AUDIO_RATE_HZ * PREBUF_MS / 1000 * (int)sizeof(int16_t))
#define CHUNK_FRAMES    256
#define IDLE_DROP_US    400000

static volatile int      s_volume = 35;
static volatile bool     s_muted  = false;
static bool              s_ready  = false;

#define AUDIO_TASK_STACK  4096

static StreamBufferHandle_t s_ring     = NULL;
static StaticStreamBuffer_t s_ring_ctrl;
static uint8_t             *s_ring_buf = NULL;
static SemaphoreHandle_t    s_push_lock = NULL;
static TaskHandle_t         s_task     = NULL;

static int16_t s_mono[CHUNK_FRAMES];
static int16_t s_stereo[CHUNK_FRAMES * 2];
static int16_t s_silence[CHUNK_FRAMES * 2];

static volatile uint32_t s_audio_drops = 0;
static volatile uint32_t s_underruns   = 0;
static volatile bool     s_reprime     = false;

uint32_t audio_drops_get(void)     { return s_audio_drops; }
uint32_t audio_underruns_get(void) { return s_underruns; }
uint32_t audio_out_ring_avail(void){ return s_ring ? (uint32_t)xStreamBufferBytesAvailable(s_ring) : 0; }

void IRAM_ATTR audio_write_mono(const int16_t *samples, int n)
{
    if (!s_ready || s_muted || n <= 0 || !s_ring) return;

    if (xSemaphoreTake(s_push_lock, 0) != pdTRUE) return;
    size_t want = (size_t)n * sizeof(int16_t);
    size_t sent = xStreamBufferSend(s_ring, samples, want, 0);
    xSemaphoreGive(s_push_lock);

    if (sent < want) s_audio_drops++;
}

void audio_write_mono_blocking(const int16_t *samples, int n)
{
    if (!s_ready || s_muted || n <= 0 || !s_ring) return;

    const uint8_t *p = (const uint8_t *)samples;
    size_t remaining = (size_t)n * sizeof(int16_t);
    int64_t deadline = esp_timer_get_time() + 3000000;

    while (remaining > 0) {
        if (s_muted) break;
        if (xSemaphoreTake(s_push_lock, pdMS_TO_TICKS(100)) != pdTRUE) break;
        size_t sent = xStreamBufferSend(s_ring, p, remaining, pdMS_TO_TICKS(100));
        xSemaphoreGive(s_push_lock);
        p += sent; remaining -= sent;
        if (esp_timer_get_time() > deadline) { s_audio_drops++; break; }
    }
}

void audio_write_p25_voice(const int16_t *src8k, int n)
{
    if (n <= 0) return;
    static int16_t up16k[1024];
    static int16_t prev = 0;
    int i = 0;
    while (i < n) {
        int chunk_in = n - i;
        if (chunk_in > 512) chunk_in = 512;
        for (int k = 0; k < chunk_in; k++) {
            int16_t s = src8k[i + k];
            up16k[k * 2]     = (int16_t)(((int)prev + (int)s) >> 1);
            up16k[k * 2 + 1] = s;
            prev = s;
        }
        audio_write_mono(up16k, chunk_in * 2);
        i += chunk_in;
    }
}

#define AUDIO_DIAG_TONE 0

#if AUDIO_DIAG_TONE == 2

static void diag_ringtone_task(void *arg)
{
    (void)arg;
    static int16_t buf[160];
    float ph = 0.0f;
    const float dph = 2.0f * 3.14159265f * 1000.0f / (float)AUDIO_RATE_HZ;
    for (;;) {
        for (int i = 0; i < 160; i++) {
            buf[i] = (int16_t)(6000.0f * sinf(ph));
            ph += dph; if (ph > 6.2831853f) ph -= 6.2831853f;
        }
        audio_write_mono(buf, 160);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
#endif

static void IRAM_ATTR audio_player_task(void *arg)
{
    (void)arg;
    int16_t *mono    = s_mono;
    int16_t *stereo  = s_stereo;
    int16_t *silence = s_silence;
    bool     playing   = false;
    int64_t  last_data = 0;

#if AUDIO_DIAG_TONE == 1
    {
        bsp_extra_codec_mute_set(false);
        float ph = 0.0f;
        const float dph = 2.0f * 3.14159265f * 1000.0f / (float)AUDIO_RATE_HZ;
        for (;;) {
            for (int i = 0; i < CHUNK_FRAMES; i++) {
                int16_t s = (int16_t)(6000.0f * sinf(ph));
                ph += dph; if (ph > 6.2831853f) ph -= 6.2831853f;
                stereo[i * 2] = s; stereo[i * 2 + 1] = s;
            }
            size_t wr = 0;
            bsp_extra_i2s_write(stereo, CHUNK_FRAMES * 4, &wr, portMAX_DELAY);
        }
    }
#endif

    for (;;) {
        if (s_reprime) {
            s_reprime = false;
            if (s_ring) xStreamBufferReset(s_ring);
            playing = false;   /* re-fill the prebuffer cleanly before output */
        }
        if (!playing) {

            if (xStreamBufferBytesAvailable(s_ring) >= (size_t)PREBUF_BYTES) {
                playing = true;
                last_data = esp_timer_get_time();
            } else {
                size_t wr = 0;
                bsp_extra_i2s_write(silence, CHUNK_FRAMES * 2 * sizeof(int16_t),
                                    &wr, portMAX_DELAY);
                continue;
            }
        }

        size_t got = xStreamBufferReceive(s_ring, mono, CHUNK_FRAMES * sizeof(int16_t),
                                          pdMS_TO_TICKS(20));
        int frames = (int)(got / sizeof(int16_t));

        if (frames > 0) {
            last_data = esp_timer_get_time();
            float vol = s_volume / 100.0f;
            for (int i = 0; i < frames; i++) {
                int16_t s = (int16_t)(mono[i] * vol);
                stereo[i * 2] = s; stereo[i * 2 + 1] = s;
            }
            size_t wr = 0;
            bsp_extra_i2s_write(stereo, frames * 4, &wr, portMAX_DELAY);
        } else {

            if (esp_timer_get_time() - last_data > IDLE_DROP_US) {
                playing = false;
            } else {
                s_underruns++;
                size_t wr = 0;
                bsp_extra_i2s_write(silence, CHUNK_FRAMES * 2 * sizeof(int16_t),
                                    &wr, portMAX_DELAY);
            }
        }
    }
}

esp_err_t audio_out_init(void)
{
    if (s_ready) return ESP_OK;

    bsp_extra_codec_init();

    esp_err_t err = bsp_extra_codec_set_fs(AUDIO_RATE_HZ, 16, I2S_SLOT_MODE_STEREO);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "codec set_fs failed: %s (continuing)", esp_err_to_name(err));
    }

    int set = 0;
    bsp_extra_codec_volume_set(s_volume, &set);

    s_push_lock = xSemaphoreCreateMutex();

    s_ring_buf = heap_caps_malloc(RING_BYTES + 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_ring_buf && s_push_lock) {
        s_ring = xStreamBufferCreateStatic(RING_BYTES, 1, s_ring_buf, &s_ring_ctrl);
    }
    if (!s_ring || !s_push_lock) {
        ESP_LOGE(TAG, "audio ring/lock alloc failed");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(audio_player_task, "audio_out",
                                            AUDIO_TASK_STACK, NULL, 11, &s_task, 1);
    if (ok != pdTRUE) {
        ESP_LOGE(TAG, "audio task create failed");
        return ESP_FAIL;
    }

    s_ready = true;
    ESP_LOGI(TAG, "audio_out ready: ring=%dms prebuf=%dms vol=%d",
             RING_MS, PREBUF_MS, s_volume);
#if AUDIO_DIAG_TONE == 2
    bsp_extra_codec_mute_set(false);
    xTaskCreatePinnedToCore(diag_ringtone_task, "diag_tone", 3072, NULL, 5, NULL, 1);
#endif
    return ESP_OK;
}

void audio_toggle_mute(void)
{
    s_muted = !s_muted;
    if (s_muted && s_ring) xStreamBufferReset(s_ring);
    bsp_extra_codec_mute_set(s_muted);
}

void audio_out_ensure_unmuted(void)
{
    if (s_ready && !s_muted) bsp_extra_codec_mute_set(false);
}

void audio_out_reset(void)
{
    if (!s_ready) return;
    /* Re-assert the codec sample rate and re-prime the output ring. Fixes the
     * first-P25-launch chop: the audio path could come up draining faster than
     * it is filled (continuous underruns) until a full reconfigure -- which
     * previously only the Settings audio control did -- corrected it. Calling
     * this on radio-app entry makes the first launch match that good state. */
    esp_err_t err = bsp_extra_codec_set_fs(AUDIO_RATE_HZ, 16, I2S_SLOT_MODE_STEREO);
    if (err != ESP_OK) ESP_LOGW(TAG, "reset set_fs: %s", esp_err_to_name(err));
    int set = 0;
    bsp_extra_codec_volume_set(s_volume, &set);
    s_reprime = true;
    ESP_LOGW(TAG, "audio_out_reset: set_fs=%s vol=%d ring_avail=%u",
             esp_err_to_name(err), s_volume, (unsigned)audio_out_ring_avail());
}

bool audio_is_muted(void) { return s_muted; }
int  audio_volume_get(void) { return s_volume; }

void audio_volume_delta(int d)
{
    audio_volume_set(s_volume + d);
}

void audio_volume_set(int v)
{
    if (v < 0)   v = 0;
    if (v > 100) v = 100;
    s_volume = v;
    int set = 0;
    bsp_extra_codec_volume_set(s_volume, &set);
}
