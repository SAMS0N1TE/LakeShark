#include "audio_out.h"
#include "audio_eq.h"
#include "audio_pcm_ring.h"
#include "audio_speech_level.h"
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
#include <string.h>
#include <stdatomic.h>

static const char *TAG = "audio_out";

/* P25 voice held for proof is released in one piece - up to two LDUs held
   plus the LDU2 that proved the call, 540 ms - and the next LDU lands 180 ms
   later (p25_voice_hold.h). At 600 ms that left one frame of jitter before
   audio_write_mono dropped whole chunks. The ring is PSRAM; this costs no
   internal RAM. */
#define RING_MS         1000
#define PREBUF_MS       280
#define RING_BYTES      (AUDIO_RATE_HZ * RING_MS  / 1000 * (int)sizeof(int16_t))
#define PREBUF_BYTES    (AUDIO_RATE_HZ * PREBUF_MS / 1000 * (int)sizeof(int16_t))
#define CHUNK_FRAMES    256
#define IDLE_DROP_US    400000

static volatile int      s_volume = 35;
static volatile bool     s_muted  = false;
static bool              s_ready  = false;
static atomic_bool       s_media_request = false, s_media_owned = false;
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
/* In 1024 us ticks: 32 bits, so a read from the other core cannot tear. */
static volatile uint32_t s_live_tick   = 0;
static volatile bool     s_live_seen   = false;
static volatile uint32_t s_tts_yielded = 0;
#define AUDIO_LIVE_HOLD_TICKS (300000 / 1024)
#define AUDIO_NOW_TICK()      ((uint32_t)(esp_timer_get_time() >> 10))

/* Every byte into the ring is counted under s_push_lock, every byte out by
   the player alone, so a position in the ring is a byte count and
   differences stay right across wrap. Speech is discarded by moving
   s_skip_to past it: the player drops what it reads before that point. Only
   the player ever resets the ring itself. */
static volatile uint32_t s_wr_bytes    = 0;
static volatile uint32_t s_rd_bytes    = 0;
static volatile uint32_t s_speech_end  = 0;
static volatile uint32_t s_skip_to     = 0;
static volatile bool     s_speech_stop = false;
/* Speech still in the ring and its level, applied by the player after the
   equalizer (audio_speech_level.h). Written under s_push_lock; the player
   reads without it, and the worst a stale read does is play something
   quieter. */
static audio_speech_span_t s_speech_span;
static volatile int32_t    s_speech_gain = AUDIO_SPEECH_UNITY;

/* Codec reconfiguration is asked for here and done by the player between
   writes: closing the codec under a write in progress is not safe. */
static volatile uint32_t s_codec_reset_req  = 0;
static volatile uint32_t s_codec_reset_done = 0;

void audio_out_speech_level_set(int pct) { s_speech_gain = audio_speech_gain(pct); }

uint32_t audio_drops_get(void)     { return s_audio_drops; }
uint32_t audio_underruns_get(void) { return s_underruns; }
uint32_t audio_out_ring_avail(void){ return s_ring ? (uint32_t)xStreamBufferBytesAvailable(s_ring) : 0; }

void audio_out_play_now(void) { s_play_now = true; }

static inline size_t IRAM_ATTR ring_send_locked(const void *p, size_t want)
{
    /* A partial PCM16 sample shifts every following sample by one byte,
       turning otherwise valid decoded voice into sustained static. */
    const size_t whole = audio_pcm_write_bytes(want, xStreamBufferSpacesAvailable(s_ring));
    const size_t sent = whole ? xStreamBufferSend(s_ring, p, whole, 0) : 0;
    s_wr_bytes += (uint32_t)sent;
    return sent;
}

/* Speech into the ring: the same send, with the speech span marked first. */
static inline size_t ring_send_speech_locked(const void *p, size_t want)
{
    const size_t whole = audio_pcm_write_bytes(want, xStreamBufferSpacesAvailable(s_ring));
    if (!whole) return 0;
    audio_speech_span_add(&s_speech_span, s_rd_bytes, s_wr_bytes,
                          s_wr_bytes + (uint32_t)whole);
    const size_t sent = xStreamBufferSend(s_ring, p, whole, 0);
    s_wr_bytes += (uint32_t)sent;
    return sent;
}

void IRAM_ATTR audio_write_mono(const int16_t *samples, int n)
{
    if (s_media_request) return;
    if (!s_ready || s_muted || n <= 0 || !s_ring) return;

    /* Live radio audio claims the speaker. */
    s_live_tick = AUDIO_NOW_TICK();
    s_live_seen = true;

    if (xSemaphoreTake(s_push_lock, 0) != pdTRUE) return;
    /* Speech not yet played would make this transmission wait behind it. */
    if ((int32_t)(s_speech_end - s_rd_bytes) > 0 &&
        (int32_t)(s_speech_end - s_skip_to) > 0) {
        s_skip_to = s_speech_end;
        s_tts_yielded++;
    }
    const size_t want = (size_t)n * sizeof(int16_t);
    const size_t sent = ring_send_locked(samples, want);
    xSemaphoreGive(s_push_lock);

    if (sent < want) s_audio_drops++;
}

void audio_write_cue(const int16_t *samples, int n)
{
    if (s_media_request) return;
    if (!s_ready || s_muted || n <= 0 || !s_ring) return;
    if (xSemaphoreTake(s_push_lock, 0) != pdTRUE) return;
    const size_t want = (size_t)n * sizeof(int16_t);
    const size_t sent = ring_send_locked(samples, want);
    xSemaphoreGive(s_push_lock);
    if (sent < want) s_audio_drops++;
}

/**/
bool audio_out_live_active(void)
{
    if (!s_live_seen) return false;
    return (uint32_t)(AUDIO_NOW_TICK() - s_live_tick) < AUDIO_LIVE_HOLD_TICKS;
}

void audio_out_speech_begin(void) { s_speech_stop = false; }

void audio_out_speech_discard(void)
{
    s_speech_stop = true;
    if (!s_ready || !s_push_lock) return;
    if (xSemaphoreTake(s_push_lock, pdMS_TO_TICKS(50)) != pdTRUE) return;
    if ((int32_t)(s_speech_end - s_skip_to) > 0) s_skip_to = s_speech_end;
    xSemaphoreGive(s_push_lock);
}

uint32_t audio_out_tts_yielded(void) { return s_tts_yielded; }

bool audio_write_speech(const int16_t *samples, int n)
{
    if (!s_ready || n <= 0 || !s_ring) return false;

    const uint8_t *p = (const uint8_t *)samples;
    size_t remaining = (size_t)n * sizeof(int16_t);
    int64_t stalled_since = 0;

    while (remaining > 0) {
        if (s_muted || s_speech_stop || s_media_request) return false;
        /* A transmission, or one about to resume, has the speaker. */
        if (audio_out_live_active()) { s_tts_yielded++; return false; }

        size_t sent = 0;
        if (xSemaphoreTake(s_push_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
            /* Again under the lock: radio that arrived since the check above
               must not end up queued behind this. */
            if (audio_out_live_active()) {
                xSemaphoreGive(s_push_lock);
                s_tts_yielded++;
                return false;
            }
            if (s_speech_stop) {
                xSemaphoreGive(s_push_lock);
                return false;
            }
            sent = ring_send_speech_locked(p, remaining);
            if (sent) s_speech_end = s_wr_bytes;
            xSemaphoreGive(s_push_lock);
        }
        if (sent) {
            p += sent; remaining -= sent;
            stalled_since = 0;
            continue;
        }
        /* Waiting for room happens without the lock, so radio audio is
           never held up behind speech. */
        const int64_t now = esp_timer_get_time();
        if (!stalled_since) stalled_since = now;
        else if (now - stalled_since > 1000000) { s_audio_drops++; return false; }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
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

/* AUDIO_PREBUFFER_BEGIN: production policy exercised by bench/quality.py. */
static bool audio_prebuffer_ready(size_t available, bool force, int64_t now,
                                  int64_t *waiting_since)
{
    if (!available) { *waiting_since = -1; return false; }
    if (*waiting_since < 0) *waiting_since = now;
    if (force || available >= (size_t)PREBUF_BYTES ||
        now - *waiting_since >= (int64_t)PREBUF_MS * 1000) {
        *waiting_since = -1;
        return true;
    }
    return false;
}
/* AUDIO_PREBUFFER_END */

/* The player's helpers stay out of IRAM: internal RAM is the scarcest
   thing on this board, and the player's own stack is PSRAM anyway. */

static void __attribute__((noinline)) player_codec_reset(void)
{
    esp_err_t err = ls_audio_hw_set_fs(AUDIO_RATE_HZ, 16, I2S_SLOT_MODE_STEREO);
    if (err != ESP_OK) ESP_LOGW(TAG, "reset set_fs: %s", esp_err_to_name(err));
    int set = 0;
    esp_err_t volume_err = ls_audio_hw_volume(s_volume, &set);
#if defined(LS_BOARD_CODEC_I2C_BUS)
    s_volume_applied = volume_err == ESP_OK;
#endif
    if (volume_err != ESP_OK)
        ESP_LOGW(TAG, "reset volume %d not confirmed: %s", s_volume, esp_err_to_name(volume_err));
    /* Media closes muted. Restore the user's mute setting as well as rate
       and volume before returning this codec to radio/tone playback. */
    esp_err_t mute_err = ls_audio_hw_mute(s_muted);
    if (mute_err != ESP_OK) ESP_LOGW(TAG, "reset mute: %s", esp_err_to_name(mute_err));
    ESP_LOGW(TAG, "audio_out_reset: set_fs=%s vol=%d ring_avail=%u",
             esp_err_to_name(err), s_volume, (unsigned)audio_out_ring_avail());
}

/* Empties the ring. Producers never block on it, so the reset only fails
   if the lock cannot be had; then it is tried again next pass. */
static bool __attribute__((noinline)) player_reprime(void)
{
    if (xSemaphoreTake(s_push_lock, pdMS_TO_TICKS(20)) != pdTRUE) return false;
    xStreamBufferReset(s_ring);
    s_rd_bytes = s_wr_bytes;
    s_skip_to = s_wr_bytes;
    xSemaphoreGive(s_push_lock);
    audio_eq_reset_state();
    return true;
}

/* Drops the part of a chunk just read that lies before s_skip_to. Returns
   the bytes left to play, moved to the front of the chunk. */
static size_t __attribute__((noinline)) player_skip(int16_t *mono, size_t got,
                                                    uint32_t *pos)
{
    const uint32_t start = s_rd_bytes;
    s_rd_bytes = start + (uint32_t)got;
    const int32_t skip = (int32_t)(s_skip_to - start);
    *pos = start;
    if (skip <= 0) return got;
    if ((size_t)skip >= got) return 0;
    memmove(mono, (uint8_t *)mono + skip, got - (size_t)skip);
    *pos = start + (uint32_t)skip;
    return got - (size_t)skip;
}

/* A write that fails returns at once; without the pause a closed codec
   would have this priority-11 task spinning. */
static void __attribute__((noinline)) player_write(const void *buf, size_t bytes)
{
    size_t wr = 0;
    if (ls_audio_hw_write((void *)buf, bytes, &wr, portMAX_DELAY) != ESP_OK)
        vTaskDelay(pdMS_TO_TICKS(5));
}

static void IRAM_ATTR audio_player_task(void *arg)
{
    (void)arg;
    int16_t *mono    = s_mono;
    int16_t *stereo  = s_stereo;
    int16_t *silence = s_silence;
    bool     playing   = false;
    int64_t  last_data = 0;
    int64_t  waiting_since = -1;

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
        /* Acknowledge between hardware writes, never by suspending a task
           that may still hold the codec's lock. */
        if (s_media_request) {
            s_media_owned = true;
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        s_media_owned = false;
        const uint32_t reset_req = s_codec_reset_req;
        if (reset_req != s_codec_reset_done) {
            player_codec_reset();
            s_codec_reset_done = reset_req;
            s_reprime = true;
        }
        if (s_reprime && player_reprime()) {
            s_reprime = false;
            playing = false;
            waiting_since = -1;
        }
        if (!playing) {
            size_t avail = xStreamBufferBytesAvailable(s_ring);

            bool force = s_play_now && avail > 0;
            if (force) s_play_now = false;

            if (audio_prebuffer_ready(avail, force, esp_timer_get_time(),
                                      &waiting_since)) {
                playing = true;
                last_data = esp_timer_get_time();
            } else {
                player_write(silence, CHUNK_FRAMES * 2 * sizeof(int16_t));
                continue;
            }
        }

        size_t got = xStreamBufferReceive(s_ring, mono, CHUNK_FRAMES * sizeof(int16_t),
                                          pdMS_TO_TICKS(20));
        uint32_t pos = 0;
        if (got) {
            last_data = esp_timer_get_time();
            got = player_skip(mono, got, &pos);
            if (!got) continue;
        }
        int frames = (int)(got / sizeof(int16_t));

        if (frames > 0) {
            audio_eq_process(mono, frames);
            audio_speech_level_apply(&s_speech_span, pos, mono, frames, s_speech_gain);
            for (int i = 0; i < frames; i++) {
                int16_t s = mono[i];
                stereo[i * 2] = s; stereo[i * 2 + 1] = s;
            }
            player_write(stereo, (size_t)frames * 4);
        } else {

            if (esp_timer_get_time() - last_data > IDLE_DROP_US) {
                playing = false;
            } else {
                s_underruns++;
                player_write(silence, CHUNK_FRAMES * 2 * sizeof(int16_t));
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

    /* The stream payload is only touched from normal task context; keeping its
       queue in internal RAM needlessly competes with USB/I2S DMA and
       makes audio startup depend on heap contiguity after enumeration. */
    const size_t storage_bytes = audio_pcm_storage_bytes(RING_BYTES);
    s_ring_buf = heap_caps_malloc(storage_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_ring_buf && s_push_lock) {
        s_ring = xStreamBufferCreateStatic(storage_bytes, 1, s_ring_buf, &s_ring_ctrl);
    }
    if (!s_ring || !s_push_lock) {
        ESP_LOGE(TAG, "audio ring/lock alloc failed");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(audio_player_task, "audio_out",
                                                    AUDIO_TASK_STACK, NULL, 11,
                                                    &s_task, 1,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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
    if (s_muted) s_reprime = true;
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

void audio_out_reprime(void) { s_reprime = true; }

/* Waits, up to half a second, for the player to reconfigure the codec
   between two of its writes. */
void audio_out_reset(void)
{
    if (!s_ready) return;
    const uint32_t want = ++s_codec_reset_req;
    for (int i = 0; i < 100 && (int32_t)(s_codec_reset_done - want) < 0; i++)
        vTaskDelay(pdMS_TO_TICKS(5));
    if ((int32_t)(s_codec_reset_done - want) < 0)
        ESP_LOGW(TAG, "audio_out_reset: player has not taken the reset yet");
}

esp_err_t audio_out_media_acquire(void)
{
    if (!s_ready || s_media_request || s_media_owned) return ESP_ERR_INVALID_STATE;
    s_media_request = true;
    for (int i = 0; i < 100 && !s_media_owned; ++i) vTaskDelay(pdMS_TO_TICKS(5));
    if (s_media_owned) return ESP_OK;
    s_media_request = false;
    return ESP_ERR_TIMEOUT;
}

void audio_out_media_release(void)
{
    if (!s_media_request) return;
    ++s_codec_reset_req;
    s_reprime = true;
    s_media_request = false;
    /* Wait for the acknowledgement to clear before a subsequent acquire. */
    for (int i = 0; i < 100 && s_media_owned; ++i) vTaskDelay(pdMS_TO_TICKS(5));
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
