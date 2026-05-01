/*
 * sam_tts.c - SAM wrapper for ESP32-P4 ES8311.
 *
 * Pipeline per utterance:
 *   1. Take the tts_mutex so only one utterance renders at a time.
 *   2. Copy text into SAM's input buffer, append its terminator, run SAMMain().
 *   3. SAM fills the PSRAM buffer with 22050 Hz u8 PCM.
 *   4. Resample on the fly to the I2S rate with linear interpolation,
 *      convert u8→s16, push into audio_write_mono() a chunk at a time.
 *
 * Memory: one PSRAM buffer sized for ~4.5 s of 22050 Hz audio (100 KB).
 */
#include "sam_tts.h"
#include "sam/sam.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>

/* SAM's reciter - converts English text in-place to phonetic. Declared
 * here because reciter.h in the upstream tree only exposes a couple of
 * helpers, not this function. */
extern int TextToPhonemes(unsigned char *);

static const char *TAG = "sam_tts";

/* 7 seconds at SAM's native rate - the longest realistic utterance we
 * generate is "QUESTIONABLE. NEW CONTACT. ALFA ECHO ... UNKNOWN." at
 * ~5.8 s (measured with host-side SAM), so 7 s gives ~20% headroom.
 * Lives in PSRAM, which the Waveshare P4-NANO has in abundance. */
#define SAM_BUF_BYTES  (22050 * 7)       /* 154350 bytes */
#define SAM_NATIVE_HZ  22050

/* Resample ring: chunk pushed to I2S at a time. Sized to align with the
 * 512-frame push in audio_write_mono so every resampler fill maps to one
 * i2s_channel_write call — minimises per-call overhead jitter. */
#define CHUNK_SAMPLES  512

/* Lead-in and tail silence, in output-rate samples. A short pre-roll of
 * silence lets the I2S DMA re-fill before the first voiced sample arrives,
 * so the very first phoneme doesn't get clipped by an empty FIFO. A matching
 * post-roll guarantees the final sample has fully clocked out of DMA by the
 * time we return (i2s_channel_write returns as soon as the samples are
 * buffered, not played, so without post-roll the tail of the utterance
 * could be cut short if something else starts speaking immediately). */
#define LEAD_SILENCE_SAMPLES  (16000 / 32)   /* ~31 ms at 16 kHz */
#define TAIL_SILENCE_SAMPLES  (16000 / 16)   /* ~62 ms at 16 kHz */

static SemaphoreHandle_t tts_mutex = NULL;
static char             *sam_buf   = NULL;
static int               out_hz    = 16000;
static void            (*s_write)(const int16_t *, int) = NULL;
static bool              s_ready   = false;

/* -------- voice tweak state -------------------------------------------
 *
 * These globals drive the post-processing chain applied inside the
 * resample loop. They're written from the TUI (settings page) and read
 * from the audio task - since ints/enums are read atomically on RISC-V
 * we don't bother locking. Worst case a preset change applies partway
 * through an utterance, which is harmless.
 * --------------------------------------------------------------------- */

/* SAM's raw parameter table per named preset. Values derived by ear
 * from the SAM community's canonical preset list.
 *
 * Note on pitch: in SAM, pitch is a *period* value, so LARGER number =
 * LOWER perceived pitch. That's why "deep" voices have pitch=110, not 30. */
typedef struct {
    const char *name;
    uint8_t speed;
    uint8_t pitch;
    uint8_t mouth;
    uint8_t throat;
} sam_preset_t;

static const sam_preset_t PRESETS[SAM_PRESET_COUNT] = {
    [SAM_PRESET_DEFAULT] = { "DEFAULT",  72,  64, 128, 128 },
    [SAM_PRESET_ELVIS]   = { "ELVIS",    72, 110, 105, 110 },
    [SAM_PRESET_DEEP]    = { "DEEP",     80, 110, 200, 100 },
    [SAM_PRESET_SOFT]    = { "SOFT",     80,  80, 170, 110 },
    [SAM_PRESET_STUFFY]  = { "STUFFY",   72,  64, 110, 160 },
};

static sam_tts_voice_preset_t s_preset   = SAM_PRESET_DEFAULT;
static int                    s_lp_mode  = 0;   /* 0=off 1=soft 2=firm  */
static int                    s_sh_mode  = 0;   /* 0=off 1=warm 2=warmer*/

/* Filter state - persists ONLY within a single utterance, reset between.
 * Stored as Q15 fixed-point y-history for each one-pole filter.
 * int32_t so the intermediate products don't overflow. */
static int32_t s_lp_y    = 0;       /* low-pass y[n-1]                  */
static int32_t s_shelf_y = 0;       /* shelf's internal LP y[n-1]       */

/* One-pole IIR coefficients in Q15 (i.e. coef * 32768).
 *   y[n] = y[n-1] + ((x[n] - y[n-1]) * a_q15 >> 15)
 * is a fast fixed-point equivalent of:
 *   y[n] = (1-a) * y[n-1] + a * x[n].
 *
 * Values here are computed from a = 1 - exp(-2*pi*fc/fs), fs=16000 Hz:
 *   fc= 300 Hz -> a ≈ 0.1140 -> Q15=3735     (shelf split cutoff)
 *   fc=2500 Hz -> a ≈ 0.6264 -> Q15=20525    (firm LP)
 *   fc=4000 Hz -> a ≈ 0.7921 -> Q15=25955    (soft LP) */
#define A_Q15_SHELF  3735
#define A_Q15_LP_SOFT 25955
#define A_Q15_LP_FIRM 20525

/* Shelf gain (boost applied to the LP branch only). Q8 - so 256 = +0 dB,
 * 384 ≈ +3.5 dB, 512 = +6 dB. These stack on top of the existing signal,
 * so an "extra" of 128 in Q8 means "+50% of the LP content added back". */
#define SHELF_EXTRA_WARM    128   /* roughly +3 dB at the shelf cutoff  */
#define SHELF_EXTRA_WARMER  256   /* roughly +6 dB at the shelf cutoff  */

static void voice_apply_preset_to_sam(sam_tts_voice_preset_t p)
{
    if (p < 0 || p >= SAM_PRESET_COUNT) return;
    const sam_preset_t *pp = &PRESETS[p];
    SetSpeed (pp->speed);
    SetPitch (pp->pitch);
    SetMouth (pp->mouth);
    SetThroat(pp->throat);
}

void sam_tts_set_preset(sam_tts_voice_preset_t p)
{
    if (p < 0 || p >= SAM_PRESET_COUNT) return;
    s_preset = p;
    voice_apply_preset_to_sam(p);
}
sam_tts_voice_preset_t sam_tts_get_preset(void) { return s_preset; }
const char *sam_tts_preset_name(sam_tts_voice_preset_t p)
{
    if (p < 0 || p >= SAM_PRESET_COUNT) return "?";
    return PRESETS[p].name;
}

void sam_tts_set_lowpass(int mode)
{
    if (mode < 0 || mode > 2) mode = 0;
    s_lp_mode = mode;
}
int sam_tts_get_lowpass(void) { return s_lp_mode; }
const char *sam_tts_lowpass_name(int mode)
{
    switch (mode) {
        case 1: return "SOFT";
        case 2: return "FIRM";
        default: return "OFF";
    }
}

void sam_tts_set_lowshelf(int mode)
{
    if (mode < 0 || mode > 2) mode = 0;
    s_sh_mode = mode;
}
int sam_tts_get_lowshelf(void) { return s_sh_mode; }
const char *sam_tts_lowshelf_name(int mode)
{
    switch (mode) {
        case 1: return "WARM";
        case 2: return "WARMER";
        default: return "OFF";
    }
}

void sam_tts_test_speak(void)
{
    /* Short phrase that exercises a mix of vowels, fricatives and
     * plosives so the user can judge preset/filter effects meaningfully.
     * Kept under 3 seconds so repeated A/B is quick. */
    sam_tts_speak("TEST. THIS IS THE A D S B VOICE CHECK.");
}

esp_err_t sam_tts_init(int out_rate_hz,
                       void (*write_mono_fn)(const int16_t *samples, int n))
{
    if (s_ready) return ESP_OK;
    if (!write_mono_fn || out_rate_hz <= 0) return ESP_ERR_INVALID_ARG;

    /* Prefer PSRAM - the big buffer would hurt on-chip SRAM. Fall back to
     * internal if PSRAM isn't available (non-NANO board variants). */
    const char *alloc_where = "PSRAM";
    sam_buf = heap_caps_malloc(SAM_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!sam_buf) {
        ESP_LOGW(TAG, "PSRAM alloc failed, trying internal RAM");
        alloc_where = "SRAM";
        sam_buf = heap_caps_malloc(SAM_BUF_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!sam_buf) {
        ESP_LOGE(TAG, "Could not allocate %d byte SAM buffer", SAM_BUF_BYTES);
        return ESP_ERR_NO_MEM;
    }

    tts_mutex = xSemaphoreCreateMutex();
    if (!tts_mutex) {
        free(sam_buf);
        sam_buf = NULL;
        return ESP_ERR_NO_MEM;
    }

    sam_set_buffer(sam_buf);
    s_write = write_mono_fn;
    out_hz  = out_rate_hz;
    s_ready = true;

    /* Apply whatever preset was loaded from NVS before init was called
     * (or the DEFAULT if nothing was loaded yet). This seeds SAM's
     * speed/pitch/mouth/throat globals so the first utterance already
     * matches the stored user preference. */
    voice_apply_preset_to_sam(s_preset);

    ESP_LOGI(TAG, "SAM TTS ready - %d B in %s, resample %d -> %d Hz",
             SAM_BUF_BYTES, alloc_where, SAM_NATIVE_HZ, out_hz);
    return ESP_OK;
}

void sam_tts_set_voice(uint8_t speed, uint8_t pitch,
                       uint8_t mouth, uint8_t throat)
{
    SetSpeed(speed);
    SetPitch(pitch);
    SetMouth(mouth);
    SetThroat(throat);
}

/* Copy text into SAM's input buffer with its required terminator.
 * SAM's reciter expects uppercase English and a 0x9b (ESC) end marker. */
static void prepare_input(const char *text, unsigned char *out, size_t out_sz)
{
    size_t n = 0;
    /* leave 2 bytes for '[' reciter terminator and 0x9b + NUL just in case */
    size_t max_text = out_sz - 4;
    for (; text && *text && n < max_text; text++, n++) {
        unsigned char c = (unsigned char)*text;
        out[n] = (unsigned char)toupper(c);
    }
    /* SAM's reciter wants '[' as its end-of-text marker; main.c does strcat
     * with "[" before calling TextToPhonemes. */
    out[n++] = '[';
    out[n]   = 0;
}

void sam_tts_speak(const char *text)
{
    if (!s_ready || !text || !*text) return;

    /* Serialize utterances. If something is already speaking, drop this one
     * rather than queueing - we don't want a backlog of stale plane IDs. */
    if (xSemaphoreTake(tts_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        ESP_LOGD(TAG, "busy, dropping utterance: %s", text);
        return;
    }

    /* Render via SAM. Its globals are all static, so this is not re-entrant -
     * mutex above protects them. */
    unsigned char input_buf[256];
    prepare_input(text, input_buf, sizeof(input_buf));

    if (!TextToPhonemes(input_buf)) {
        ESP_LOGW(TAG, "TextToPhonemes failed for: %s", text);
        xSemaphoreGive(tts_mutex);
        return;
    }

    SetInput(input_buf);
    if (!SAMMain()) {
        ESP_LOGW(TAG, "SAMMain failed for: %s", text);
        xSemaphoreGive(tts_mutex);
        return;
    }

    /* SAM's bufferpos is the internal cycle count; actual u8 samples = /50.
     * Guard the divide so a runaway render can't overflow sam_buf. */
    int out_samples = GetBufferLength() / 50;
    if (out_samples <= 0) { xSemaphoreGive(tts_mutex); return; }
    if (out_samples > SAM_BUF_BYTES) out_samples = SAM_BUF_BYTES;

    const uint8_t *src = (const uint8_t *)GetBuffer();

    /* Linear resample 22050 -> out_hz, u8 unsigned -> s16 signed, into a
     * small stack chunk, then push to I2S. */
    int16_t chunk[CHUNK_SAMPLES];
    int      chunk_n = 0;

    /* --- Lead-in silence ---------------------------------------------------
     * Push a short run of zeros before the voice starts. This gives the I2S
     * DMA pipeline time to refill from idle (ES8311 is running at all times
     * at 16 kHz; between utterances the DMA buffer drains). Without this,
     * the very first ~10-20 ms of the first word gets eaten by the empty
     * FIFO and can sound clipped or glitchy. Samples are already zero
     * because `chunk` is stack-allocated but we memset explicitly so the
     * tail of the last utterance's buffer contents can't leak through. */
    memset(chunk, 0, sizeof(chunk));
    {
        int remaining = LEAD_SILENCE_SAMPLES;
        while (remaining > 0) {
            int push = remaining > CHUNK_SAMPLES ? CHUNK_SAMPLES : remaining;
            s_write(chunk, push);
            remaining -= push;
        }
    }

    /* Fixed-point phase accumulator (Q16). Step = 22050/out_hz. */
    uint64_t step = ((uint64_t)SAM_NATIVE_HZ << 16) / (uint32_t)out_hz;
    uint64_t phase = 0;

    /* The resampler output length is out_samples * out_hz / SAM_NATIVE_HZ. */
    int64_t out_total = (int64_t)out_samples * out_hz / SAM_NATIVE_HZ;

    /* Scale factor for u8 centered at 128 -> s16. Multiply the 0..255 delta
     * from centre (-128..+127) by 180 - leaves ~6 dB headroom above the raw
     * SAM bit-depth and keeps the ES8311 from clipping on peaks. When the
     * shelf boost is active we reduce this ceiling slightly so the +3/+6 dB
     * bump doesn't push peaks into the clip. */
    int gain = 180;
    if (s_sh_mode == 1) gain = 150;     /* -1.5 dB ceiling  */
    else if (s_sh_mode == 2) gain = 120; /* -3.5 dB ceiling */

    /* Pick the low-pass coefficient for this utterance. */
    int32_t a_lp = 0;
    if      (s_lp_mode == 1) a_lp = A_Q15_LP_SOFT;
    else if (s_lp_mode == 2) a_lp = A_Q15_LP_FIRM;

    /* Shelf boost in Q8 (256 = +0 dB, higher = more low-end added). */
    int32_t shelf_extra = 0;
    if      (s_sh_mode == 1) shelf_extra = SHELF_EXTRA_WARM;
    else if (s_sh_mode == 2) shelf_extra = SHELF_EXTRA_WARMER;

    /* Reset filter state at the start of each utterance. Leaving the old
     * y[n-1] alive across utterances would cause an initial transient as
     * the filters re-converge to the new signal. */
    s_lp_y    = 0;
    s_shelf_y = 0;

    for (int64_t i = 0; i < out_total; i++) {
        uint32_t idx  = (uint32_t)(phase >> 16);
        uint32_t frac = (uint32_t)(phase & 0xFFFF);
        phase += step;

        if ((int)idx >= out_samples - 1) {
            /* Tail - just duplicate the last sample so we don't read past. */
            idx = out_samples - 1;
            frac = 0;
        }

        int a = (int)src[idx]     - 128;
        int b = (int)src[idx + 1] - 128;
        /* Q16 linear interp: out = a + (b-a) * frac / 65536 */
        int lerp = a + (int)(((int64_t)(b - a) * (int64_t)frac) >> 16);
        int32_t s = lerp * gain;    /* signed, ~±23000 typical           */

        /* ---- Low-shelf boost ----
         * Decompose s into a low-passed branch (shelf_y) and its
         * complement (the high-passed residual). Adding a scaled copy
         * of the LP branch back onto s boosts low frequencies without
         * touching the highs. This is the cheapest way to add "body"
         * to a thin source, and doesn't introduce phase shift above
         * the crossover. */
        if (shelf_extra) {
            s_shelf_y += ((s - s_shelf_y) * A_Q15_SHELF) >> 15;
            s += (s_shelf_y * shelf_extra) >> 8;
        }

        /* ---- Low-pass ----
         * Standard one-pole IIR. Kills the 4-bit quantisation hash that
         * sits above ~4 kHz in SAM's raw output and makes it sound
         * harsh. Runs AFTER the shelf so the shelf's HP branch passes
         * through cleanly before the LP takes effect. */
        if (a_lp) {
            s_lp_y += ((s - s_lp_y) * a_lp) >> 15;
            s = s_lp_y;
        }

        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;

        chunk[chunk_n++] = (int16_t)s;
        if (chunk_n == CHUNK_SAMPLES) {
            s_write(chunk, chunk_n);
            chunk_n = 0;
        }
    }
    if (chunk_n > 0) s_write(chunk, chunk_n);

    /* --- Tail silence -----------------------------------------------------
     * Push zeros after the voice ends. i2s_channel_write buffers into DMA
     * and returns — the actual speaker output lags by roughly one DMA
     * buffer's worth of samples. Without a tail of silence, if the mutex
     * releases and a back-to-back utterance (or a beep tone) begins
     * immediately, it would either overlap the un-played tail of this one
     * or generate a discontinuity at the splice. The zero pad also lets
     * the ES8311's DC-blocking filter settle before the next event. */
    memset(chunk, 0, sizeof(chunk));
    {
        int remaining = TAIL_SILENCE_SAMPLES;
        while (remaining > 0) {
            int push = remaining > CHUNK_SAMPLES ? CHUNK_SAMPLES : remaining;
            s_write(chunk, push);
            remaining -= push;
        }
    }

    xSemaphoreGive(tts_mutex);
}
