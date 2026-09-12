#include "audio_out.h"
#include "audio_eq.h"
#include "tone.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "ls_audio_hw.h"
#include "ls_board.h"
#include "settings.h"
#include <math.h>

static const char *TAG = "audio_out";

#define RING_MS         600
#define PREBUF_MS       280
#define RING_BYTES      (AUDIO_RATE_HZ * RING_MS  / 1000 * (int)sizeof(int16_t))
#define PREBUF_BYTES    (AUDIO_RATE_HZ * PREBUF_MS / 1000 * (int)sizeof(int16_t))
#define CHUNK_FRAMES    256
#define IDLE_DROP_US    400000

static volatile int      s_volume = 35;
static volatile bool     s_muted  = false;
static bool              s_ready  = false;
#if defined(LS_BOARD_CODEC_I2C_BUS)
static bool              s_volume_applied = false;
#endif

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
static volatile bool     s_play_now    = false;
/* When live radio audio last reached the ring. FM and P25 voice both
   arrive through audio_write_mono; speech arrives through the blocking
   variant. All three push into the same stream buffer, so before this they
   interleaved chunk by chunk and came out as two voices over each other with
   static between them. P25 voice only started actually decoding once the
   HDU/ESS work landed, which is why speech had the speaker to itself until
   now. Live traffic wins; speech yields. */
static volatile int64_t  s_live_us     = 0;
static volatile uint32_t s_tts_yielded = 0;
#define AUDIO_LIVE_HOLD_US 300000

uint32_t audio_drops_get(void)     { return s_audio_drops; }
uint32_t audio_underruns_get(void) { return s_underruns; }
uint32_t audio_out_ring_avail(void){ return s_ring ? (uint32_t)xStreamBufferBytesAvailable(s_ring) : 0; }

void audio_out_play_now(void) { s_play_now = true; }

void IRAM_ATTR audio_write_mono(const int16_t *samples, int n)
{
    if (!s_ready || s_muted || n <= 0 || !s_ring) return;

    /* Live radio audio claims the speaker. */
    s_live_us = esp_timer_get_time();

    if (xSemaphoreTake(s_push_lock, 0) != pdTRUE) return;
    size_t want = (size_t)n * sizeof(int16_t);
    size_t sent = xStreamBufferSend(s_ring, samples, want, 0);
    xSemaphoreGive(s_push_lock);

    if (sent < want) s_audio_drops++;
}

/**/
bool audio_out_live_active(void)
{
    int64_t last = s_live_us;
    if (last == 0) return false;
    return (esp_timer_get_time() - last) < AUDIO_LIVE_HOLD_US;
}

uint32_t audio_out_tts_yielded(void) { return s_tts_yielded; }

void audio_write_mono_blocking(const int16_t *samples, int n)
{
    if (!s_ready || s_muted || n <= 0 || !s_ring) return;

    /* Do not start speaking over a live transmission. */
    if (audio_out_live_active()) { s_tts_yielded++; return; }

    const uint8_t *p = (const uint8_t *)samples;
    size_t remaining = (size_t)n * sizeof(int16_t);
    int64_t deadline = esp_timer_get_time() + 3000000;

    while (remaining > 0) {
        if (s_muted) break;
        /* A transmission that starts mid-utterance stops it, rather
           than letting the two share the ring. */
        if (audio_out_live_active()) { s_tts_yielded++; break; }
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
        ls_audio_hw_mute(false);
        float ph = 0.0f;
        const float dph = 2.0f * 3.14159265f * 1000.0f / (float)AUDIO_RATE_HZ;
        for (;;) {
            for (int i = 0; i < CHUNK_FRAMES; i++) {
                int16_t s = (int16_t)(6000.0f * sinf(ph));
                ph += dph; if (ph > 6.2831853f) ph -= 6.2831853f;
                stereo[i * 2] = s; stereo[i * 2 + 1] = s;
            }
            size_t wr = 0;
            ls_audio_hw_write(stereo, CHUNK_FRAMES * 4, &wr, portMAX_DELAY);
        }
    }
#endif

    for (;;) {
        if (s_reprime) {
            s_reprime = false;
            if (s_ring) xStreamBufferReset(s_ring);
            audio_eq_reset_state();
            playing = false;
        }
        if (!playing) {
            size_t avail = xStreamBufferBytesAvailable(s_ring);

            bool force = s_play_now && avail > 0;
            if (force) s_play_now = false;

            if (force || avail >= (size_t)PREBUF_BYTES) {
                playing = true;
                last_data = esp_timer_get_time();
            } else {
                size_t wr = 0;
                ls_audio_hw_write(silence, CHUNK_FRAMES * 2 * sizeof(int16_t),
                                    &wr, portMAX_DELAY);
                continue;
            }
        }

        size_t got = xStreamBufferReceive(s_ring, mono, CHUNK_FRAMES * sizeof(int16_t),
                                          pdMS_TO_TICKS(20));
        int frames = (int)(got / sizeof(int16_t));

        if (frames > 0) {
            last_data = esp_timer_get_time();
            audio_eq_process(mono, frames);
            for (int i = 0; i < frames; i++) {
                int16_t s = mono[i];
                stereo[i * 2] = s; stereo[i * 2 + 1] = s;
            }
            size_t wr = 0;
            ls_audio_hw_write(stereo, frames * 4, &wr, portMAX_DELAY);
        } else {

            if (esp_timer_get_time() - last_data > IDLE_DROP_US) {
                playing = false;
            } else {
                s_underruns++;
                size_t wr = 0;
                ls_audio_hw_write(silence, CHUNK_FRAMES * 2 * sizeof(int16_t),
                                    &wr, portMAX_DELAY);
            }
        }
    }
}

esp_err_t audio_out_init(void)
{
    if (s_ready) return ESP_OK;

    /* Gate here, not at the call sites. */

#if !LS_HAS_AUDIO
    ESP_LOGW(TAG, "no codec driver for this board - audio output disabled");
    return ESP_OK;
#else
    esp_err_t init_err = ls_audio_hw_init(false);
    if (init_err != ESP_OK) return init_err;

    esp_err_t err = ls_audio_hw_set_fs(AUDIO_RATE_HZ, 16, I2S_SLOT_MODE_STEREO);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "codec set_fs failed: %s (continuing)", esp_err_to_name(err));
    }

    s_volume = settings_get_volume();
    int set = 0;
    esp_err_t volume_err = ls_audio_hw_volume(s_volume, &set);
#if defined(LS_BOARD_CODEC_I2C_BUS)
    s_volume_applied = volume_err == ESP_OK;
#endif
    if (volume_err != ESP_OK)
        ESP_LOGW(TAG, "codec volume %d not confirmed: %s", s_volume, esp_err_to_name(volume_err));

    audio_eq_init(AUDIO_RATE_HZ);

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

    snd_test_init();

    s_ready = true;
    ESP_LOGI(TAG, "audio_out ready: ring=%dms prebuf=%dms vol=%d",
             RING_MS, PREBUF_MS, s_volume);
#if AUDIO_DIAG_TONE == 2
    ls_audio_hw_mute(false);
    xTaskCreatePinnedToCore(diag_ringtone_task, "diag_tone", 3072, NULL, 5, NULL, 1);
#endif
    return ESP_OK;
#endif /* LS_HAS_AUDIO */
}

void audio_toggle_mute(void)
{
    s_muted = !s_muted;
    if (s_muted && s_ring) xStreamBufferReset(s_ring);
    /* s_ready is false on a board whose codec was never initialised,
       and the handle behind this call is NULL there.  audio_out_ensure_unmuted
       already checked; this one did not, so the console's `mute` command was
       a reachable path into an uninitialised codec. */
    if (s_ready) ls_audio_hw_mute(s_muted);
}

void audio_out_ensure_unmuted(void)
{
    if (s_ready && !s_muted) ls_audio_hw_mute(false);
}

void audio_out_reset(void)
{
    if (!s_ready) return;

    esp_err_t err = ls_audio_hw_set_fs(AUDIO_RATE_HZ, 16, I2S_SLOT_MODE_STEREO);
    if (err != ESP_OK) ESP_LOGW(TAG, "reset set_fs: %s", esp_err_to_name(err));
    int set = 0;
    esp_err_t volume_err = ls_audio_hw_volume(s_volume, &set);
#if defined(LS_BOARD_CODEC_I2C_BUS)
    s_volume_applied = volume_err == ESP_OK;
#endif
    if (volume_err != ESP_OK)
        ESP_LOGW(TAG, "reset volume %d not confirmed: %s", s_volume, esp_err_to_name(volume_err));
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
#if defined(LS_BOARD_CODEC_I2C_BUS)
    /* Do not cache a failed codec write as an applied volume, or the
       equal-value shortcut prevents a later retry after the bus recovers. */
    if (v == s_volume && s_volume_applied) return;
    int set = 0;
    esp_err_t err = ls_audio_hw_volume(v, &set);
    s_volume_applied = err == ESP_OK;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "codec volume %d not confirmed: %s", v, esp_err_to_name(err));
        return;
    }
    s_volume = v;
#else
    if (v == s_volume) return;
    s_volume = v;
    int set = 0;
    ls_audio_hw_volume(s_volume, &set);
#endif
    settings_set_volume(s_volume);
}
