
#include "audio_out.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "bsp_board_extra.h"
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

typedef struct { float b0, b1, b2, a1, a2; float z1, z2; } biquad_t;

#define EQ_BASS_HZ    120.0f
#define EQ_MID_HZ    1500.0f
#define EQ_TREBLE_HZ 3500.0f

static biquad_t     s_hpf, s_bass, s_mid, s_treble;
static volatile bool s_eq_on = true;
static float        s_eq_hpf_hz    = 250.0f;
static float        s_eq_bass_db   = 0.0f;
static float        s_eq_mid_db    = 2.0f;
static float        s_eq_treble_db = 2.0f;
static bool         s_eq_ready     = false;

static void biquad_hpf(biquad_t *bq, float fs, float f0, float q)
{
    float w0 = 2.0f * (float)M_PI * f0 / fs;
    float c = cosf(w0), s = sinf(w0);
    float alpha = s / (2.0f * q);
    float a0 = 1.0f + alpha;
    bq->b0 = ((1.0f + c) * 0.5f) / a0;
    bq->b1 = (-(1.0f + c)) / a0;
    bq->b2 = ((1.0f + c) * 0.5f) / a0;
    bq->a1 = (-2.0f * c) / a0;
    bq->a2 = (1.0f - alpha) / a0;
}

static void biquad_peak(biquad_t *bq, float fs, float f0, float q, float gain_db)
{
    float A = powf(10.0f, gain_db / 40.0f);
    float w0 = 2.0f * (float)M_PI * f0 / fs;
    float c = cosf(w0), s = sinf(w0);
    float alpha = s / (2.0f * q);
    float a0 = 1.0f + alpha / A;
    bq->b0 = (1.0f + alpha * A) / a0;
    bq->b1 = (-2.0f * c) / a0;
    bq->b2 = (1.0f - alpha * A) / a0;
    bq->a1 = (-2.0f * c) / a0;
    bq->a2 = (1.0f - alpha / A) / a0;
}

static void biquad_lowshelf(biquad_t *bq, float fs, float f0, float gain_db)
{
    float A = powf(10.0f, gain_db / 40.0f);
    float w0 = 2.0f * (float)M_PI * f0 / fs;
    float c = cosf(w0), s = sinf(w0);
    float alpha = s * 0.70710678f;
    float ap1 = A + 1.0f, am1 = A - 1.0f, ta = 2.0f * sqrtf(A) * alpha;
    float a0 = ap1 + am1 * c + ta;
    bq->b0 = (A * (ap1 - am1 * c + ta)) / a0;
    bq->b1 = (2.0f * A * (am1 - ap1 * c)) / a0;
    bq->b2 = (A * (ap1 - am1 * c - ta)) / a0;
    bq->a1 = (-2.0f * (am1 + ap1 * c)) / a0;
    bq->a2 = (ap1 + am1 * c - ta) / a0;
}

static void biquad_highshelf(biquad_t *bq, float fs, float f0, float gain_db)
{
    float A = powf(10.0f, gain_db / 40.0f);
    float w0 = 2.0f * (float)M_PI * f0 / fs;
    float c = cosf(w0), s = sinf(w0);
    float alpha = s * 0.70710678f;
    float ap1 = A + 1.0f, am1 = A - 1.0f, ta = 2.0f * sqrtf(A) * alpha;
    float a0 = ap1 - am1 * c + ta;
    bq->b0 = (A * (ap1 + am1 * c + ta)) / a0;
    bq->b1 = (-2.0f * A * (am1 + ap1 * c)) / a0;
    bq->b2 = (A * (ap1 + am1 * c - ta)) / a0;
    bq->a1 = (2.0f * (am1 - ap1 * c)) / a0;
    bq->a2 = (ap1 - am1 * c - ta) / a0;
}

static inline float biquad_run(biquad_t *bq, float x)
{
    float y = bq->b0 * x + bq->z1;
    bq->z1 = bq->b1 * x - bq->a1 * y + bq->z2;
    bq->z2 = bq->b2 * x - bq->a2 * y;
    return y;
}

static void eq_recompute(void)
{
    float fs = (float)AUDIO_RATE_HZ;
    biquad_hpf(&s_hpf, fs, s_eq_hpf_hz, 0.7071f);
    biquad_lowshelf(&s_bass, fs, EQ_BASS_HZ, s_eq_bass_db);
    biquad_peak(&s_mid, fs, EQ_MID_HZ, 0.9f, s_eq_mid_db);
    biquad_highshelf(&s_treble, fs, EQ_TREBLE_HZ, s_eq_treble_db);
    s_eq_ready = true;
}

static inline float eq_chain(float x)
{
    x = biquad_run(&s_hpf, x);
    x = biquad_run(&s_bass, x);
    x = biquad_run(&s_mid, x);
    x = biquad_run(&s_treble, x);
    return x;
}

static inline int16_t clip16(float v)
{
    if (v >  32767.0f) return  32767;
    if (v < -32768.0f) return -32768;
    return (int16_t)v;
}

void audio_write_p25_voice(const int16_t *src8k, int n)
{
    if (n <= 0) return;
    if (!s_eq_ready) eq_recompute();

    static int16_t up16k[1024];
    static float r0 = 0, r1 = 0, r2 = 0, r3 = 0;

    bool eq = s_eq_on;
    int i = 0;
    while (i < n) {
        int chunk_in = n - i;
        if (chunk_in > 512) chunk_in = 512;
        int o = 0;
        for (int k = 0; k < chunk_in; k++) {
            r0 = r1; r1 = r2; r2 = r3; r3 = (float)src8k[i + k];
            float e  = r1;
            float od = (-r0 + 9.0f * r1 + 9.0f * r2 - r3) * (1.0f / 16.0f);
            if (eq) {
                e  = eq_chain(e);
                od = eq_chain(od);
            }
            up16k[o++] = clip16(e);
            up16k[o++] = clip16(od);
        }
        audio_write_mono(up16k, o);
        i += chunk_in;
    }
}

void audio_voice_eq_enable(bool on) { s_eq_on = on; }
bool audio_voice_eq_enabled(void)   { return s_eq_on; }

void audio_voice_eq_set_hpf(float hz)
{
    if (hz < 20.0f)   hz = 20.0f;
    if (hz > 1000.0f) hz = 1000.0f;
    s_eq_hpf_hz = hz;
    eq_recompute();
}

static float clamp_db(float db)
{
    if (db < -12.0f) return -12.0f;
    if (db >  12.0f) return  12.0f;
    return db;
}

void audio_voice_eq_set_bass(float db)   { s_eq_bass_db   = clamp_db(db); eq_recompute(); }
void audio_voice_eq_set_mid(float db)    { s_eq_mid_db    = clamp_db(db); eq_recompute(); }
void audio_voice_eq_set_treble(float db) { s_eq_treble_db = clamp_db(db); eq_recompute(); }

float audio_voice_eq_hpf(void)       { return s_eq_hpf_hz; }
float audio_voice_eq_bass_db(void)   { return s_eq_bass_db; }
float audio_voice_eq_mid_db(void)    { return s_eq_mid_db; }
float audio_voice_eq_treble_db(void) { return s_eq_treble_db; }

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
            playing = false;
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

    s_volume = settings_get_volume();
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
    int set = 0;
    bsp_extra_codec_volume_set(s_volume, &set);
    s_reprime = true;
    ESP_LOGW(TAG, "audio_out_reset: reprime vol=%d ring_avail=%u",
             s_volume, (unsigned)audio_out_ring_avail());
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
    if (v == s_volume) return;
    s_volume = v;
    int set = 0;
    bsp_extra_codec_volume_set(s_volume, &set);
    settings_set_volume(s_volume);
}
